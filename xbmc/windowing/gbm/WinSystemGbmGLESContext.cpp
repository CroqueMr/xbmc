/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbmGLESContext.h"

#include "OptionalsReg.h"
#include "cores/RetroPlayer/process/gbm/RPProcessInfoGbm.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererDMAOpenGLES.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.h"
#include "cores/VideoPlayer/Process/gbm/ProcessInfoGBM.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererDRMPRIMEGLES.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"

extern "C"
{
#include <libavutil/pixfmt.h>
#ifdef HAVE_DVBRIDGE
#include "dvbridge_placebo.h"
#endif
}
#include "utils/BufferObjectFactory.h"
#include "utils/DMAHeapBufferObject.h"
#include "utils/DumbBufferObject.h"
#include "utils/GBMBufferObject.h"
#include "utils/UDMABufferObject.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/WindowSystemFactory.h"

#include <mutex>
#include <cstdlib>
#include <cstring>
#ifdef HAVE_DVBRIDGE
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackPolicy.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "drm/DRMAtomic.h"
#endif

#include <gbm.h>

using namespace KODI::WINDOWING::GBM;

using namespace std::chrono_literals;

CWinSystemGbmGLESContext::CWinSystemGbmGLESContext()
  : CWinSystemGbmEGLContext(EGL_PLATFORM_GBM_MESA, "EGL_MESA_platform_gbm")
{
}

void CWinSystemGbmGLESContext::Register()
{
  CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "gbm");
}

std::unique_ptr<CWinSystemBase> CWinSystemGbmGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemGbmGLESContext>();
}

bool CWinSystemGbmGLESContext::InitWindowSystem()
{
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CDVDFactoryCodec::ClearHWAccels();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoGbm::Register();
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryDMAOpenGLES);
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);

  // GLES 3.0 required for 10-bit texture format support (GL_RGB10_A2, GL_R16UI, etc.)
  // Fall back to GLES 2.0 for devices that don't support GLES 3.0 (e.g. lima driver)
  if (!CWinSystemGbmEGLContext::InitWindowSystemEGL(EGL_OPENGL_ES3_BIT, EGL_OPENGL_ES_API))
  {
    if (!CWinSystemGbmEGLContext::InitWindowSystemEGL(EGL_OPENGL_ES2_BIT, EGL_OPENGL_ES_API))
    {
      return false;
    }
  }

  bool general, deepColor;
  m_vaapiProxy.reset(GBM::VaapiProxyCreate(m_DRM->GetRenderNodeFileDescriptor()));
  GBM::VaapiProxyConfig(m_vaapiProxy.get(), m_eglContext.GetEGLDisplay());
  GBM::VAAPIRegisterRenderGLES(m_vaapiProxy.get(), general, deepColor);

  if (general)
  {
    GBM::VAAPIRegister(m_vaapiProxy.get(), deepColor);
  }

  CRendererDRMPRIMEGLES::Register();
  CRendererDRMPRIME::Register();
  CDVDVideoCodecDRMPRIME::Register();
  VIDEOPLAYER::CProcessInfoGBM::Register();

  CScreenshotSurfaceGLES::Register();

  CBufferObjectFactory::ClearBufferObjects();
  CDumbBufferObject::Register();
#if defined(HAS_GBM_BO_MAP)
  CGBMBufferObject::Register();
#endif
#if defined(HAVE_LINUX_MEMFD) && defined(HAVE_LINUX_UDMABUF)
  CUDMABufferObject::Register();
#endif
#if defined(HAVE_LINUX_DMA_HEAP)
  CDMAHeapBufferObject::Register();
#endif

  return true;
}

bool CWinSystemGbmGLESContext::SetFullScreen(bool fullScreen,
                                             RESOLUTION_INFO& res,
                                             bool blankOtherDisplays)
{
#ifdef HAVE_DVBRIDGE
  if (!EndDVBridge())
    return false;
  m_dvBridge.reset();
#endif
  if (res.iWidth != m_nWidth || res.iHeight != m_nHeight)
  {
    CLog::Log(LOGDEBUG, "CWinSystemGbmGLESContext::{} - resolution changed, creating a new window",
              __FUNCTION__);
    CreateNewWindow("", fullScreen, res);
  }

  if (!m_eglContext.TrySwapBuffers())
  {
    CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
    throw std::runtime_error("eglSwapBuffers failed");
  }

  CWinSystemGbm::SetFullScreen(fullScreen, res, blankOtherDisplays);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);

  return true;
}

void CWinSystemGbmGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  if (!m_bRenderCreated)
    return;

#ifdef HAVE_DVBRIDGE
  if (m_hdrConversion)
  {
    bool presented = false;
    if (m_dvFrame && m_dvBridge && m_dvBridge->ResolveHDR10(UseLimitedColor()))
    {
      glFinish();
      if (m_eglContext.TrySwapBuffers())
      {
        auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        auto* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get();
        presented = drm && bo &&
            drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR", 1) &&
            drm->FlipPageTransactional(bo);
      }
    }
    if (m_dvBridge)
      m_dvBridge->Presented(presented);
    m_dvFrame = false;
    return;
  }
  if (m_dvState.Captured())
  {
    bool presented = false;
    // EGL's window framebuffer has a bottom-left GL origin, while transport
    // metadata is indexed from the top-left of the HDMI raster.
    if (m_dvFrame && m_dvBridge && m_dvBridge->Pack(0, m_nWidth, m_nHeight, true))
    {
      glFinish();
      if (m_eglContext.TrySwapBuffers())
      {
        presented = FlipDVBridgePage(m_dvState, false);
      }
    }
    if (m_dvBridge)
      m_dvBridge->Presented(presented);
    m_dvFrame = false;
    return;
  }
#endif

  if (rendered || videoLayer)
  {
    bool async = !videoLayer && m_eglFence;
    if (rendered)
    {
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
      if (async)
      {
        int fd = m_DRM->TakeOutFenceFd();
        if (fd != -1)
        {
          m_eglFence->CreateKMSFence(fd);
          m_eglFence->WaitSyncGPU();
        }

        m_eglFence->CreateGPUFence();
      }
#endif

      if (!m_eglContext.TrySwapBuffers())
      {
        CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
        throw std::runtime_error("eglSwapBuffers failed");
      }

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
      if (async)
      {
        int fd = m_eglFence->FlushFence();
        m_DRM->SetInFenceFd(fd);

        m_eglFence->WaitSyncCPU();
      }
#endif
    }

    CWinSystemGbm::FlipPage(rendered, videoLayer, async);
  }
  else
  {
    KODI::TIME::Sleep(10ms);
  }

  if (m_dispReset && m_dispResetTimer.IsTimePast())
  {
    CLog::Log(LOGDEBUG, "CWinSystemGbmGLESContext::{} - Sending display reset to all clients",
              __FUNCTION__);
    m_dispReset = false;
    std::unique_lock lock(m_resourceSection);

    for (auto resource : m_resources)
      resource->OnResetDisplay();
  }
}

