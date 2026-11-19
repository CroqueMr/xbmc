/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbmGLESContext.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#endif

#include "OptionalsReg.h"
#include "cores/RetroPlayer/process/gbm/RPProcessInfoGbm.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererDMAOpenGLES.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererFBO.h"
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
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <unistd.h>
#ifdef HAVE_DVBRIDGE
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "DVBridgeCapture.h"
#include "rendering/capture/CaptureService.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackPolicy.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "drm/DRMAtomic.h"
#endif

#include <gbm.h>

using namespace KODI::WINDOWING::GBM;

using namespace std::chrono_literals;

#ifdef HAVE_DVBRIDGE
float CWinSystemGbmGLESContext::GetDisplayLatency()
{
  if (CB1AsyncPlayback())
  {
    const float refresh = m_DRM->GetCurrentMode().fRefreshRate;
    const float fps = CServiceBroker::GetDataCacheCore().GetVideoFps();
    // One source-picture preparation interval and one pending display refresh.
    if (refresh >= 10.0f)
      return 1000.0f / refresh +
          1000.0f / (std::isfinite(fps) && fps >= 10.0f ? std::min(fps, refresh) : refresh);
  }
  return CWinSystemGbmEGLContext::GetDisplayLatency();
}

std::optional<double> CWinSystemGbmGLESContext::CB1PresentationAge() const
{
  if (!CB1AsyncPlayback()) return std::nullopt;
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  const uint64_t completed = drm ? drm->CB1PresentationTime() : 0;
  const double refresh = m_DRM->GetCurrentMode().fRefreshRate;
  timespec now{};
  if (!completed || !std::isfinite(refresh) || refresh < 10 ||
      clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return std::nullopt;
  const double nowNs = static_cast<double>(now.tv_sec) * 1e9 + now.tv_nsec;
  const double age = (nowNs - completed) / 1000;
  const double period = 1e6 / refresh;
  const double fps = CServiceBroker::GetDataCacheCore().GetVideoFps();
  const double sourcePeriod = std::isfinite(fps) && fps >= 10 ?
      std::max(period, 1e6 / fps) : period;
  // Keep one proven completion as the CPU crosses its estimated vblank timestamp.
  if (age < -period || age > 1.25 * sourcePeriod) return std::nullopt;
  return age;
}
#endif

CWinSystemGbmGLESContext::CWinSystemGbmGLESContext()
  : CWinSystemGbmEGLContext(EGL_PLATFORM_GBM_MESA, "EGL_MESA_platform_gbm")
{
}

void CWinSystemGbmGLESContext::Register()
{
  CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "gbm");
}

CWinSystemGbmGLESContext::~CWinSystemGbmGLESContext()
{
#ifdef HAVE_DVBRIDGE
  if (m_dvBridge) m_dvBridge->Cancel();
  m_dvBridge.reset();
  if (m_dvContext && m_dvContext->GL())
  {
    m_nativeDVRetirement = std::move(m_dvContext);
    m_nativeDVProperties = std::make_shared<CDVBridgeState>(std::move(m_dvState));
  }
#endif
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
  RETRO::CRPProcessInfoGbm::RegisterRendererFactory(new RETRO::CRendererFactoryFBO);

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
  if (!RetireDVContext()) return false;
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

#ifdef HAVE_DVBRIDGE
bool CWinSystemGbmGLESContext::BeginRender()
{
  // A prepared picture already includes its GUI; wait before drawing it again.
  if (m_cb1FramePending && m_dvFrame && !CompleteDVBridgePage(8)) return false;
  return CRenderSystemGLES::BeginRender();
}
#endif

void CWinSystemGbmGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  if (!m_bRenderCreated)
    return;

  PresentFrame(rendered, videoLayer);
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

void CWinSystemGbmGLESContext::PresentFrame(bool rendered, bool videoLayer)
{
#ifdef HAVE_DVBRIDGE
  // Yield to the pending scanout instead of repeatedly redrawing the GUI.
  if (!CompleteDVBridgePage(8)) return;
  if (m_dvStopPending && !StopDVBridgePlayback()) return;
  if (m_dvEndPending && !EndDVBridge()) return;
  if (m_failedHDR10SetupRestore && !HasPendingDVOutputSurface() && !EndDVBridge())
    return;
  if (!PollRejectedDVSurface()) return;
  if (m_dvOrdinaryFrame)
  {
    if (!rendered || !m_eglContext.TrySwapBuffers()) return;
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    auto& surface = m_GBM->GetDevice().GetSurface();
    auto* bo = surface.LockTransactionalBuffer();
    const bool submitted = drm && drm->FlipPageTransactional(bo,true);
    surface.CompleteTransactionalBuffer(submitted);
    if (!submitted)
    {
      DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(false);
      RejectDVOutputSurface(); return;
    }
    if (!CompleteDVOutputSurface(true)) return;
    surface.EndTransactionalMode();
    m_dvState.Restored(); m_hdrDeepColorPrevious.reset();
    m_dvOrdinaryFrame = false;
    DVBRIDGE::StreamTelemetry::ClearOutput();
    DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(true);
    return;
  }
  if (m_hdrConversion)
  {
    if (!m_dvFrame) return;
    bool presented = false;
    CDVBridgeGLES::Submission submitted;
    if (m_dvFrame && m_dvBridge && m_dvBridge->ResolveHDR10(UseLimitedColor()))
    {
      submitted = m_dvBridge->CaptureSubmission();
      const bool asynchronous = CreateCB1GPUFence(submitted);
      if (!asynchronous && !m_dvBridge->Advanced()) glFinish();
      if (m_eglContext.TrySwapBuffers())
      {
        auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        auto& surface = m_GBM->GetDevice().GetSurface();
        auto* bo = surface.LockTransactionalBuffer();
        const bool valid = drm && bo &&
            drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR", 1);
        presented = drm && drm->FlipPageTransactional(valid ? bo : nullptr, false, asynchronous);
        if (!drm || !drm->HasPendingCB1Page()) surface.CompleteTransactionalBuffer(presented);
      }
      else if (asynchronous)
        std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)->FlipPageTransactional(nullptr);
    }
    if (m_dvBridge && m_dvBridge->Presented(presented,submitted))
    {
      if (DVBRIDGE::PlaybackNotification::Pending())
        DVBRIDGE::PlaybackNotification::NativeOutputAccepted(DVBRIDGE::StreamTelemetry::Read().output);
      const auto* plane=m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
      DVBRIDGE::StreamTelemetry::NativeFormat(
          {submitted.identity.stream,submitted.identity.picture,submitted.identity.revision},
          plane ? plane->GetFormat() : 0,m_nWidth,m_nHeight,m_DRM->GetCurrentMode().fRefreshRate);
    }
    if (presented)
    {
      CompleteDVOutputSurface(true);
      m_dvActivating = false;
      if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->SetDVActivationPending(false);
      m_dvState.Restored();
    }
    else RejectDVOutputSurface();
    m_cb1FramePending = m_dvFrame = false;
    return;
  }
  if (m_dvState.Captured())
  {
    if (!m_dvFrame) return;
    bool presented = false;
    CDVBridgeGLES::Submission submitted;
    // EGL's window framebuffer has a bottom-left GL origin, while transport
    // metadata is indexed from the top-left of the HDMI raster.
    if (m_dvFrame && m_dvBridge && m_dvBridge->Pack(0, m_nWidth, m_nHeight, true))
    {
      submitted = m_dvBridge->CaptureSubmission();
      const bool asynchronous = CreateCB1GPUFence(submitted);
      if (!asynchronous && !m_dvBridge->Advanced()) glFinish();
      auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
      if (m_eglContext.TrySwapBuffers())
      {
        const bool link = !m_hdrDeepColorPrevious || (drm && drm->AddProperty(
            drm->GetConnector(),"DVBRIDGE_HDR10_DEEP_COLOR",*m_hdrDeepColorPrevious));
        if (link)
        {
          presented = FlipDVBridgePage(m_dvState, false, asynchronous);
        }
        else if (asynchronous) drm->FlipPageTransactional(nullptr);
      }
      else if (asynchronous) drm->FlipPageTransactional(nullptr);
    }
    if (m_dvBridge && m_dvBridge->Presented(presented,submitted))
    {
      if (DVBRIDGE::PlaybackNotification::Pending())
        DVBRIDGE::PlaybackNotification::NativeOutputAccepted(DVBRIDGE::StreamTelemetry::Read().output);
      const auto* plane=m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
      DVBRIDGE::StreamTelemetry::NativeFormat(
          {submitted.identity.stream,submitted.identity.picture,submitted.identity.revision},
          plane ? plane->GetFormat() : 0,m_nWidth,m_nHeight,m_DRM->GetCurrentMode().fRefreshRate);
    }
    if (presented)
    {
      CompleteDVOutputSurface(true);
      m_dvActivating = false;
      if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->SetDVActivationPending(false);
      m_hdrDeepColorPrevious.reset();
    }
    else RejectDVOutputSurface();
    m_cb1FramePending = m_dvFrame = false;
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
#ifdef HAVE_DVBRIDGE
    if (auto drm=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
      DVBRIDGE::PlaybackNotification::OrdinaryOutputAccepted(drm->LastPageAccepted());
#endif
  }
  else
  {
    KODI::TIME::Sleep(10ms);
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
#ifdef HAVE_DVBRIDGE
    m_dvPQSubtitleFbo.Cleanup();
    m_dvPQSubtitleWidth = m_dvPQSubtitleHeight = 0;
    m_dvPQSubtitleDraws = 0;
    m_dvPQSubtitleRendering = m_dvPQSubtitleCleared = m_dvNativeGui = false;
#endif
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

#ifdef HAVE_DVBRIDGE
  m_dvSubtitleBounds = {};
  m_dvSubtitleDraws = 0;
  m_dvSubtitleContent = 0;
  m_dvPQSubtitleDraws = 0;
  m_dvPQSubtitleCleared = false;
  // The one prepared next picture already owns its packed subtitle composition.
  if (!m_cb1FramePending || !m_dvFrame)
    m_dvNativeGui = false;
#endif

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }
#ifdef HAVE_DVBRIDGE
  if (HasDVBridgeScreenshot() && (!m_dvPQSubtitleFbo.IsValid() ||
      m_dvPQSubtitleWidth != width || m_dvPQSubtitleHeight != height) && BeginDVPQSubtitle())
    EndDVPQSubtitle();
#endif

  return true;
}

void CWinSystemGbmGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

#ifdef HAVE_DVBRIDGE
  // CB1 overwrites the full scanout raster, including exact black borders.
  if (m_dvState.Captured() || m_hdrConversion) return;
#endif

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
  bool hasPQSubtitles = false;
#ifdef HAVE_DVBRIDGE
  hasPQSubtitles = HasDVBridgeScreenshot() && m_dvPQSubtitleDraws != 0;
#endif
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() ==
#ifdef HAVE_DVBRIDGE
                          (hasPQSubtitles ? m_dvPQSubtitleDraws : 0u)
#else
                          0u
#endif
    );
    m_guiFboClean = guiEmpty;
    if (guiEmpty && !hasPQSubtitles)
      return;
  }
  else if (m_guiFboClean && !hasPQSubtitles)
  {
    return;
  }