bool CWinSystemGbmGLESContext::SetGuiCompositing(int colorTransfer)
{
  m_guiCompositing = (colorTransfer != 0);

  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (UseLimitedColor()
#ifdef HAVE_DVBRIDGE
          && !m_dvState.Captured() && !m_hdrConversion
#endif
          )
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGLES>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to compile GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
        return false;
      }
    }

    if (!m_compositeShader->CreateLUTs(colorTransfer))
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to create LUTs");
      m_compositeShader.reset();
      m_guiCompositing = false;
      return false;
    }
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemGbmGLESContext::BeginGuiComposite(bool guiWillRender)
{
  if (!m_guiCompositing)
    return false;

  m_guiWillRender = guiWillRender;

  int width = m_nWidth;
  int height = m_nHeight;

  // create or recreate FBO if size changed
  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();

    if (!m_guiFbo.Initialize())
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to initialize GUI FBO");
      return false;
    }

    if (!m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR, "CWinSystemGbmGLESContext: failed to create GUI FBO texture {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    if (GetEnabledFrontToBackRendering() && !m_guiFbo.AttachDepthBuffer(width, height))
    {
      CLog::Log(LOGERROR,
                "CWinSystemGbmGLESContext: failed to attach depth buffer to GUI FBO {}x{}", width,
                height);
      m_guiFbo.Cleanup();
      return false;
    }

    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false; // fresh FBO is undefined, force a clear
    CLog::Log(LOGDEBUG, "CWinSystemGbmGLESContext: created GUI FBO {}x{}", width, height);
  }

  // When GUI render is being skipped, leave the FBO bind/clear out: nothing
  // will draw into it this frame. The FBO's prior sRGB GUI content is
  // implicitly preserved across the skipped frame as a side effect.
  //! @todo The preserved sRGB FBO is currently not leveraged: D2P reuses the
  //! post-PQ GUI plane back buffer directly via display HW, and single-plane
  //! never reaches !guiWillRender (the dirty-driven skip is gated on
  //! IsRenderingVideoLayer). Future single-plane "gate, don't move" work
  //! lets the GUI walk skip while CompositeGui still runs each video frame,
  //! re-using this cached sRGB FBO as the composite source.
  if (!guiWillRender)
    return true;

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }

  return true;
}

void CWinSystemGbmGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

  // When the GUI render is skipped this frame, Flip(hasRendered=false, ...)
  // will skip eglSwapBuffers and the back-buffer contents never reach the
  // screen. Clearing it is pure waste. Gate on D2P because single-plane
  // never reaches !m_guiWillRender (the dirty-driven skip is gated on
  // IsRenderingVideoLayer()), so this is the only path that triggers.
  const bool isD2P = m_DRM && m_DRM->GetVideoPlane() != nullptr && m_DRM->GetGuiPlane() != nullptr;
  if (isD2P && !m_guiWillRender)
    return;

  // Clear the backbuffer before video renders. In the FBO compositing path,
  // video renders in the RenderEx pass with clear=false, so DrawBlackBars is
  // never called. Without this clear, letterbox areas retain stale content
  // from the swap chain when the display resolution doesn't change between
  // GUI and video playback.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// CompositeGui is the last GL operation in the frame (called just before EndRender).
// GL state (blend mode, active texture, vertex arrays) is not restored afterward;
// the next frame's rendering sets its own state.
void CWinSystemGbmGLESContext::CompositeGui()
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;

  // Only update m_guiFboClean when GUI render fired this frame; otherwise the
  // FBO is in the same state as the previous frame and the flag stays as-is.
  // m_guiFboClean meaning depends on context:
  //   single-plane: "FBO is empty/clean" (no composite work needed)
  //   D2P:          "FBO is empty/clean AND back buffer cache is invalid"
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() == 0);
    m_guiFboClean = guiEmpty;
    if (guiEmpty)
      return;
  }
  else if (m_guiFboClean)
  {
    return;
  }

  // D2P with no new render: the cached PQ frame is already in the GUI plane
  // back buffer from the prior composite. Skip the shader pass entirely; Flip
  // will skip eglSwapBuffers too (hasRendered==false because Render was not
  // called), and the display HW keeps scanning out the cached frame while the
  // video plane updates independently via atomic commit.
  if (!m_guiWillRender && m_DRM && m_DRM->GetVideoPlane() != nullptr &&
      m_DRM->GetGuiPlane() != nullptr)
    return;

#ifdef HAVE_DVBRIDGE
  GLint previousFramebuffer = 0;
  GLint previousViewport[4]{};
  const bool dvComposite = m_dvState.Captured() || m_hdrConversion;
  if (dvComposite)
  {
    if (!m_dvFrame || !m_dvBridge)
      return;
    const auto target = m_dvBridge->GetPQFramebuffer();
    if (!target)
    {
      m_dvFrame = false;
      return;
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, 3840, 2160);
  }