#ifdef HAVE_DVBRIDGE
  if (m_dvNativeGui)
    return;
#endif

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
  GLint posLoc = m_compositeShader->GetPosLoc();
  GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  // PQ subtitles keep their original luminance and premultiplied alpha.
#ifdef HAVE_DVBRIDGE
  if (hasPQSubtitles)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_dvPQSubtitleFbo.Texture());
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_compositeShader->SetInputPQ(true);
    m_compositeShader->Enable();
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
    m_compositeShader->Disable();
  }
#endif
  if (!m_guiFboClean)
  {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());
    if (m_DRM->GetVideoPlane() && m_DRM->GetGuiPlane())
      glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    else
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_compositeShader->SetInputPQ(false);
    m_compositeShader->Enable();
    glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
    m_compositeShader->Disable();
  }
  m_compositeShader->SetInputPQ(false);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

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
      window->CanRetainDVSource();
  return DVBRIDGE::Eligible(available, available, source, software, interlaced, stereo);
}
} // namespace

bool DVBRIDGE::RetainsDVSource(const CDVDStreamInfo& stream)
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
      stream.interlaced,!stream.stereo_mode.empty());
  if (stream.dovi.dv_profile && DiagnosticsEnabled())
    CLog::Log(LOGINFO, "DVBridge stream candidate: profile={} level={} compatibility={} native_eligible={} software={} interlaced={} stereo={} mode={} (selection probe, not active output)",
        stream.dovi.dv_profile, stream.dovi.dv_level, stream.dovi.dv_bl_signal_compatibility_id, eligible,
        (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0, stream.interlaced,
        !stream.stereo_mode.empty(), static_cast<int>(playbackMode.load()));
  return eligible;
}

bool DVBRIDGE::WantsNativeOutput(const CDVDStreamInfo& stream)
{
  return NativeDVEligible(stream.dovi.dv_profile != 0,
      (stream.codecOptions & CODEC_FORCE_SOFTWARE) != 0,
      stream.interlaced,!stream.stereo_mode.empty());
}

bool DVBRIDGE::WantsNativeOutput(const VideoPicture* picture)
{
  auto* window = dynamic_cast<CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  if (!window) return false;
  const auto selected = window->SelectedDVOutput(picture);
  if (selected.route != OutputRoute::Vanilla) return true;
  if (selected.reason != OutputReason::NoGPUProcessing &&
      selected.reason != OutputReason::NoDVTransport &&
      selected.reason != OutputReason::NoHDR10Transport) return false;
  auto caps = window->GetDVOutputCapabilities(picture, true);
  // Keep source/cadence intent before the GUI thread selects the required mode.
  // Cached current-mode facts and activation remain strict.
  if (SelectOutput(FrameOutputSettings(), caps).route != OutputRoute::Vanilla) return true;
  // Decoder configuration precedes GL import; unknown import must not erase DV cadence intent.
  return picture && caps.isDV && !caps.processingKnown &&
      (picture->doviSourceBinding.Valid() || picture->doviMetadata.Binding().Valid()) &&
      NativeDVEligible(true, false, (picture->iFlags & DVP_FLAG_INTERLACED) != 0,
                       !picture->stereoMode.empty());
}

bool CWinSystemGbmGLESContext::DVBridgeRequested(const VideoPicture* picture)
{
  return DVBRIDGE::WantsNativeOutput(picture);
}

int CWinSystemGbmGLESContext::VideoOutputBitDepth(const VideoPicture* picture) const
{
  const auto selection = SelectedDVOutput(picture);
  return selection.route == DVBRIDGE::OutputRoute::NativeDV ? 8 :
      selection.route == DVBRIDGE::OutputRoute::HDR10 ? 10 :
      CWinSystemGbmEGLContext::VideoOutputBitDepth(picture);
}

bool CWinSystemGbmGLESContext::CreateNewWindow(const std::string& name, bool fullScreen,
                                             RESOLUTION_INFO& res)
{
  if (!CWinSystemGbmEGLContext::CreateNewWindow(name, fullScreen, res)) return false;
  RefreshDVOutputCapabilities();
  return true;
}

void CWinSystemGbmGLESContext::RefreshDVOutputCapabilities()
{
  if (eglGetCurrentContext() == EGL_NO_CONTEXT ||
      eglGetCurrentContext() != m_eglContext.GetEGLContext()) return;
  auto caps = GetDVOutputCapabilities(nullptr);
  caps.nativeDV = SupportsNativeDVOutput();
  caps.hdr10 = SupportsHDR10Conversion();
  if (m_renderableType != EGL_OPENGL_ES3_BIT)
  {
    caps.processingKnown = true;
    for (auto& mode : caps.processing) mode = {false, DVBRIDGE::OutputReason::NoGPUProcessing};
    for (auto& mode : caps.hdr10Processing) mode = {false, DVBRIDGE::OutputReason::NoGPUProcessing};
  }
  else if (m_dvBridge && m_dvBridge->HasRenderer())
  {
    caps.processingKnown = true;
    for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
    {
      const bool usable=m_dvBridge->SupportsMode(static_cast<dvbridge_mode>(mode));
      caps.processing[mode] = {usable, usable ? DVBRIDGE::OutputReason::None :
          DVBRIDGE::OutputReason::NoGPUProcessing};
      const bool ai=m_dvBridge->SupportsHDR10AI(static_cast<dvbridge_mode>(mode));
      caps.hdr10Processing[mode] = {ai,ai ? DVBRIDGE::OutputReason::None : DVBRIDGE::OutputReason::NoGPUProcessing};
    }
  }
  else
  {
    CDVBridgeGLES::Context probe;
    auto* context = m_dvContext ? m_dvContext.get() : &probe;
    if (context->Initialize(false))
    {
      if (auto* renderer = context->Attach())
      {
        const bool common = pl_opengl_has_ext(context->GL(), "GL_EXT_float_blend") &&
            pl_opengl_has_ext(context->GL(), "GL_OES_texture_float_linear");
        caps.processingKnown = true;
        for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
        {
          const bool usable=common && dvbridge_renderer_supports_mode(renderer, static_cast<dvbridge_mode>(mode));
          caps.processing[mode] = {usable, usable ? DVBRIDGE::OutputReason::None :
              DVBRIDGE::OutputReason::NoGPUProcessing};
          const bool ai=common && dvbridge_renderer_supports_hdr10_ai(renderer,static_cast<dvbridge_mode>(mode));
          caps.hdr10Processing[mode] = {ai,ai ? DVBRIDGE::OutputReason::None : DVBRIDGE::OutputReason::NoGPUProcessing};
        }
        context->Detach(renderer);
      }
    }
  }
  const bool availableNativeDV = SupportsNativeDVOutput(false);
  const bool availableHDR10 = SupportsHDR10Conversion(false);
  std::lock_guard lock(m_dvCapabilityMutex);
  m_dvCapabilities = caps;
  m_dvAvailableNativeDV = availableNativeDV;
  m_dvAvailableHDR10 = availableHDR10;
}