#endif

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());

  glEnable(GL_BLEND);

  // In D2P, the GUI plane is composited against a separate video plane by
  // the display HW. The default glBlendFunc also blends the alpha channel,
  // leaving the GUI plane buffer with src.a^2; the hardware composite then
  // reads that squared alpha and translucent GUI pixels render at the
  // wrong opacity. Replace the stored alpha so the hardware sees src.a.
  if (m_DRM->GetVideoPlane() && m_DRM->GetGuiPlane())
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
  else
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // set up orthographic projection (screen coords, Y-down)
  float w = static_cast<float>(m_guiFboWidth);
  float h = static_cast<float>(m_guiFboHeight);

  // BEGIN GUI target projection (also exercised by the GPU orientation regression).
  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};
#ifdef HAVE_DVBRIDGE
  if (dvComposite)
  {
    // libplacebo's PQ texture stores logical top at row zero. The final HDMI
    // packing pass reverses it for EGL scanout. Compose the GUI in that same
    // logical orientation, not in the ordinary window framebuffer orientation.
    proj[5] = -proj[5];
    proj[13] = -proj[13];
  }
#endif
  // END GUI target projection

  m_compositeShader->SetProjection(proj);
  m_compositeShader->Enable();

  GLint posLoc = m_compositeShader->GetPosLoc();
  GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

  m_compositeShader->Disable();
#ifdef HAVE_DVBRIDGE
  if (dvComposite)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
  }
#endif
}

#ifdef HAVE_DVBRIDGE
namespace
{
bool NativeDVEligible(bool source, bool software, bool interlaced, bool stereo)
{
  if (!source || software)
    return false;
  auto* window = dynamic_cast<CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  const auto mode = DVBRIDGE::playbackMode.load();
  const bool available = window && mode != DVBRIDGE::OutputMode::Disabled &&
      (mode == DVBRIDGE::OutputMode::HDR10 ? window->SupportsHDR10Conversion() : window->SupportsNativeDVOutput());
  return DVBRIDGE::Eligible(available, available, source, software, interlaced, stereo);
}
} // namespace

bool DVBRIDGE::WantsNativeOutput(const CDVDStreamInfo& stream)
{
  if (!(stream.codecOptions & CODEC_FORCE_SOFTWARE))
  {
    StreamTelemetry::Begin(stream.dovi.dv_profile, stream.dovi.dv_level,
        stream.dovi.dv_version_major, stream.dovi.dv_version_minor,
        stream.dovi.bl_present_flag, stream.dovi.el_present_flag, stream.dovi.rpu_present_flag);
    StreamTelemetry::StaticMetadata(stream.masteringMetadata.get(), stream.contentLightMetadata.get());
  }
  const bool eligible = NativeDVEligible(stream.dovi.dv_profile != 0,
                          (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0,
                          stream.interlaced, !stream.stereo_mode.empty());
  if (stream.dovi.dv_profile && DiagnosticsEnabled())
    CLog::Log(LOGINFO, "DVBridge stream candidate: profile={} level={} compatibility={} native_eligible={} software={} interlaced={} stereo={} mode={} (selection probe, not active output)",
        stream.dovi.dv_profile, stream.dovi.dv_level, stream.dovi.dv_bl_signal_compatibility_id, eligible,
        (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0, stream.interlaced,
        !stream.stereo_mode.empty(), static_cast<int>(playbackMode.load()));
  return eligible;
}

bool DVBRIDGE::WantsNativeOutput(const VideoPicture* picture)
{
  return picture && NativeDVEligible(picture->doviMetadata.Data() != nullptr, false,
                                     (picture->iFlags & DVP_FLAG_INTERLACED) != 0,
                                     !picture->stereoMode.empty());
}

bool CWinSystemGbmGLESContext::DVBridgeRequested(const VideoPicture* picture)
{
  return DVBRIDGE::WantsNativeOutput(picture);
}

int CWinSystemGbmGLESContext::VideoOutputBitDepth(const VideoPicture* picture) const
{
  if (!DVBridgeRequested(picture))
    return CWinSystemGbmEGLContext::VideoOutputBitDepth(picture);
  if (DVBRIDGE::playbackMode.load() == DVBRIDGE::OutputMode::HDR10)
    return 10;
  return 8;
}

bool CWinSystemGbmGLESContext::SupportsHDR10Conversion()
{
  if (!IsHDRDisplay() || !GetDisplayHDRCapabilities().SupportsHDR10())
    return false;
  const auto* mode = m_DRM->GetSelectedMode();
  auto* connector = m_DRM->GetConnector();
  return connector && connector->IsConnected() && connector->SupportsProperty("HDR_OUTPUT_METADATA") &&
      connector->SupportsProperty("DVBRIDGE_HDR10_DEEP_COLOR") &&
      mode && mode->hdisplay == 3840 && mode->vdisplay == 2160 && !(mode->flags & DRM_MODE_FLAG_INTERLACE);
}

bool CWinSystemGbmGLESContext::ConfigureHDR10Conversion(const VideoPicture& picture)
{
  VideoPicture output;
  output.SetParams(picture);
  output.color_transfer = AVCOL_TRC_SMPTE2084;
  output.color_primaries = AVCOL_PRI_BT2020;
  output.color_space = AVCOL_SPC_BT2020_NCL;
  output.colorBits = 10;
  dvbridge_hdr10_metadata metadata{};
  if (!dvbridge_get_hdr10_metadata(&metadata, picture.doviMetadata.Data(), picture.doviMetadata.Size()))
    return false;
  if (metadata.max_luminance)
  {
    if (!output.hasDisplayMetadata)
      output.displayMetadata = {};
    output.hasDisplayMetadata = true;
    output.displayMetadata.has_luminance = metadata.max_luminance != 0;
    output.displayMetadata.max_luminance = {static_cast<int>(metadata.max_luminance), 1};
    output.displayMetadata.min_luminance = {static_cast<int>(metadata.min_luminance), 10000};
    output.hasLightMetadata = true;
    output.lightMetadata.MaxCLL = metadata.max_cll;
    output.lightMetadata.MaxFALL = metadata.max_fall;
  }
  if (DVBRIDGE::DiagnosticsEnabled())
    CLog::Log(LOGINFO, "DVBridge HDR10 mapping: target={} nits origin={} curve=libplacebo-spline dynamic=L1 inverse=false maxcll=unknown maxfall=unknown",
        metadata.max_luminance, metadata.level6 ? "RPU-L6" : "RPU-source-PQ");
  SetColorimetry(&output);
  if (!SetHDR(&output))
    return false;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector)
    return false;
  const auto previous = connector->GetPropertyValue("DVBRIDGE_HDR10_DEEP_COLOR");
  if (!previous || !drm->AddProperty(connector, "DVBRIDGE_HDR10_DEEP_COLOR", 1))
    return false;
  m_hdrDeepColorPrevious = previous;
  if (!BeginDVBridge())
  {
    drm->AddProperty(connector, "DVBRIDGE_HDR10_DEEP_COLOR", *previous);
    m_hdrDeepColorPrevious.reset();
    return false;
  }
  if (DVBRIDGE::DiagnosticsEnabled())
    CLog::Log(LOGINFO, "DVBridge HDR10: link minimum=10 bits; native HDMI bandwidth and format validation retained");
  return true;
}

bool CWinSystemGbmGLESContext::BeginDVBridge()
{
  const auto failure = [this](const char* stage) {
    if (DVBRIDGE::DiagnosticsEnabled() &&
        m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), stage))
      CLog::Log(LOGERROR, "DVBridge setup: stage={} failures={} size={}x{} depth={} renderable={}",
          stage, m_dvSetupFailures.Count(), m_nWidth, m_nHeight,
          GetOutputBitDepth(), m_renderableType);
    return false;
  };
  const bool hdr10 = DVBRIDGE::playbackMode.load() == DVBRIDGE::OutputMode::HDR10;
  if (m_nWidth != 3840 || m_nHeight != 2160 ||
      (hdr10 ? GetOutputBitDepth() < 10 : GetOutputBitDepth() != 8) ||
      m_renderableType != EGL_OPENGL_ES3_BIT)
    return failure("output-format");
  if (m_dvState.Captured() || m_hdrConversion)
    return m_dvBridge && m_guiCompositing && m_compositeShader;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return failure("atomic-output-unavailable");
  if (!m_dvBridge)
    m_dvBridge = std::make_unique<CDVBridgeGLES>();
  if (!m_dvBridge->Initialize(false, hdr10))
    return failure("renderer-init");
  if (!hdr10 && !m_dvState.Captured() && !m_dvState.Capture(*drm))
    return failure("drm-state-capture");
  m_hdrConversion = hdr10;
  if (hdr10 && DVBRIDGE::DiagnosticsEnabled())
    CLog::Log(LOGINFO, "DVBridge conversion: output=HDR10 transfer=PQ primaries=BT2020 target_mapping=none framebuffer_bits={} range={}",
        GetOutputBitDepth(),
        UseLimitedColor() ? "limited" : "full");
  m_compositeShader.reset();
  return SetGuiCompositing(AVCOL_TRC_SMPTE2084);
}