bool CWinSystemGbmGLESContext::CanRetainDVSource() const
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  if (preferences.output == DVBRIDGE::OutputMode::Disabled) return false;
  const auto caps = GetDVOutputCapabilities(nullptr, true);
  if (!caps.nativeDV && !caps.hdr10) return false;
  if (!caps.processingKnown) return true;
  const auto mode = preferences.Policy().mode;
  return (caps.processing[mode].usable &&
      (preferences.output == DVBRIDGE::OutputMode::HDR10 ? caps.hdr10 : caps.nativeDV)) ||
      (preferences.output != DVBRIDGE::OutputMode::HDR10 && caps.hdr10 &&
       caps.processing[DVBRIDGE_MODE_HDR10_BASIC].usable);
}

bool CWinSystemGbmGLESContext::CompleteDVModeSwitch(const VideoPicture& picture)
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (SupportsDVOutputMode() && drm && drm->IsSelectedModeCommitted()) return true;
  // An advertised mode can still fail its real modeset. Reuse the existing
  // per-source activation failure latch; never wait or retry every frame.
  ResetDVActivation(&picture);
  std::lock_guard lock(m_dvCapabilityMutex);
  m_dvNativeFailed = m_dvHDR10Failed = m_dvFallbackActive = true;
  m_dvActivationReason = DVBRIDGE::OutputReason::ActivationFailed;
  m_dvFailedStream = m_dvActivationStream;
  m_dvFailedPolicy = m_dvActivationPolicy;
  return false;
}

void CWinSystemGbmGLESContext::ObserveDVImport(DVBRIDGE::SourceBinding binding,
                                              dvbridge_geometry geometry, bool usable)
{
  std::lock_guard lock(m_dvCapabilityMutex);
  if (eglGetCurrentContext() != m_eglContext.GetEGLContext()) binding = {};
  m_dvImportBinding = binding;
  m_dvImportGeometry = geometry;
  m_dvImportUsable = usable;
  m_dvImportGeneration = m_dvCapabilities.generation;
  if (binding.Valid() && (binding.stream != m_dvFatalBinding.stream ||
                          binding.revision != m_dvFatalBinding.revision))
  {
    m_dvFatalBinding = binding;
    m_dvFatalReason = DVBRIDGE::OutputReason::None;
    m_dvFatalConsumed = false;
  }
}

DVBRIDGE::OutputCapabilities CWinSystemGbmGLESContext::GetDVOutputCapabilities(
    const VideoPicture* picture, bool allowModeSwitch) const
{
  return GetDVOutputCapabilities(picture, DVBRIDGE::FrameOutputSettings(), allowModeSwitch);
}

DVBRIDGE::OutputCapabilities CWinSystemGbmGLESContext::GetDVOutputCapabilities(
    const VideoPicture* picture, const DVBRIDGE::OutputPreferences& preferences, bool allowModeSwitch) const
{
  std::lock_guard lock(m_dvCapabilityMutex);
  auto caps = m_dvCapabilities;
  if (allowModeSwitch)
  {
    caps.nativeDV = m_dvAvailableNativeDV;
    caps.hdr10 = m_dvAvailableHDR10;
  }
  if (!picture) return caps;
  caps.sourceKnown = true;
  caps.isDV = picture->doviSourceInfo.profile || picture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION;
  caps.isHDR10 = !caps.isDV && picture->hdrType == StreamHdrType::HDR_TYPE_HDR10;
  if (caps.isHDR10)
  {
#ifdef CB1_HAS_HDR10_AI
    caps.processing=caps.hdr10Processing;
    const bool valid = m_dvImportUsable && m_dvImportBinding.Valid() &&
        m_dvImportBinding == picture->doviSourceBinding &&
        m_dvImportGeneration == caps.generation &&
        picture->doviSourcePts != AV_NOPTS_VALUE &&
        !(picture->iFlags & DVP_FLAG_INTERLACED) && picture->stereoMode.empty();
    for (auto& mode : caps.processing)
      if (!valid) mode = {false,DVBRIDGE::OutputReason::SourceIneligible};
#endif
    return caps;
  }
  if (!caps.isDV) return caps;
  const auto binding = picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding();
  if (!m_dvImportBinding.Valid() || m_dvImportBinding != binding ||
      m_dvImportGeneration != caps.generation)
  {
    caps.processingKnown = false;
    return caps;
  }
  const auto& metadata = picture->doviMetadata;
  const bool paired = picture->doviEnhancement && picture->doviEnhancementBinding.Valid() &&
      picture->doviEnhancementBinding == metadata.Binding() &&
      picture->doviEnhancement->pts == picture->doviSourcePts;
  dvbridge_color color{};
  unsigned margins[4];
  const bool valid = m_dvImportUsable && metadata.Binding().Valid() &&
      picture->doviSourcePts != AV_NOPTS_VALUE && !(picture->iFlags & DVP_FLAG_INTERLACED) &&
      picture->stereoMode.empty() &&
      dvbridge_frame_area(margins, metadata.Data(), metadata.Size(), m_dvImportGeometry) &&
      dvbridge_map_color(&color, metadata.Data(), metadata.Size(), paired);
  dvbridge_hdr10_session session{};
  const bool basic = valid && dvbridge_hdr10_session_init(&session, metadata.Data(), metadata.Size());
  for (unsigned mode = 1; mode < caps.processing.size(); ++mode)
  {
    if (!valid)
      caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    else if (mode == DVBRIDGE_MODE_HDR10_BASIC && !basic)
      caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    else if (mode >= DVBRIDGE_MODE_HDR10_EXPERT && caps.processing[mode].usable && preferences.ProfileValid())
    {
      auto policy = preferences.Policy();
      policy.mode = static_cast<dvbridge_mode>(mode);
      policy.tv = {preferences.peak, static_cast<dvbridge_panel>(preferences.panel),
          static_cast<dvbridge_gamut>(preferences.gamut)};
      pl_color_space nominal{};
      dvbridge_hdr10_metadata signal{};
      if (!dvbridge_reference_map(&color, &nominal, &signal, &policy,
                                 metadata.Data(), metadata.Size(), paired))
        caps.processing[mode] = {false, DVBRIDGE::OutputReason::SourceIneligible};
    }
  }
  return caps;
}

DVBRIDGE::OutputSelection CWinSystemGbmGLESContext::SelectedDVOutput(const VideoPicture* picture) const
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  auto caps = GetDVOutputCapabilities(picture, preferences);
  return SelectedDVOutput(picture, preferences, caps);
}

DVBRIDGE::OutputSelection CWinSystemGbmGLESContext::SelectedDVOutput(const VideoPicture* picture,
    const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::OutputCapabilities& caps) const
{
  const auto binding = picture ? (picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding()) : DVBRIDGE::SourceBinding{};
  auto reason = DVBRIDGE::OutputReason::None;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream == m_dvActivationStream && preferences.revision == m_dvActivationPolicy)
    {
      caps.nativeDV &= !m_dvNativeFailed;
      caps.hdr10 &= !m_dvHDR10Failed;
      reason = m_dvActivationReason;
    }
  }
  auto result = DVBRIDGE::SelectOutput(preferences, caps);
#ifdef CB1_HAS_HDR10_AI
  if (caps.isHDR10)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (!m_aiReady || m_aiFallback || binding.stream != m_aiActivation.stream ||
        binding.revision != m_aiActivation.revision || preferences.revision != m_aiPolicy)
      result = {};
  }
#endif
  if (reason != DVBRIDGE::OutputReason::None) result.reason = reason;
  return result;
}

#ifdef CB1_HAS_HDR10_AI
#ifndef CB1_L1L3_BUNDLE_DIR
#define CB1_L1L3_BUNDLE_DIR "/usr/share/cb1/l1l3"
#endif
bool CWinSystemGbmGLESContext::HDR10AILookaheadActive() const
{
  std::lock_guard lock(m_dvCapabilityMutex);
  return m_aiActive;
}

bool CWinSystemGbmGLESContext::PrepareHDR10AIQueue(const VideoPicture& picture,
    std::span<const CDVBridgeGLES::Layer> queue, bool end, bool paused,
    uint64_t presentationDeadline)
{
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const auto caps = GetDVOutputCapabilities(&picture, preferences);
  if (!caps.isHDR10 ||
      DVBRIDGE::SelectOutput(preferences, caps).route == DVBRIDGE::OutputRoute::Vanilla)
  {
    {
      std::lock_guard lock(m_dvCapabilityMutex);
      m_aiActive=false;
    }
    return !m_dvContext || m_dvContext->StopHDR10AI();
  }
  const auto binding = picture.doviSourceBinding;
  if (queue.empty() || !(queue.front().binding == binding)) return false;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream != m_aiActivation.stream ||
        binding.revision != m_aiActivation.revision || preferences.revision != m_aiPolicy)
    {
      m_aiActivation = binding;
      m_aiPolicy = preferences.revision;
      m_aiReady = m_aiFallback = false;
    }
    m_aiActive=!m_aiFallback;
    if (m_aiFallback) return true;
  }
  if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
  auto decision = HDR10AISource::Decision::Native;
  cb1_ai_result prediction{};
  if (m_dvContext->Initialize(false))
  {
    decision = m_dvContext->PrepareHDR10AIQueue(queue, preferences,
        CB1_L1L3_BUNDLE_DIR, DVBRIDGE::DiagnosticNowMs(), end, &prediction, paused,
        presentationDeadline);
  }
  if (decision == HDR10AISource::Decision::Prepare &&
      (!m_dvBridge || !m_dvBridge->CommittedHDR10AI()))
  {
    // Prime the output before Kodi starts the playback clock.
    {
      std::lock_guard lock(m_dvCapabilityMutex);
      m_aiReady = true;
    }
    auto status = CDVBridgeGLES::FrameStatus::Pending;
    if (PrepareDVOutputFrame(picture) && BeginDVBridge())
      status = m_dvBridge->PrepareHDR10AIFrame(queue.front(),
          m_dvImportGeometry, preferences, prediction, m_dvSelection.policy);
    const auto result = status == CDVBridgeGLES::FrameStatus::Ready ||
        status == CDVBridgeGLES::FrameStatus::Reused ? CB1_AI_READY :
        status == CDVBridgeGLES::FrameStatus::Failed ||
        status == CDVBridgeGLES::FrameStatus::Unsupported ? CB1_AI_INCOMPATIBLE : CB1_AI_PENDING;
    decision = m_dvContext->HDR10AI().Preparation(binding, result, DVBRIDGE::DiagnosticNowMs());
  }
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    m_aiReady |= decision == HDR10AISource::Decision::Prepare;
    m_aiFallback = decision == HDR10AISource::Decision::Native;
    m_aiActive = !m_aiFallback;
  }
  if (decision == HDR10AISource::Decision::Native)
  {
    CLog::Log(LOGWARNING, "CB1 HDR10 AI: native HDR10 fallback stream={} revision={} policy={}",
        binding.stream, binding.revision, preferences.revision);
    DVBRIDGE::PlaybackNotification::HDR10Recovery();
  }
  return decision == HDR10AISource::Decision::Prepare ||
      decision == HDR10AISource::Decision::Native;
}
#endif

void CWinSystemGbmGLESContext::ResetDVActivation(const VideoPicture* picture)
{
  if (!picture) return;
  const auto binding = picture->doviSourceBinding.Valid() ?
      picture->doviSourceBinding : picture->doviMetadata.Binding();
  const auto revision = DVBRIDGE::FrameOutputSettings().revision;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (binding.stream == m_dvActivationStream && revision == m_dvActivationPolicy) return;
    m_dvActivationStream = binding.stream;
    m_dvActivationPolicy = revision;
    m_dvNativeFailed = m_dvHDR10Failed = m_dvFallbackActive = false;
    m_dvActivationReason = DVBRIDGE::OutputReason::None;
    m_dvFailedStream = m_dvFailedPolicy = 0;
  }
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM)) drm->ResetDVActivation();
}

bool CWinSystemGbmGLESContext::RecoverDVActivation()
{
  if (!m_dvRecoveryPending) return true;
  if (!PollRejectedDVSurface()) return false;
  // Retire the rejected GPU work before restoring properties with clean pixels.
  if (!m_dvContext || m_dvContext->Poll() != DVBRIDGE_DV_IDLE) return false;
  if (!EndDVBridge())
  {
    m_dvRollbackFailed = true;
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvActivationReason = DVBRIDGE::OutputReason::RestoreFailed;
    return false;
  }
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  bool fallback;
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    fallback = m_dvFallbackActive;
  }
  const bool retry = drm && !fallback && drm->CanRetryDVActivation(true);
  if (!retry)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (m_dvRejectedRoute == DVBRIDGE::OutputRoute::NativeDV) m_dvNativeFailed = true;
    if (m_dvRejectedRoute == DVBRIDGE::OutputRoute::HDR10) m_dvHDR10Failed = true;
    m_dvFallbackActive = true;
    m_dvActivationReason = DVBRIDGE::OutputReason::ActivationFailed;
    m_dvFailedStream = m_dvActivationStream;
    m_dvFailedPolicy = m_dvActivationPolicy;
  }
  m_dvConfiguredOutput.reset();
  DVBRIDGE::StreamTelemetry::NativeFailure(DVBRIDGE::OutputReason::ActivationFailed,
      drm ? drm->DVActivationError() : 0,drm ? drm->DVActivationFailures() : 0,true);
  m_dvRecoveryPending = false;
  return true;
}

DVBRIDGE::OutputReason CWinSystemGbmGLESContext::ConsumeDVPlaybackFailure(DVBRIDGE::SourceBinding binding)
{
  std::lock_guard lock(m_dvCapabilityMutex);
  if (!binding.Valid() || binding.stream != m_dvFatalBinding.stream ||
      binding.revision != m_dvFatalBinding.revision || m_dvFatalConsumed)
    return DVBRIDGE::OutputReason::None;
  if (m_dvFatalReason != DVBRIDGE::OutputReason::None) m_dvFatalConsumed = true;
  return m_dvFatalReason;
}

bool CWinSystemGbmGLESContext::SupportsHDR10Conversion(bool requireCurrentMode)
{
  if (!IsHDRDisplay() || !GetDisplayHDRCapabilities().SupportsHDR10())
    return false;
  auto* connector = m_DRM->GetConnector();
  return connector && connector->IsConnected() && connector->SupportsProperty("HDR_OUTPUT_METADATA") &&
      connector->SupportsProperty("DVBRIDGE_HDR10_DEEP_COLOR") &&
      SupportsDVOutputMode(requireCurrentMode);
}

bool CWinSystemGbmGLESContext::ConfigureHDR10Conversion(const VideoPicture& picture)
{
  if (!CompleteDVBridgePage()) return false;
  const auto stream = picture.doviMetadata.Binding().stream;
  if (!stream) return false;
  if (m_failedHDR10SetupRestore && !EndDVBridge())
    return false;
  dvbridge_hdr10_session session{};
  if (m_hdr10Session && m_hdr10SessionStream == stream) session = *m_hdr10Session;
  else if (!dvbridge_hdr10_session_init(&session, picture.doviMetadata.Data(), picture.doviMetadata.Size()))
    return false;
  auto atomic = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (m_dvState.Captured() && (!atomic || !m_dvState.Stage(*atomic,true))) return false;
  VideoPicture output;
  output.SetParams(picture);
  output.color_transfer = AVCOL_TRC_SMPTE2084;
  output.color_primaries = AVCOL_PRI_BT2020;
  output.color_space = AVCOL_SPC_BT2020_NCL;
  output.colorBits = 10;
  const auto& metadata = session.output;
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
  // Every rejected setup after this point must restore ordinary HDR/color state.
  m_failedHDR10SetupRestore = true;
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
  if (!BeginDVBridge() || !m_dvBridge->SetHDR10Session(session,stream))
  {
    if (!drm->AddProperty(connector, "DVBRIDGE_HDR10_DEEP_COLOR", *previous))
    {
      drm->FlipPageTransactional(nullptr, true);
    }
    m_hdrDeepColorPrevious.reset();
    return false;
  }
  m_hdr10Session = session;
  m_hdr10SessionStream = stream;
  m_failedHDR10SetupRestore = false;
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    CLog::Log(LOGINFO, "DVBridge HDR10 configured: policy=Basic target={} nits origin={} curve=libplacebo-spline dynamic=L1-or-static inverse=false creative_trims=not-applied maxcll=unknown maxfall=unknown framebuffer_bits={} range={}",
        session.target.hdr.max_luma, session.output.level6 ? "RPU-L6" : "RPU-source-PQ",
        GetOutputBitDepth(), UseLimitedColor() ? "limited" : "full");
    CLog::Log(LOGINFO, "DVBridge HDR10: link minimum=10 bits; native HDMI bandwidth and format validation retained");
  }
  return true;
}