bool CWinSystemGbmGLESContext::SetVideoOutput(const VideoPicture* picture)
{
  // Restore with the current surface before EGL replaces its buffers.
  if ((m_dvState.Captured() || m_hdrConversion) && !EndDVBridge())
    return false;
  m_dvBridge.reset();
  return CWinSystemGbmEGLContext::SetVideoOutput(picture);
}

bool CWinSystemGbmGLESContext::RenderDVBridge(const CDVBridgeGLES::Layer& layer,
                                            const AVFrame* enhancement,
                                            const CDVMetadataBuffer& metadata,
                                            dvbridge_geometry geometry)
{
  m_dvFrame = false;
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM); drm && !drm->DVRetryReady())
    return false;
  if (!BeginDVBridge())
    return false;
  const bool overlayFree = m_guiWillRender && GetGUIElementCount() == 0 && !m_dvOverlays;
  m_dvFrame = m_dvBridge->PrepareHardware(layer, enhancement, metadata, geometry, overlayFree);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return m_dvFrame;
}

bool CWinSystemGbmGLESContext::EndDVBridge()
{
  if (m_hdrDeepColorPrevious)
  {
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    if (!drm || !drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR",
                                  *m_hdrDeepColorPrevious))
      return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (!m_eglContext.TrySwapBuffers())
      return false;
    auto* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get();
    if (!bo || !drm->FlipPageTransactional(bo, true))
      return false;
    m_hdrDeepColorPrevious.reset();
    if (DVBRIDGE::DiagnosticsEnabled())
      CLog::Log(LOGINFO, "DVBridge HDR10: native link policy restored");
  }
  m_dvFrame = false;
  // Never reset Kodi's ordinary HDR GUI compositor unless we owned DV output.
  if (!m_dvState.Captured() && !m_hdrConversion)
    return true;
  if (m_dvState.Captured())
  {
    // Restore signaling with a clean ordinary framebuffer, never tunnel pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (!m_eglContext.TrySwapBuffers() || !FlipDVBridgePage(m_dvState, true))
    {
      if (DVBRIDGE::DiagnosticsEnabled() &&
          m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), "restore"))
        CLog::Log(LOGERROR, "DVBridge: output restoration failed; retaining scanout state; failures={}",
            m_dvSetupFailures.Count());
      return false;
    }
  }
  if (m_dvBridge)
    m_dvBridge->Reset();
  m_hdrConversion = false;
  if (DVBRIDGE::DiagnosticsEnabled() &&
      m_dvSetupFailures.Recovered(DVBRIDGE::DiagnosticNowMs()))
    CLog::Log(LOGINFO, "DVBridge: normal display restored after failures={}", m_dvSetupFailures.Count());
  SetGuiCompositing(0);
  return true;
}

bool CWinSystemGbmGLESContext::DestroyWindowSystem()
{
  if (!EndDVBridge())
    return false;
  m_dvBridge.reset();
  return CWinSystemGbmEGLContext::DestroyWindowSystem();
}
#endif

bool CWinSystemGbmGLESContext::CreateContext()
{
  const EGLint version = (m_renderableType == EGL_OPENGL_ES3_BIT) ? 3 : 2;

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, version}});

  if (!m_eglContext.CreateContext(contextAttribs))
  {
    CLog::Log(LOGERROR, "EGL context creation failed");
    return false;
  }
  return true;
}