bool CWinSystemGbmGLESContext::RestoreFailedHDR10Setup()
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return false;
  if (!StageOrdinaryHDR10Recovery())
  {
    drm->FlipPageTransactional(nullptr, true);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
  if (!m_eglContext.TrySwapBuffers())
  {
    drm->FlipPageTransactional(nullptr, true);
    return false;
  }
  auto& surface = m_GBM->GetDevice().GetSurface();
  auto* bo = surface.LockTransactionalBuffer();
  const bool presented = drm->FlipPageTransactional(bo, true);
  surface.CompleteTransactionalBuffer(presented);
  if (!presented)
    return false;
  surface.EndTransactionalMode();
  CompleteOrdinaryHDR10Recovery();
  m_failedHDR10SetupRestore = false;
  return true;
}

bool CWinSystemGbmGLESContext::BeginDVBridge()
{
  if (!PollRejectedDVSurface()) return false;
  const auto failure = [this](const char* stage) {
    if (DVBRIDGE::DiagnosticsEnabled() &&
        m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), stage))
      CLog::Log(LOGERROR, "DVBridge setup: stage={} failures={} size={}x{} depth={} renderable={}",
          stage, m_dvSetupFailures.Count(), m_nWidth, m_nHeight,
          GetOutputBitDepth(), m_renderableType);
    return false;
  };
  const bool hdr10 = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10;
  if (!SupportsDVOutputMode() || m_nWidth != 3840 || m_nHeight != 2160 ||
      (hdr10 ? GetOutputBitDepth() < 10 : GetOutputBitDepth() != 8) ||
      m_renderableType != EGL_OPENGL_ES3_BIT)
    return failure("output-format");
  if (m_dvBridge && m_dvConfiguredOutput == m_dvSelection.policy.mode &&
      (m_dvState.Captured() || m_hdrConversion))
    return m_dvBridge && m_guiCompositing && m_compositeShader;
  if (!CompleteDVBridgePage()) return false;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || !drm->IsSelectedModeCommitted())
    return failure("atomic-output-unavailable");
  CDVWindowRetirement::PollOnce();
  if (CDVBridgeGLES::Context::NewContextBlocked()) return failure("context-retained");
  if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
  if (m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE)
    return failure("context-retiring");
  if (!m_dvBridge)
  {
    m_dvBridge = std::make_unique<CDVBridgeGLES>();
    if (!m_dvBridge->Initialize(false, hdr10,m_dvContext))
      return failure("renderer-init");
  }
  m_GBM->GetDevice().UseDVOutputLifetime();
  if (!hdr10 && !m_dvState.Captured() && !m_dvState.Capture(*drm))
    return failure("drm-state-capture");
  m_hdrConversion = hdr10;
  m_compositeShader.reset();
  const bool ready = SetGuiCompositing(AVCOL_TRC_SMPTE2084);
  if (ready) m_dvConfiguredOutput = m_dvSelection.policy.mode;
  return ready;
}

bool CWinSystemGbmGLESContext::SetVideoOutput(const VideoPicture* picture)
{
  if (picture && m_dvStopPending) return false;
  if (picture && !SupportsDVOutputMode() && DVBridgeRequested(picture))
    return true; // Configure/import first; the GUI owner must modeset before transport setup.
  if (!CompleteDVBridgePage()) return false;
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  if (!RecoverDVActivation()) return false;
  ResetDVActivation(picture);
  const auto capabilities = GetDVOutputCapabilities(picture);
  if (picture && capabilities.isDV && preferences.output != DVBRIDGE::OutputMode::Disabled &&
      (!capabilities.processingKnown || (picture->strDVELType == "FEL" && !picture->doviEnhancement)))
    return true; // Configure must reach UploadTexture before source-dependent activation.
  m_dvSelection = SelectedDVOutput(picture);
  if (!PollRejectedDVSurface()) return false;
  const bool transaction = DVBridgeRequested(picture) || m_dvContext || m_dvState.Captured();
  if (transaction)
  {
    if (HasPendingDVOutputSurface()) return false;
    // Same-format policy/frame work bypasses this actual transport transition.
    const bool active = m_dvSelection.route == DVBRIDGE::OutputRoute::NativeDV ?
        m_dvState.Captured() : m_hdrConversion;
    m_dvActivating = DVBridgeRequested(picture) && (m_dvActivating || !active);
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
      drm->SetDVActivationPending(m_dvActivating);
    if (!m_dvOutputRetiring)
    {
      m_dvPriorConfiguredOutput = m_dvState.Captured() || m_hdrConversion ?
          m_dvConfiguredOutput : std::nullopt;
      m_dvPriorHDRConversion = m_hdrConversion;
      m_dvPriorGuiCompositing = m_guiCompositing;
      m_dvPendingStream = picture ? (picture->doviSourceBinding.Valid() ?
          picture->doviSourceBinding.stream : picture->doviMetadata.Binding().stream) : 0;
      m_dvPendingPolicy = preferences.revision;
      if (m_dvBridge)
      {
        m_dvBridge->Cancel();
        // Cancelled readiness cannot cross an output-format transaction.
        m_cb1FramePending = m_dvFrame = m_dvNativeGui = false;
        m_dvOutputRetiring = true;
        if (!m_dvContext->FenceLastUse()) return false;
      }
    }
    if (m_dvContext && m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE)
      return false;
    if (!m_dvContext) m_dvContext = std::make_shared<CDVBridgeGLES::Context>();
    if (!m_dvContext->Initialize(false)) return false;
    m_dvOutputRetiring = false;
    m_GBM->GetDevice().UseDVOutputLifetime();
    UseDVOutputSurfaceTransaction(true);
    const bool staged = CWinSystemGbmEGLContext::SetVideoOutput(picture);
    UseDVOutputSurfaceTransaction(false);
    if (!staged) return false;
    const bool requested = DVBridgeRequested(picture);
    const bool hdr10 = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10;
    if (requested && m_dvBridge && !m_dvBridge->RebindOutputSurface(hdr10))
    {
      RejectDVOutputSurface();
      return false;
    }
    m_hdrConversion = false;
    m_dvConfiguredOutput.reset();
    // Stop has no video frame to complete the staged GUI output transaction.
    if (!picture && HasPendingDVOutputSurface()) m_dvOrdinaryFrame = true;
    return true;
  }
  // Restore with the current surface before EGL replaces its buffers.
  if ((m_dvState.Captured() || m_hdrConversion || m_failedHDR10SetupRestore) && !EndDVBridge())
    return false;
  m_dvBridge.reset();
  return CWinSystemGbmEGLContext::SetVideoOutput(picture);
}

bool CWinSystemGbmGLESContext::PrepareDVOutputFrame(const VideoPicture& picture)
{
  if (m_dvStopPending) return false;
  if (!SupportsDVOutputMode() && DVBridgeRequested(&picture))
    return false; // A future-mode admission is not permission to activate at 1080p.
  if (m_dvRollbackFailed || !RecoverDVActivation())
  {
    if (m_dvRollbackFailed)
    {
      if (DVBRIDGE::StreamTelemetry::Read().transaction.reason != DVBRIDGE::OutputReason::RestoreFailed)
      {
        auto atomic=std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        DVBRIDGE::StreamTelemetry::NativeFailure(DVBRIDGE::OutputReason::RestoreFailed,
            0,atomic ? atomic->DVActivationFailures() : 0,false);
      }
      std::lock_guard lock(m_dvCapabilityMutex);
      if (!m_dvFatalConsumed) m_dvFatalReason = DVBRIDGE::OutputReason::RestoreFailed;
    }
    return false;
  }
  // A stopped file's GUI transaction must be presented before new video setup.
  if (m_dvOrdinaryFrame && HasPendingDVOutputSurface()) return false;
  ResetDVActivation(&picture);
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const auto capabilities = GetDVOutputCapabilities(&picture, preferences);
#ifdef CB1_HAS_HDR10_AI
  if ((!capabilities.isHDR10 || preferences.hdr10Source==DVBRIDGE::HDR10SourceMode::Native) &&
      m_dvContext && !m_dvContext->StopHDR10AI()) return false;
#endif
  if (capabilities.isDV && preferences.output != DVBRIDGE::OutputMode::Disabled &&
      (!capabilities.processingKnown || (picture.strDVELType == "FEL" && !picture.doviEnhancement)))
    return false;
  auto selectionCapabilities = capabilities;
  m_dvSelection = SelectedDVOutput(&picture, preferences, selectionCapabilities);
  // Retained renderer facts belong to this render-side phase, never Configure.
  const auto binding = picture.doviSourceBinding.Valid() ?
      picture.doviSourceBinding : picture.doviMetadata.Binding();
  if (capabilities.sourceKnown)
  {
    auto identity = m_dvBridge ? m_dvBridge->SelectionIdentity(binding, preferences, m_dvSelection.policy.mode) :
        DVBRIDGE::SourceBinding{};
    if (!identity.Valid() && binding.Valid()) identity = {binding.stream,binding.picture,preferences.revision};
    int requested=static_cast<int>(preferences.Policy().mode);
#ifdef CB1_HAS_HDR10_AI
    if (capabilities.isHDR10) requested=static_cast<int>(preferences.HDR10Policy().mode);
#endif
    DVBRIDGE::StreamTelemetry::NativeSelection(identity, selectionCapabilities,
        requested,int(m_dvSelection.policy.mode),preferences.preset,m_dvSelection.reason);
  }
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    m_dvAttemptFailures = drm->DVActivationFailures();
  // Every selected automatic HDR10 fallback owns a single attempt for this file.
  if (capabilities.processingKnown && capabilities.isDV &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 || !capabilities.nativeDV) &&
      preferences.output != DVBRIDGE::OutputMode::HDR10 &&
      preferences.output != DVBRIDGE::OutputMode::Disabled)
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvNativeFailed = m_dvFallbackActive = true;
    if (m_dvActivationReason == DVBRIDGE::OutputReason::None)
      m_dvActivationReason = m_dvSelection.reason;
  }
  const auto& source = picture.doviSourceInfo;
  const bool ordinaryBL = !source.profile || source.profile == 7 ||
      source.compatibility == 1 || source.compatibility == 2 || source.compatibility == 4;
  if (m_dvSelection.route == DVBRIDGE::OutputRoute::Vanilla && capabilities.processingKnown &&
      !ordinaryBL && preferences.output != DVBRIDGE::OutputMode::Disabled &&
      !(source.el && !picture.doviEnhancement))
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    if (!m_dvFatalConsumed)
      m_dvFatalReason = m_dvSelection.reason == DVBRIDGE::OutputReason::None ?
          DVBRIDGE::OutputReason::SourceIneligible : m_dvSelection.reason;
    return false;
  }
  if (!PollRejectedDVSurface()) return false;
  // A format match cannot carry a fixed target across original source streams.
  if (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 && m_hdr10Session &&
      m_hdr10SessionStream != picture.doviMetadata.Binding().stream)
    return ConfigureHDR10Conversion(picture);
  if (m_dvConfiguredOutput == m_dvSelection.policy.mode && !m_dvOutputRetiring &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::Vanilla || m_dvState.Captured() || m_hdrConversion))
    return true;
  const bool requested = DVBridgeRequested(&picture);
  if (requested && m_dvBridge && !HasPendingDVOutputSurface() &&
      (m_dvState.Captured() || m_hdrConversion) &&
      (m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10) == m_hdrConversion)
  {
    // Same-format policies keep the renderer and its committed history intact.
    if (!CompleteDVBridgePage()) return false;
    m_dvConfiguredOutput = m_dvSelection.policy.mode;
    return true;
  }
  if (!HasPendingDVOutputSurface() && !SetVideoOutput(&picture)) return false;
  if (requested)
  {
    if (CDVBridgeGLES::Context::NewContextBlocked() ||
        (m_dvContext && m_dvContext->GL() && m_dvContext->Poll() != DVBRIDGE_DV_IDLE))
      return false;
    const bool ready = m_dvSelection.route == DVBRIDGE::OutputRoute::HDR10 ?
        ConfigureHDR10Conversion(picture) : BeginDVBridge();
    if (!ready) { RejectDVOutputSurface(); return false; }
  }
  else
  {
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    if (!drm || (m_dvState.Captured() && !m_dvState.Stage(*drm,true)) ||
        (m_hdrDeepColorPrevious && !drm->AddProperty(drm->GetConnector(),
            "DVBRIDGE_HDR10_DEEP_COLOR",*m_hdrDeepColorPrevious)))
    { RejectDVOutputSurface(); return false; }
    SetColorimetry(&picture);
    SetHDR(&picture);
    SetGuiCompositing(picture.color_transfer);
    m_dvOrdinaryFrame = true;
  }
  m_dvConfiguredOutput = m_dvSelection.policy.mode;
  return true;
}

bool CWinSystemGbmGLESContext::RejectDVOutputSurface()
{
  if (m_dvOrdinaryFrame)
  {
    m_dvRollbackFailed = true;
    std::lock_guard lock(m_dvCapabilityMutex);
    m_dvActivationReason = DVBRIDGE::OutputReason::RestoreFailed;
    return false;
  }
  if (!m_dvActivating && !HasPendingDVOutputSurface()) return true;
  if (!m_dvRecoveryPending)
  {
    m_dvRecoveryPending = true;
    m_dvRejectedRoute = m_dvSelection.route;
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
        drm && drm->DVActivationFailures() == m_dvAttemptFailures)
      drm->FailDVActivation(EINVAL);
  }
  if (!HasPendingDVOutputSurface())
  {
    if (m_dvBridge) m_dvBridge->Cancel();
    if (!m_dvContext || !m_dvContext->FenceLastUse()) m_dvRollbackFailed = true;
    return !m_dvRollbackFailed;
  }
  if (!m_dvRejectedSurface)
  {
    m_dvRejectedSurface = true;
    m_dvConfiguredOutput.reset();
    if (m_dvBridge) m_dvBridge->Cancel();
    // The tail covers imports, resolve and the candidate's native swap commands.
    if (!m_dvContext || !m_dvContext->FenceLastUse()) m_dvRollbackFailed = true;
    if (!m_eglContext.BindPriorOutputSurface()) m_dvRollbackFailed = true;
  }
  return PollRejectedDVSurface();
}

bool CWinSystemGbmGLESContext::PollRejectedDVSurface()
{
  if (!m_dvRejectedSurface) return true;
  if (m_dvRollbackFailed || !m_dvContext ||
      m_dvContext->Poll() != DVBRIDGE_DV_IDLE) return false;
  if (!CompleteDVOutputSurface(false))
  {
    m_dvRollbackFailed = true;
    return false;
  }
  // Ordinary restoration may retain a 10-bit drawable without CB1 processing.
  if (m_dvBridge && m_dvPriorConfiguredOutput &&
      !m_dvBridge->RebindOutputSurface(m_dvPriorHDRConversion))
  {
    m_dvRollbackFailed = true;
    return false;
  }
  m_hdrConversion = m_dvPriorHDRConversion;
  m_dvConfiguredOutput = m_dvPriorConfiguredOutput;
  SetGuiCompositing(m_dvPriorGuiCompositing ? AVCOL_TRC_SMPTE2084 : 0);
  m_dvRejectedSurface = false;
  return true;
}

bool KODI::WINDOWING::GBM::HasDVBridgeScreenshot(CWinSystemBase& window)
{
  auto* gbm = dynamic_cast<CWinSystemGbmGLESContext*>(&window);
  return gbm && gbm->HasDVBridgeScreenshot();
}

unsigned int KODI::WINDOWING::GBM::GetDVBridgeScreenshotFramebuffer(CWinSystemBase& window)
{
  auto* gbm = dynamic_cast<CWinSystemGbmGLESContext*>(&window);
  return gbm ? gbm->GetDVBridgeScreenshotFramebuffer() : 0;
}

bool CWinSystemGbmGLESContext::CB1AsyncAvailable() const
{
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  return m_eglFence && drm && drm->CanAsyncDVBridge() && m_dvBridge &&
      !m_dvBridge->Fallback() && !m_dvFallbackActive && !m_dvActivating &&
      !m_dvRecoveryPending && !m_dvRejectedSurface && !m_dvRollbackFailed &&
      !m_dvOutputRetiring && !m_dvShutdownFence && !m_dvStopPending && !HasPendingDVOutputSurface() &&
      m_dvSelection.route != DVBRIDGE::OutputRoute::Vanilla &&
      m_dvSelection.reason == DVBRIDGE::OutputReason::None &&
      (m_hdrConversion || m_dvState.Captured());
}

bool CWinSystemGbmGLESContext::CreateCB1GPUFence(const CDVBridgeGLES::Submission& submission)
{
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  const auto current = m_dvBridge ? m_dvBridge->CaptureSubmission() : CDVBridgeGLES::Submission{};
  if (submission.asyncEligible && current.asyncEligible && submission.sequence &&
      submission.sequence == current.sequence && submission.target == current.target &&
      submission.resolveSerial == current.resolveSerial &&
      submission.identity.stream == current.identity.stream &&
      submission.identity.picture == current.identity.picture &&
      submission.identity.revision == current.identity.revision && CB1AsyncAvailable())
  {
    // The exported fence covers packing as well as source reconstruction.
    m_eglFence->CreateGPUFence();
    glFlush();
    const int fd = m_eglFence->FlushFence();
    if (fd >= 0)
    {
      const int prior = m_DRM->TakeOutFenceFd();
      if (prior >= 0) close(prior);
      m_DRM->SetInFenceFd(fd);
      return true;
    }
    glFinish();
  }
#endif
  return false;
}

bool CWinSystemGbmGLESContext::RenderDVBridge(const CDVBridgeGLES::Layer& layer,
                                            std::shared_ptr<AVFrame> enhancement,
                                            DVBRIDGE::SourceBinding enhancementBinding,
                                            const CDVMetadataBuffer& metadata,
                                            dvbridge_geometry geometry)
{
  // A delayed KMS handoff must not replace the one prepared next picture.
  if (m_cb1FramePending && m_dvFrame) return true;
  m_cb1FramePending = false;
  m_dvFrame = false;
  if (!BeginDVBridge())
    return false;
  const auto capture = CServiceBroker::GetCaptureService();
  const bool capturing = capture && !capture->TakeActive(
      KODI::RENDERING::CAPTURE::CaptureContent::COMPOSITE).empty();
  const bool overlayFree = !capturing &&
      (!m_guiWillRender || GetGUIElementCount() == 0) && !m_dvOverlays;
  const bool subtitlesOnly = !capturing && m_guiWillRender && m_guiFbo.IsValid() &&
      m_guiFboWidth == 3840 && m_guiFboHeight == 2160 && m_compositeShader &&
      m_dvSubtitleDraws && m_dvSubtitleDraws == GetGUIElementCount() &&
      (!m_dvPQSubtitleDraws || m_dvPQSubtitleDraws == m_dvSubtitleDraws);
  const bool pqSubtitles = subtitlesOnly && m_dvPQSubtitleDraws;
  // Prime the subtitle shader once; empty frames retain the lean native shader.
  const bool nativeOverlay = !m_hdrConversion &&
      (subtitlesOnly || (overlayFree && !m_dvBridge->NativeOverlayPrimed())) &&
      m_guiFbo.IsValid() && m_guiFboWidth == 3840 && m_guiFboHeight == 2160 && m_compositeShader;
  const bool nativeSubtitles = m_dvBridge->SetNativeOverlay(
      nativeOverlay ? (pqSubtitles ? m_dvPQSubtitleFbo.Texture() : m_guiFbo.Texture()) : 0,
      nativeOverlay ? m_compositeShader->GetDegammaTexture() : 0,
      nativeOverlay ? m_compositeShader->GetTransferTexture() : 0,
      m_dvSubtitleBounds, m_dvSubtitleContent, pqSubtitles);
  if (std::getenv("CB1_PRESENTATION_AUDIT"))
  {
    static unsigned prior = ~0u;
    const unsigned route = (overlayFree ? 1 : 0) | (nativeSubtitles ? 2 : 0) |
        (pqSubtitles ? 4 : 0) | (GetGUIElementCount() << 4) | (m_dvSubtitleDraws << 16);
    if (route != prior)
    {
      prior = route;
      CLog::Log(LOGINFO, "CB1 subtitle route: gui={} subtitle_draws={} pq_draws={} gui_render={} free={} native={} bounds={},{},{},{}",
          GetGUIElementCount(), m_dvSubtitleDraws, m_dvPQSubtitleDraws, m_guiWillRender, overlayFree, nativeSubtitles,
          m_dvSubtitleBounds.x1, m_dvSubtitleBounds.y1, m_dvSubtitleBounds.x2, m_dvSubtitleBounds.y2);
    }
  }
  m_dvNativeGui = false;
  const auto preferences = DVBRIDGE::FrameOutputSettings();
  const auto policy = m_dvSelection.policy;
  const bool allowAsync = CB1AsyncAvailable() && !capturing;
#ifdef CB1_HAS_HDR10_AI
  const bool aiFrame = !metadata.Data();
  cb1_ai_result prediction{};
  if (aiFrame && m_dvBridge->PredictHDR10AI(layer.binding, preferences,
      DVBRIDGE::DiagnosticNowMs(), &prediction) != HDR10AISource::Decision::Prepare)
    return false;
#endif
  const auto prepare = [&]() {
#ifdef CB1_HAS_HDR10_AI
    if (aiFrame)
      return m_dvBridge->PrepareHDR10AIFrame(layer, geometry, preferences,
          prediction, policy, allowAsync);
#endif
    return m_dvBridge->PrepareHardwareFrame(layer, enhancement, enhancementBinding,
        metadata, geometry, preferences, overlayFree || nativeSubtitles, &policy, allowAsync);
  };
#ifdef CB1_HAS_HDR10_AI
  const double refresh = m_DRM->GetCurrentMode().fRefreshRate;
  const auto interval = std::chrono::duration<double>(refresh >= 10 ? 1.0 / refresh : 0.02);
  const auto aiDeadline = std::chrono::steady_clock::now() +
      (capturing ? std::chrono::duration<double>(1.0) : interval);
#endif
  auto status = prepare();
#ifdef CB1_HAS_HDR10_AI
  if (aiFrame)
  {
    // Future analysis may advance, but cannot withhold a ready current picture.
    for (;;)
    {
      const auto analysis = m_dvBridge->CollectHDR10AI();
      if (analysis != CB1_AI_READY && analysis != CB1_AI_PENDING)
        return false;
      const bool renderPending = status == CDVBridgeGLES::FrameStatus::Pending ||
          status == CDVBridgeGLES::FrameStatus::Busy;
      if ((!renderPending && status != CDVBridgeGLES::FrameStatus::Ready &&
           status != CDVBridgeGLES::FrameStatus::Reused) ||
          !renderPending || std::chrono::steady_clock::now() >= aiDeadline)
        break;
      if (renderPending) status = prepare();
      if (status == CDVBridgeGLES::FrameStatus::Pending ||
          status == CDVBridgeGLES::FrameStatus::Busy)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  else
#endif
  if (capturing)
  {
    // A one-shot capture must finish preparing before Kodi's composite tap.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((status == CDVBridgeGLES::FrameStatus::Pending ||
            status == CDVBridgeGLES::FrameStatus::Busy) &&
           std::chrono::steady_clock::now() < deadline)
    {
      if (status == CDVBridgeGLES::FrameStatus::Pending)
      {
        if (m_dvBridge->PollReady())
          status = CDVBridgeGLES::FrameStatus::Ready;
      }
      else
        status = prepare();
      if (status != CDVBridgeGLES::FrameStatus::Ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  // A paused final surface is already committed scanout, not a fresh resolve.
  const bool reused = status == CDVBridgeGLES::FrameStatus::Reused;
  m_dvFrame = status == CDVBridgeGLES::FrameStatus::Ready;
  m_dvNativeGui = (m_dvFrame || reused) && m_dvBridge->NativeOverlayComposed();
  if (m_dvFrame && m_hdrConversion)
  {
    if (const auto* policy = m_dvBridge->HDR10Output())
    {
      VideoPicture output;
      output.color_transfer = AVCOL_TRC_SMPTE2084;
      output.color_primaries = AVCOL_PRI_BT2020;
      output.color_space = AVCOL_SPC_BT2020_NCL;
      output.colorBits = 10;
      output.hasDisplayMetadata = true;
      output.displayMetadata = {};
      output.displayMetadata.has_luminance = true;
      output.displayMetadata.max_luminance = {static_cast<int>(policy->hdr10.max_luminance),1};
      output.displayMetadata.min_luminance = {static_cast<int>(policy->hdr10.min_luminance),10000};
      output.hasLightMetadata = true;
      output.lightMetadata = {policy->hdr10.max_cll,policy->hdr10.max_fall};
      SetColorimetry(&output);
      m_dvFrame = SetHDR(&output);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  m_cb1FramePending = allowAsync && m_dvContext->AsyncPreparation(layer.binding.stream) &&
      (m_dvFrame || status == CDVBridgeGLES::FrameStatus::Pending ||
       status == CDVBridgeGLES::FrameStatus::Busy);
  return m_dvFrame || reused;
}

void CWinSystemGbmGLESContext::RecordDVSubtitle(const CRect& rect, unsigned texture)
{
  if (!m_guiCompositing || !m_guiWillRender || rect.IsEmpty())
    return;
  if (!m_dvSubtitleDraws++)
    m_dvSubtitleBounds = rect;
  else
    m_dvSubtitleBounds.Union(rect);
  // A reused glyph texture keeps a paused picture reusable. Position/fade edits rekey it.
  m_dvSubtitleContent = (m_dvSubtitleContent ^ texture) * 1099511628211ULL;
  for (float value : {rect.x1, rect.y1, rect.x2, rect.y2})
    m_dvSubtitleContent = (m_dvSubtitleContent ^ std::bit_cast<uint32_t>(value)) * 1099511628211ULL;
  if (m_dvPQSubtitleRendering)
    ++m_dvPQSubtitleDraws;
}

bool CWinSystemGbmGLESContext::BeginDVPQSubtitle()
{
  if (!HasDVBridgeScreenshot() || !m_guiCompositing || !m_guiWillRender)
    return false;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_dvPQPreviousFbo);
  if (!m_dvPQSubtitleFbo.IsValid() || m_dvPQSubtitleWidth != m_guiFboWidth ||
      m_dvPQSubtitleHeight != m_guiFboHeight)
  {
    m_dvPQSubtitleFbo.Cleanup();
    if (!m_dvPQSubtitleFbo.Initialize() ||
        !m_dvPQSubtitleFbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_guiFboWidth,
                                                m_guiFboHeight, GL_RGBA16F, GL_HALF_FLOAT) ||
        (GetEnabledFrontToBackRendering() &&
         !m_dvPQSubtitleFbo.AttachDepthBuffer(m_guiFboWidth, m_guiFboHeight)))
    {
      CLog::Log(LOGERROR, "CB1: PQ subtitle framebuffer allocation failed");
      m_dvPQSubtitleFbo.Cleanup();
      glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
      return false;
    }
    m_dvPQSubtitleWidth = m_guiFboWidth;
    m_dvPQSubtitleHeight = m_guiFboHeight;
  }
  if (!m_dvPQSubtitleFbo.BeginRender())
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
    return false;
  }
  if (!m_dvPQSubtitleCleared)
  {
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    m_dvPQSubtitleCleared = true;
  }
  m_dvPQSubtitleRendering = true;
  return true;
}

void CWinSystemGbmGLESContext::EndDVPQSubtitle()
{
  m_dvPQSubtitleRendering = false;
  glBindFramebuffer(GL_FRAMEBUFFER, m_dvPQPreviousFbo);
}

void CWinSystemGbmGLESContext::DiscardDVBridgeFrame()
{
  // Discard unsubmitted work; accepted KMS buffers retain their retirement owner.
  if (m_dvBridge) m_dvBridge->Cancel();
  m_cb1FramePending = m_dvFrame = m_dvNativeGui = false;
}

bool CWinSystemGbmGLESContext::StopDVBridgePlayback()
{
  // The window outlives the renderer when a pending scanout defers teardown.
  m_dvStopPending = true;
  if (!EndDVBridge()) return false;
  SetGuiCompositing(false);
  SetHDR(nullptr);
  SetColorimetry(nullptr);
  if (!SetVideoOutput(nullptr)) return false;
  m_dvStopPending = false;
  return true;
}

bool CWinSystemGbmGLESContext::EndDVBridge()
{
  if (!CompleteDVBridgePage())
  {
    m_dvEndPending = true;
    return false;
  }
  if (m_failedHDR10SetupRestore && !RestoreFailedHDR10Setup())
    return false;
  if (m_hdrDeepColorPrevious)
  {
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    if (!drm)
      return false;
    if (!drm->AddProperty(drm->GetConnector(), "DVBRIDGE_HDR10_DEEP_COLOR",
                          *m_hdrDeepColorPrevious))
    {
      drm->FlipPageTransactional(nullptr, true);
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
    if (!m_eglContext.TrySwapBuffers())
      return false;
    auto& surface = m_GBM->GetDevice().GetSurface();
    auto* bo = surface.LockTransactionalBuffer();
    const bool presented = drm->FlipPageTransactional(bo, true);
    surface.CompleteTransactionalBuffer(presented);
    if (!presented)
      return false;
    surface.EndTransactionalMode();
    m_hdrDeepColorPrevious.reset();
    if (DVBRIDGE::DiagnosticsEnabled())
      CLog::Log(LOGINFO, "DVBridge HDR10: native link policy restored");
  }
  m_cb1FramePending = m_dvFrame = false;
  // Never reset Kodi's ordinary HDR GUI compositor unless we owned DV output.
  if (!m_dvState.Captured() && !m_hdrConversion)
  {
    m_dvEndPending = false;
    return true;
  }
  if (m_dvState.Captured())
  {
    // Restore signaling with a clean ordinary framebuffer, never tunnel pixels.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_dvBridge || !m_dvBridge->Advanced()) glFinish();
    if (!m_eglContext.TrySwapBuffers() || !FlipDVBridgePage(m_dvState, true))
    {
      if (DVBRIDGE::DiagnosticsEnabled() &&
          m_dvSetupFailures.Record(DVBRIDGE::DiagnosticNowMs(), "restore"))
        CLog::Log(LOGERROR, "DVBridge: output restoration failed; retaining scanout state; failures={}",
            m_dvSetupFailures.Count());
      return false;
    }
  }
  if (m_hdrConversion)
    m_dvBridge.reset();
  else if (m_dvBridge)
    m_dvBridge->Reset();
  m_hdr10Session.reset();
  m_hdr10SessionStream = 0;
  m_hdrConversion = false;
  if (DVBRIDGE::DiagnosticsEnabled() &&
      m_dvSetupFailures.Recovered(DVBRIDGE::DiagnosticNowMs()))
    CLog::Log(LOGINFO, "DVBridge: normal display restored after failures={}", m_dvSetupFailures.Count());
  SetGuiCompositing(0);
  m_dvEndPending = false;
  return true;
}

bool CWinSystemGbmGLESContext::DestroyWindowSystem()
{
  if (!EndDVBridge())
    return false;
  m_dvBridge.reset();
  if (!RetireDVContext()) return false;
  if (!CWinSystemGbmEGLContext::DestroyWindowSystem()) return false;
  std::lock_guard lock(m_dvCapabilityMutex);
  const auto generation = m_dvCapabilities.generation + 1;
  m_dvCapabilities = {};
  m_dvAvailableNativeDV = m_dvAvailableHDR10 = false;
  m_dvCapabilities.generation = generation;
  m_dvImportBinding = {};
  return true;
}

bool CWinSystemGbmGLESContext::RetireDVContext()
{
  if (!CompleteDVBridgePage()) return false;
  if (m_dvBridge) m_dvBridge->Cancel();
  m_dvBridge.reset();
  if (!m_dvContext || !m_dvContext->GL()) return true;
  if (!m_dvShutdownFence)
  {
    m_dvShutdownFence = true;
    if (!m_dvContext->FenceLastUse()) return false;
  }
  if (m_dvContext->Poll() != DVBRIDGE_DV_IDLE || !m_dvContext->Close()) return false;
  // Retain the per-video setting snapshot across graphics-context recreation.
  m_dvShutdownFence = false;
  return true;
}

bool CWinSystemGbmGLESContext::DestroyWindow()
{
  if (!EndDVBridge() || !RetireDVContext()) return false;
  if (!CWinSystemGbmEGLContext::DestroyWindow()) return false;
  ObserveDVImport({}, {}, false);
  return true;
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
#ifdef HAVE_DVBRIDGE
  {
    std::lock_guard lock(m_dvCapabilityMutex);
    const auto generation = m_dvCapabilities.generation + 1;
    m_dvCapabilities = {};
    m_dvAvailableNativeDV = m_dvAvailableHDR10 = false;
    m_dvCapabilities.generation = generation;
    m_dvImportBinding = {};
    m_dvImportUsable = false;
  }
#endif
  return true;
}
