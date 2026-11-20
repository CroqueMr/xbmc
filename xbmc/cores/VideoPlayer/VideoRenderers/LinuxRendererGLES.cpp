/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "LinuxRendererGLES.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#endif

#include "RenderFactory.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "VideoShaders/VideoFilterShaderGLES.h"
#include "VideoShaders/YUV2RGBShaderGLES.h"
#include "VideoShaders/dither.h"
#include "application/Application.h"
#include "cores/IPlayer.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "guilib/Texture.h"
#include "rendering/GLExtensions.h"
#include "rendering/MatrixGL.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/GLUtils.h"
#include "utils/HDRUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoSourceSideData.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10SourceType.h"
#ifdef CB1_HAS_HDR10_AI
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10AIBuffers.h"
#endif
#include "UpscalerSettings.h"
#include "ShaderUpscalerPresets.h"
#include "windowing/gbm/WinSystemGbmGLESContext.h"

extern "C"
{
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
}

#include <array>
#include <iterator>
#endif

#include <algorithm>
#include <mutex>

extern "C"
{
#include <libavutil/dovi_meta.h>
}

using namespace Shaders;
using namespace Shaders::GLES;

#ifdef HAVE_DVBRIDGE
struct CLinuxRendererGLES::ShaderUpscaler
{
  pl_opengl gl{};
  pl_renderer renderer{};
  std::vector<const pl_hook*> hooks;
  int preset{-1};
  bool ready{};

  ~ShaderUpscaler()
  {
    KODI::VIDEO::UPSCALER::DestroyHooks(hooks);
    if (renderer)
      pl_renderer_destroy(&renderer);
    if (gl)
      pl_opengl_destroy(&gl);
  }

  bool Load(int method)
  {
    preset = method;
    pl_opengl_params context{};
    context.get_proc_addr = reinterpret_cast<pl_voidfunc_t (*)(const char*)>(eglGetProcAddress);
    context.egl_display = eglGetCurrentDisplay();
    context.egl_context = eglGetCurrentContext();
    gl = pl_opengl_create(nullptr, &context);
    if (!gl)
      return false;
    renderer = pl_renderer_create(nullptr, gl->gpu);
    if (!renderer)
      return false;
    return KODI::VIDEO::UPSCALER::LoadHooks(gl->gpu, method, hooks);
  }
};
#endif

CLinuxRendererGLES::CLinuxRendererGLES()
{
  m_format = AV_PIX_FMT_NONE;

  m_fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();

  m_renderSystem = dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());

  std::tie(m_useDithering, m_ditherDepth) = CServiceBroker::GetWinSystem()->GetDitherSettings();
  if (m_useDithering)
  {
    if (m_renderSystem && !CGLExtensions::IsExtensionSupported(CGLExtensions::EXT_texture_norm16))
    {
      CLog::Log(LOGWARNING,
                "GLES: dithering requested but GL_EXT_texture_norm16 not supported, disabling");
      m_useDithering = false;
    }
    else
    {
#if defined(GL_R16_EXT)
      glGenTextures(1, &m_ditherTex);
      glActiveTexture(GL_TEXTURE3);
      glBindTexture(GL_TEXTURE_2D, m_ditherTex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R16_EXT, dither_size, dither_size, 0, GL_RED,
                   GL_UNSIGNED_SHORT, dither_matrix);
      glActiveTexture(GL_TEXTURE0);
      if (m_ditherDepth == 0)
        CLog::Log(LOGDEBUG, "GLES: dithering auto");
      else
        CLog::Log(LOGDEBUG, "GLES: dithering enabled, depth={}", m_ditherDepth);
#else
      // GL_EXT_texture_norm16 not exposed by this GL header (e.g. iOS):
      // no 16-bit single-channel format available for the dither texture.
      CLog::Log(LOGWARNING, "GLES: GL_R16_EXT undefined at build time; dithering disabled");
      m_useDithering = false;
#endif
    }
  }

#if defined(GL_ES_VERSION_3_0)
  int32_t intermediatePrecision = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_VIDEOPLAYER_HQSCALERPRECISION);
  if (intermediatePrecision == 16)
  {
    m_intermediateFormat = GL_RGBA16F;
    m_intermediateType = GL_FLOAT;
  }
  else if (intermediatePrecision == 10)
  {
    m_intermediateFormat = GL_RGB10_A2;
    m_intermediateType = GL_UNSIGNED_INT_2_10_10_10_REV;
  }
  CLog::Log(LOGDEBUG, "GLES: HQ scaler precision={}, intermediate format={:#x}",
            intermediatePrecision, static_cast<unsigned>(m_intermediateFormat));
#endif

#if defined (GL_UNPACK_ROW_LENGTH_EXT)
  if (CGLExtensions::IsExtensionSupported(CGLExtensions::EXT_unpack_subimage))
  {
    m_pixelStoreKey = GL_UNPACK_ROW_LENGTH_EXT;
  }
#endif
}

CLinuxRendererGLES::~CLinuxRendererGLES()
{
  UnInit();

  ReleaseShaders();

  if (m_ditherTex)
    glDeleteTextures(1, &m_ditherTex);

  free(m_planeBuffer);
  m_planeBuffer = nullptr;
}

CBaseRenderer* CLinuxRendererGLES::Create(CVideoBuffer *buffer)
{
  return new CLinuxRendererGLES();
}

bool CLinuxRendererGLES::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("default", CLinuxRendererGLES::Create);
  return true;
}

bool CLinuxRendererGLES::ValidateRenderTarget()
{
  if (!m_bValidated)
  {
    // function pointer for texture might change in
    // call to LoadShaders
    glFinish();

    for (int i = 0 ; i < MAX_RENDER_BUFFERS ; i++)
    {
      DeleteTexture(i);
    }

    // trigger update of video filters
    m_scalingMethodGui = VS_SCALINGMETHOD_MAX;

    // create the yuv textures
    UpdateVideoFilter();
    LoadShaders();

    if (m_renderMethod < 0)
    {
      return false;
    }

    for (int i = 0; i < m_NumYUVBuffers; i++)
    {
      CreateTexture(i);
    }

    m_bValidated = true;

    return true;
  }

  return false;
}

#ifdef CB1_HAS_HDR10_AI
namespace
{
DVBRIDGE::HDR10SourceMode SoftwareHDR10AIMode(StreamHdrType type)
{
  if (type == StreamHdrType::HDR_TYPE_HLG)
    return DVBRIDGE::hlgPlaybackMode.load();
  if (type == StreamHdrType::HDR_TYPE_HDR10PLUS)
    return DVBRIDGE::hdr10PlusPlaybackMode.load();
  return DVBRIDGE::hdr10PlaybackMode.load();
}

bool SoftwareHLG8(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != AV_PIX_FMT_YUV420P || !picture.IsLimitedHLG8())
    return false;
  unsigned int major, minor;
  CServiceBroker::GetRenderSystem()->GetRenderVersion(major, minor);
  return major >= 3;
}

unsigned SoftwareHDR10AIBufferCount(const VideoPicture& picture, float fps, unsigned orientation)
{
  const auto format = picture.videoBuffer->GetFormat();
  const bool planar = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_YUV444P10LE ||
      SoftwareHLG8(picture);
  const bool supported = planar && orientation == 0 && !picture.doviSourceInfo.profile &&
      picture.stereoMode.empty() && picture.iWidth <= 3840 && picture.iHeight <= 2160;
  const auto mode = SoftwareHDR10AIMode(picture.hdrType);
  const bool source = supported &&
      DVBRIDGE::IsHDR10AIInput(picture.hdrType, mode == DVBRIDGE::HDR10SourceMode::HDR10PlusDV);
  return DVBRIDGE::HDR10AIBuffers::Count(source, mode, fps, picture.iFlags & DVP_FLAG_INTERLACED);
}
}
#endif

bool CLinuxRendererGLES::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  return Configure(picture, fps, orientation, RetainedOutput::None);
}

bool CLinuxRendererGLES::Configure(const VideoPicture& picture, float fps,
                                   unsigned int orientation, RetainedOutput retainedOutput)
{
  auto colorimetry = KODI::UTILS::GetColorimetry(picture);
  bool inferred = (picture.color_space == AVCOL_SPC_UNSPECIFIED);
  CLog::Log(LOGDEBUG,
            "LinuxRendererGLES::Configure: fps: {:0.3f}, {}x{}, colorimetry: {}{}, {}bit, {} range",
            fps, picture.iWidth, picture.iHeight, KODI::UTILS::ColorimetryToString(colorimetry),
            inferred ? " (inferred)" : "", picture.colorBits,
            picture.color_range == 1 ? "full" : "limited");
  m_format = picture.videoBuffer->GetFormat();
#ifdef CB1_HAS_HDR10_AI
  m_planarHLG8 = SoftwareHLG8(picture);
#endif

  // CPU-upload renderer: HWACCEL pix_fmts (e.g. AV_PIX_FMT_DRM_PRIME) have no host planes.
  if (GetShaderFormat() == SHADER_NONE)
  {
    CLog::Log(LOGDEBUG, "LinuxRendererGLES::Configure: refusing unsupported pix_fmt {}",
              m_format == AV_PIX_FMT_NONE ? "none" : av_get_pix_fmt_name(m_format));
    return false;
  }

  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;
  m_fps = fps;

  m_iFlags =
      GetFlagsChromaPosition(picture.chroma_position) | GetFlagsStereoMode(picture.stereoMode);

  m_srcPrimaries = picture.color_primaries;
  m_srcColorBits = picture.colorBits;
  m_srcFullRange = picture.color_range == 1;
  m_toneMap = false;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  m_bConfigured = true;
  m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  m_scalingMethod = m_videoSettings.m_ScalingMethod;
#ifdef HAVE_DVBRIDGE
  m_reportedUpscaler = -2;
#endif

  // Ensure that textures are recreated and rendering starts only after the 1st
  // frame is loaded after every call to Configure().
  m_bValidated = false;

#ifdef CB1_HAS_HDR10_AI
  if (retainedOutput == RetainedOutput::HDR10)
  {
    auto& window = static_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext&>(
        *CServiceBroker::GetWinSystem());
    m_passthroughHDR = true;
    m_hdrFboActive = window.IsHdrComposite();
    m_reloadShaders = true;
    window.BeginHDR10Handoff();
    return true;
  }
  m_softwareAIBuffers = SoftwareHDR10AIBufferCount(picture, fps, orientation);
  m_aiLookahead = HDR10AIQueueEligible();
  m_aiOutputPending = HDR10AILookahead() &&
      SoftwareHDR10AIMode(picture.hdrType) != DVBRIDGE::HDR10SourceMode::Native;
  if (m_aiOutputPending)
  {
    // An already committed HDR10-to-DV output can outlive the renderer.
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    m_dvRequested = window && window->OwnsHDR10DVOutput();
    m_passthroughHDR = m_hdrFboActive = false;
    return true;
  }
#endif
#ifdef HAVE_DVBRIDGE
  if (retainedOutput == RetainedOutput::NativeDV)
  {
    // The renderer owns cleanup now. Source admission happens after import.
    m_dvRequested = m_hdrFboActive = true;
    m_passthroughHDR = false;
    CLog::Log(LOGDEBUG, "DVBridge: retained Master Reference HDMI output across renderer replacement");
    return true;
  }
#endif
  return ConfigureVideoOutput(picture);
}

bool CLinuxRendererGLES::ConfigureVideoOutput(const VideoPicture& picture)
{
#ifdef CB1_HAS_HDR10_AI
  if (m_aiOutputPending &&
      KODI::WINDOWING::GBM::CWinSystemGbmGLESContext::DVBridgeRequested(&picture))
  {
    // Queue preparation already owns the final surface and its first DV frame.
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    m_dvRequested = true;
    m_passthroughHDR = false;
    m_hdrFboActive = window && window->BeginDVBridge();
    m_bConfigured = m_hdrFboActive;
    return m_hdrFboActive;
  }
#endif
  if (!CServiceBroker::GetWinSystem()->SetVideoOutput(&picture))
  {
    CLog::Log(LOGWARNING, "LinuxRendererGLES::Configure: SetVideoOutput failed");
#ifdef HAVE_DVBRIDGE
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    if (KODI::WINDOWING::GBM::CWinSystemGbmGLESContext::DVBridgeRequested(&picture) ||
        (window && window->HasPendingDVBridgeRestore()))
    {
      m_bConfigured = false;
      return false;
    }
#endif
  }


#ifdef HAVE_DVBRIDGE
  m_dvRequested = KODI::WINDOWING::GBM::CWinSystemGbmGLESContext::DVBridgeRequested(&picture);
  if (m_dvRequested)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    const auto route = window ? window->SelectedDVOutput(&picture).route
                              : DVBRIDGE::OutputRoute::Vanilla;
    // The TV-led transport publishes its own HDMI colour state in the first
    // DV commit. Staging the base layer here can switch the connector twice.
    if (route != DVBRIDGE::OutputRoute::NativeDV)
      CServiceBroker::GetWinSystem()->SetColorimetry(&picture);
    m_passthroughHDR = false;
    // Native activation follows source import and its output-surface transaction.
    m_hdrFboActive = window &&
        (route != DVBRIDGE::OutputRoute::HDR10 || window->ConfigureHDR10Conversion(picture));
    if (!m_hdrFboActive)
    {
      m_bConfigured = false;
      CLog::Log(LOGERROR, "DVBridge: native output initialization rejected");
      return false;
    }
    m_bConfigured = true;
    return true;
  }
#endif

  CServiceBroker::GetWinSystem()->SetColorimetry(&picture, true);
  m_passthroughHDR = CServiceBroker::GetWinSystem()->SetHDR(&picture);
  CLog::Log(LOGDEBUG, "LinuxRendererGLES::Configure: HDR passthrough: {}",
            m_passthroughHDR ? "on" : "off");

  m_hdrFboActive =
      m_passthroughHDR && CServiceBroker::GetWinSystem()->SetGuiCompositing(picture.color_transfer);
  if (m_passthroughHDR && !m_hdrFboActive)
    CLog::Log(LOGWARNING, "LinuxRendererGLES::Configure: HDR passthrough active but GUI "
                          "compositing not supported by windowing system");

  // AI priming may have compiled the ordinary shader before HDR was selected.
  m_reloadShaders = true;
  m_bConfigured = true;
  return true;
}

#ifdef CB1_HAS_HDR10_AI
void CLinuxRendererGLES::PreserveHDR10AIOutputOnReconfigure(const VideoPicture& picture)
{
  // Growing the software analysis queue must keep native HLG until the DV frame is ready.
  m_preserveHLGOutput = picture.hdrType == StreamHdrType::HDR_TYPE_HLG &&
      m_passthroughHDR && !m_dvRequested && picture.videoBuffer->GetFormat() == m_format &&
      SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers;
}
#endif

bool CLinuxRendererGLES::ConfigChanged(const VideoPicture &picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
  {
    return true;
  }

#ifdef CB1_HAS_HDR10_AI
  // Kodi can rebuild the queue when Quick Switch first enables analysis.
  return SoftwareHLG8(picture) != m_planarHLG8 ||
      SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers;
#else
  return false;
#endif
}

int CLinuxRendererGLES::NextYUVTexture()
{
  return (m_iYUVRenderBuffer + 1) % m_NumYUVBuffers;
}

void CLinuxRendererGLES::AddVideoPicture(const VideoPicture &picture, int index)
{
  CPictureBuffer &buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CLog::LogF(LOGERROR, "unreleased video buffer");
    buf.videoBuffer->Release();
  }
  buf.videoBuffer = picture.videoBuffer;
  buf.videoBuffer->Acquire();
  buf.loaded = false;
  buf.doviMetadata = picture.doviMetadata;
#ifdef HAVE_DVBRIDGE
  buf.informationGeneration = picture.sourceSideData ? picture.sourceSideData->Generation() : 0;
  buf.doviEnhancement = picture.doviEnhancement;
  buf.doviEnhancementBinding = picture.doviEnhancementBinding;
  if (picture.doviSourceBinding.Valid() || picture.doviMetadata.Data())
  {
    buf.doviSourcePicture = std::make_shared<VideoPicture>();
    buf.doviSourcePicture->CopyRef(picture);
  }
  else buf.doviSourcePicture.reset();
#endif
  buf.doviPts = picture.doviSourcePts == AV_NOPTS_VALUE ? DVD_NOPTS_VALUE :
                  static_cast<double>(picture.doviSourcePts);
  buf.m_srcPrimaries = picture.color_primaries;
  buf.m_srcColSpace = picture.color_space;
  buf.m_srcColTransfer = picture.color_transfer;
  buf.m_srcFullRange = picture.color_range == 1;
  buf.m_srcBits = picture.colorBits;

  buf.hasDisplayMetadata = picture.hasDisplayMetadata;
  buf.displayMetadata = picture.displayMetadata;
  buf.lightMetadata = picture.lightMetadata;
  buf.hasHDR10LightMetadata = picture.hasLightMetadata;
  buf.hdrType = picture.hdrType;
  if (picture.hasLightMetadata && picture.lightMetadata.MaxCLL)
  {
    buf.hasLightMetadata = picture.hasLightMetadata;
  }
}

void CLinuxRendererGLES::ReleaseBuffer(int idx)
{
  CPictureBuffer &buf = m_buffers[idx];
  buf.doviMetadata.Reset();
#ifdef HAVE_DVBRIDGE
  buf.doviEnhancement.reset();
  buf.doviEnhancementBinding = {};
  buf.doviSourcePicture.reset();
#endif
  buf.doviPts = DVD_NOPTS_VALUE;
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

void CLinuxRendererGLES::CalculateTextureSourceRects(int source, int num_planes)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  // calculate the source rectangle
  for(int field = 0; field < 3; field++)
  {
    for(int plane = 0; plane < num_planes; plane++)
    {
      CYuvPlane& p = buf.fields[field][plane];

      p.rect = m_sourceRect;
      p.width  = im->width;
      p.height = im->height;

      if(field != FIELD_FULL)
      {
        // correct for field offsets and chroma offsets
        float offset_y = 0.5;
        if(plane != 0)
        {
          offset_y += 0.5f;
        }

        if(field == FIELD_BOT)
        {
          offset_y *= -1;
        }

        p.rect.y1 += offset_y;
        p.rect.y2 += offset_y;

        // half the height if this is a field
        p.height  *= 0.5f;
        p.rect.y1 *= 0.5f;
        p.rect.y2 *= 0.5f;
      }

      if(plane != 0)
      {
        p.width   /= 1 << im->cshift_x;
        p.height  /= 1 << im->cshift_y;

        p.rect.x1 /= 1 << im->cshift_x;
        p.rect.x2 /= 1 << im->cshift_x;
        p.rect.y1 /= 1 << im->cshift_y;
        p.rect.y2 /= 1 << im->cshift_y;
      }

      // protect against division by zero
      if (p.texheight == 0 || p.texwidth == 0 ||
          p.pixpertex_x == 0 || p.pixpertex_y == 0)
      {
        continue;
      }

      p.height  /= p.pixpertex_y;
      p.rect.y1 /= p.pixpertex_y;
      p.rect.y2 /= p.pixpertex_y;
      p.width   /= p.pixpertex_x;
      p.rect.x1 /= p.pixpertex_x;
      p.rect.x2 /= p.pixpertex_x;

      if (m_textureTarget == GL_TEXTURE_2D)
      {
        p.height  /= p.texheight;
        p.rect.y1 /= p.texheight;
        p.rect.y2 /= p.texheight;
        p.width   /= p.texwidth;
        p.rect.x1 /= p.texwidth;
        p.rect.x2 /= p.texwidth;
      }
    }
  }
}

void CLinuxRendererGLES::LoadPlane(CYuvPlane& plane, int type,
                                   unsigned width, unsigned height,
                                   int stride, int bpp, void* data)
{
  const GLvoid *pixelData = data;
  int bps = bpp * KODI::UTILS::GL::glFormatElementByteCount(type);

  glBindTexture(m_textureTarget, plane.id);

  bool pixelStoreChanged = false;
  if (stride != static_cast<int>(width * bps))
  {
    if (m_pixelStoreKey > 0)
    {
      pixelStoreChanged = true;
      glPixelStorei(m_pixelStoreKey, stride / bps);
    }
    else
    {
      size_t planeSize = width * height * bps;
      if (m_planeBufferSize < planeSize)
      {
        m_planeBuffer = static_cast<unsigned char*>(realloc(m_planeBuffer, planeSize));
        m_planeBufferSize = planeSize;
      }

      unsigned char *src(static_cast<unsigned char*>(data)),
                    *dst(m_planeBuffer);

      for (unsigned int y = 0; y < height; ++y, src += stride, dst += width * bps)
        memcpy(dst, src, width * bps);

      pixelData = m_planeBuffer;
    }
  }
  GLenum datatype = (bpp == 2) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
  glTexSubImage2D(m_textureTarget, 0, 0, 0, width, height, type, datatype, pixelData);

  if (m_pixelStoreKey > 0 && pixelStoreChanged)
    glPixelStorei(m_pixelStoreKey, 0);

  // check if we need to load any border pixels
  if (height < plane.texheight)
  {
    glTexSubImage2D(m_textureTarget, 0, 0, height, width, 1, type, datatype,
                    static_cast<const unsigned char*>(pixelData) + stride * (height - 1));
  }

  if (width  < plane.texwidth)
  {
    glTexSubImage2D(m_textureTarget, 0, width, 0, 1, height, type, datatype,
                    static_cast<const unsigned char*>(pixelData) + bps * (width - 1));
  }

  glBindTexture(m_textureTarget, 0);
}

bool CLinuxRendererGLES::Flush(bool saveBuffers)
{
  glFinish();

  // When asked to save buffers, leave the currently held textures/picture
  // references alone so the renderer can keep presenting the last frame.
  if (!saveBuffers)
  {
#ifdef HAVE_DVBRIDGE
    if (m_dvRequested)
      if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
              CServiceBroker::GetWinSystem()))
        window->DiscardDVBridgeFrame();
#endif
    for (int i = 0; i < m_NumYUVBuffers; i++)
    {
      DeleteTexture(i);
    }

    m_iYUVRenderBuffer = 0;

    // Only invalidate the render target when we actually tore the buffers
    // down above.
    m_bValidated = false;
  }

  glFinish();
  m_fbo.fbo.Cleanup();

  return saveBuffers;
}

void CLinuxRendererGLES::Update()
{
  if (!m_bConfigured)
  {
    return;
  }

  ManageRenderArea();
  ValidateRenderTarget();
}

void CLinuxRendererGLES::UpdateActiveArea(int index)
{
  CRect area;
  // The decoder retains only bounded metadata. Without L5, the whole frame is active.
  const auto* metadata =
      reinterpret_cast<const AVDOVIMetadata*>(m_buffers[index].doviMetadata.Data());
  for (int i = 0; metadata && i < metadata->num_ext_blocks; ++i)
  {
    const AVDOVIDmData* block = av_dovi_get_ext(metadata, i);
    if (block->level != 5)
      continue;
    const unsigned left = block->l5.left_offset, right = block->l5.right_offset;
    const unsigned top = block->l5.top_offset, bottom = block->l5.bottom_offset;
    if (left + right < m_sourceWidth && top + bottom < m_sourceHeight)
      area.SetRect(left, top, m_sourceWidth - right, m_sourceHeight - bottom);
    break;
  }
  SetActiveArea(area);
}

void CLinuxRendererGLES::ClearBackBuffer()
{
  //set the entire backbuffer to black
  //if we do a two pass render, we have to draw a quad. else we might occlude OSD elements.
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetRenderOrder() ==
      RENDER_ORDER_ALL_BACK_TO_FRONT)
  {
    CServiceBroker::GetWinSystem()->GetGfxContext().Clear(0xff000000);
  }
  else
  {
    ClearBackBufferQuad();
  }
}

void CLinuxRendererGLES::ClearBackBufferQuad()
{
  CRect windowRect(0, 0, CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth(),
                   CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight());
  struct Svertex
  {
    float x, y;
  };

  std::vector<Svertex> vertices{
      {windowRect.x1, windowRect.y2 * 2},
      {windowRect.x1, windowRect.y1},
      {windowRect.x2 * 2, windowRect.y1},
  };

  glDisable(GL_BLEND);

  m_renderSystem->EnableGUIShader(ShaderMethodGLES::SM_DEFAULT);
  GLint posLoc = m_renderSystem->GUIShaderGetPos();
  GLint uniCol = m_renderSystem->GUIShaderGetUniCol();
  GLint depthLoc = m_renderSystem->GUIShaderGetDepth();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);
  glUniform1f(depthLoc, -1);

  GLuint vertexVBO;
  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, vertices.size());

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  m_renderSystem->DisableGUIShader();
}

void CLinuxRendererGLES::DrawBlackBars()
{
  CRect windowRect(0, 0, CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth(),
                   CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight());

  auto quads = windowRect.SubtractRect(m_destRect);

  struct Svertex
  {
    float x, y;
  };

  std::vector<Svertex> vertices(6 * quads.size());

  GLubyte count = 0;
  for (const auto& quad : quads)
  {
    vertices[count + 1].x = quad.x1;
    vertices[count + 1].y = quad.y1;

    vertices[count + 0].x = vertices[count + 5].x = quad.x1;
    vertices[count + 0].y = vertices[count + 5].y = quad.y2;

    vertices[count + 2].x = vertices[count + 3].x = quad.x2;
    vertices[count + 2].y = vertices[count + 3].y = quad.y1;

    vertices[count + 4].x = quad.x2;
    vertices[count + 4].y = quad.y2;

    count += 6;
  }

  glDisable(GL_BLEND);

  CRenderSystemGLES* renderSystem =
      dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());
  if (!renderSystem)
    return;

  renderSystem->EnableGUIShader(ShaderMethodGLES::SM_DEFAULT);
  GLint posLoc = renderSystem->GUIShaderGetPos();
  GLint uniCol = renderSystem->GUIShaderGetUniCol();
  GLint depthLoc = m_renderSystem->GUIShaderGetDepth();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);
  glUniform1f(depthLoc, -1);

  GLuint vertexVBO;
  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, vertices.size());

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  renderSystem->DisableGUIShader();
}

void CLinuxRendererGLES::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  m_frameRendered = false;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrameRendered = false;
#endif
  m_iYUVRenderBuffer = index;

  if (!m_bConfigured)
  {
    return;
  }

  if (const bool fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();
      fullRange != m_fullRange)
  {
    m_fullRange = fullRange;
    m_reloadShaders = true;
    m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  }

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
  {
    if (clear) //if clear is set, we're expected to overwrite all backbuffer pixels, even if we have nothing to render
      ClearBackBuffer();

    return;
  }

  if (!IsGuiLayer())
  {
    RenderUpdateVideo(clear, flags, alpha);
    return;
  }

  CPictureBuffer& buf = m_buffers[index];

  if (!buf.fields[FIELD_FULL][0].id)
  {
    return;
  }

  UpdateActiveArea(index);
  ManageRenderArea();

  if (clear)
  {
    if (alpha == 255)
      DrawBlackBars();
    else
    {
      ClearBackBuffer();
    }
  }

  if (alpha < 255)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (m_pYUVProgShader)
    {
      m_pYUVProgShader->SetAlpha(alpha / 255.0f);
    }

    if (m_pYUVBobShader)
    {
      m_pYUVBobShader->SetAlpha(alpha / 255.0f);
    }
  }
  else
  {
    glDisable(GL_BLEND);
    if (m_pYUVProgShader)
    {
      m_pYUVProgShader->SetAlpha(1.0f);
    }

    if (m_pYUVBobShader)
    {
      m_pYUVBobShader->SetAlpha(1.0f);
    }
  }

  const bool rendered = Render(flags, index);
  m_frameRendered = rendered;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrameRendered = rendered;
#endif
  if (!rendered && clear)
    ClearBackBuffer();

  VerifyGLState();
  glEnable(GL_BLEND);
}

void CLinuxRendererGLES::RenderUpdateVideo(bool clear, unsigned int flags, unsigned int alpha)
{
  m_frameRendered = false;
  if (!m_bConfigured)
    return;

  if (const bool fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();
      fullRange != m_fullRange)
  {
    m_fullRange = fullRange;
    m_reloadShaders = true;
    m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  }

  if (IsGuiLayer())
    return;

  int index = m_iYUVRenderBuffer;
  CPictureBuffer& buf = m_buffers[index];

  if (!buf.fields[FIELD_FULL][0].id)
    return;

  UpdateActiveArea(index);
  ManageRenderArea();

  if (clear)
  {
    if (alpha == 255)
      DrawBlackBars();
    else
      ClearBackBuffer();
  }

  if (alpha < 255)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (m_pYUVProgShader)
      m_pYUVProgShader->SetAlpha(alpha / 255.0f);
    if (m_pYUVBobShader)
      m_pYUVBobShader->SetAlpha(alpha / 255.0f);
  }
  else
  {
    glDisable(GL_BLEND);
    if (m_pYUVProgShader)
      m_pYUVProgShader->SetAlpha(1.0f);
    if (m_pYUVBobShader)
      m_pYUVBobShader->SetAlpha(1.0f);
  }

  const bool rendered = Render(flags, index);
  m_frameRendered = rendered;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrameRendered = rendered;
#endif
  if (!rendered && clear)
    ClearBackBuffer();

  VerifyGLState();
  glEnable(GL_BLEND);
}

ESCALINGMETHOD CLinuxRendererGLES::SelectScalingMethod() const
{
  return ResolveScalingMethod(m_videoSettings.m_ScalingMethod);
}

ESCALINGMETHOD CLinuxRendererGLES::ResolveScalingMethod(ESCALINGMETHOD method) const
{
  if(!Supports(method))
  {
    CLog::Log(LOGWARNING,
              "CLinuxRendererGLES::UpdateVideoFilter - chosen scaling method {}, is not supported "
              "by renderer",
              method);
    method = VS_SCALINGMETHOD_LINEAR;
  }

  if (method == VS_SCALINGMETHOD_AUTO)
  {
    bool scaleSD = m_sourceHeight < 720 && m_sourceWidth < 1280;
    bool scaleUp =
        (int)m_sourceHeight < m_viewRect.Height() && (int)m_sourceWidth < m_viewRect.Width();
    bool scaleFps =
        m_fps <
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoAutoScaleMaxFps +
            0.01f;

    if (Supports(VS_SCALINGMETHOD_LANCZOS3_FAST) && scaleSD && scaleUp && scaleFps)
      method = VS_SCALINGMETHOD_LANCZOS3_FAST;
    else
      method = VS_SCALINGMETHOD_LINEAR;
  }

  return method;
}

#ifdef HAVE_DVBRIDGE
ESCALINGMETHOD CLinuxRendererGLES::AutoUpscalingOverride() const
{
  using namespace KODI::VIDEO::UPSCALER;
  if (Get(m_sourceWidth, m_sourceHeight, KODI_DEFAULT) == VS_SCALINGMETHOD_AUTO)
    return ResolveScalingMethod(VS_SCALINGMETHOD_AUTO);
  return VS_SCALINGMETHOD_LINEAR;
}
#endif

void CLinuxRendererGLES::UpdateVideoFilter()
{
  CRect srcRect;
  CRect dstRect;
  CRect viewRect;
  GetVideoRect(srcRect, dstRect, viewRect);

  // TODO: ValidateRenderTarget runs before the display resolution is established
  // (the WHITELIST/resolution ADJUST happens ~500ms after the first RenderUpdate).
  // This causes UpdateVideoFilter to be called with a 0x0 viewport. The proper fix
  // is in CRenderManager to defer rendering until the resolution is ready.
  if (viewRect.Height() == 0 || viewRect.Width() == 0)
    return;

  // TODO: GL also checks nonLinStretchChanged and cmsChanged in the early exit
  // and the reload check below. Add when non-linear stretch and CMS are ported to GLES.
  auto requested = m_videoSettings.m_ScalingMethod;
#ifdef HAVE_DVBRIDGE
  if (!m_dvRequested && KODI::VIDEO::UPSCALER::Enlarges(
          srcRect.Width(), srcRect.Height(), dstRect.Width(), dstRect.Height()))
  {
    const auto override = static_cast<ESCALINGMETHOD>(
        KODI::VIDEO::UPSCALER::Get(m_sourceWidth, m_sourceHeight, requested));
    if (override == requested || Supports(override))
      requested = override;
  }
#endif
  if (m_scalingMethodGui == requested &&
      viewRect.Height() == m_lastViewRect.Height() && viewRect.Width() == m_lastViewRect.Width() &&
      srcRect.Height() == m_lastSourceRect.Height() && srcRect.Width() == m_lastSourceRect.Width())
  {
    return;
  }

  // Viewport-only change doesn't need shader reload -- only method changes do
  if (m_scalingMethod != requested)
    m_reloadShaders = true;

  const ESCALINGMETHOD previousMethod = m_scalingMethod;
  m_scalingMethodGui = requested;
  m_scalingMethod = ResolveScalingMethod(requested);
  m_lastViewRect = viewRect;
  m_lastSourceRect = srcRect;

  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }

  m_fbo.fbo.Cleanup();

  VerifyGLState();

  if (m_scalingMethod != previousMethod)
    m_reloadShaders = true;

  switch (m_scalingMethod)
  {
  case VS_SCALINGMETHOD_NEAREST:
  case VS_SCALINGMETHOD_LINEAR:
  {
    // TODO: GL creates DefaultFilterShader here (or StretchFilterShader for
    // non-linear stretch). Add when non-linear stretch is ported to GLES.
    SetTextureFilter(m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR);
    m_renderQuality = RQ_SINGLEPASS;
    return;
  }
  case VS_SCALINGMETHOD_LANCZOS3_FAST:
  case VS_SCALINGMETHOD_SPLINE36_FAST:
  {
    // On GLES 3.1+, FAST scalers use a single-pass combined YUV+convolution shader
    // (YUV2RGBFilterShader with textureGather). On lower versions, fall through to
    // multi-pass FBO path. Matches GL 4.0+ single-pass / GL <4.0 multi-pass pattern.
    // On GLES, VAAPI maps to SHADER_NV12_RRG (GL maps it to SHADER_NV12).
    // Only patterns the filter shader implements may pass: >8-bit planar
    // (XBMC_YV12_HI) has no sampling block there and takes the multipass path.
    EShaderFormat fmt = GetShaderFormat();
    if (fmt == SHADER_NV12 || fmt == SHADER_NV12_RRG || fmt == SHADER_YV12)
    {
      uint32_t major, minor;
      m_renderSystem->GetRenderVersion(major, minor);
      if (major >= 3 && minor >= 1)
      {
        SetTextureFilter(GL_LINEAR);
        m_renderQuality = RQ_SINGLEPASS;
        return;
      }
    }

    [[fallthrough]];
  }

  case VS_SCALINGMETHOD_FSR1:
  case VS_SCALINGMETHOD_FSR1_RCAS:
  case VS_SCALINGMETHOD_LANCZOS2:
  case VS_SCALINGMETHOD_SPLINE36:
  case VS_SCALINGMETHOD_LANCZOS3:
  case VS_SCALINGMETHOD_CUBIC_B_SPLINE:
  case VS_SCALINGMETHOD_CUBIC_MITCHELL:
  case VS_SCALINGMETHOD_CUBIC_CATMULL:
  case VS_SCALINGMETHOD_CUBIC_0_075:
  case VS_SCALINGMETHOD_CUBIC_0_1:
  {
    if (m_renderMethod & RENDER_GLSL)
    {
      if (!m_fbo.fbo.Initialize())
      {
        CLog::Log(LOGERROR, "GLES: Error initializing FBO");
        break;
      }

      // m_intermediateFormat and m_intermediateType are set in the constructor from
      // hqscalerprecision setting. Try requested format first, fall back to GL_RGBA
      // if unsupported (e.g. GLES 2.0 context where GL_RGB10_A2 / GL_RGBA16F are
      // not available).
      if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight,
                                            m_intermediateFormat, m_intermediateType, GL_NEAREST))
      {
        if (m_intermediateFormat != GL_RGBA)
        {
          m_intermediateFormat = GL_RGBA;
          m_intermediateType = GL_UNSIGNED_BYTE;
          if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight,
                                                GL_RGBA))
          {
            CLog::Log(LOGERROR, "GLES: Error creating texture and binding to FBO");
            break;
          }
        }
        else
        {
          CLog::Log(LOGERROR, "GLES: Error creating texture and binding to FBO");
          break;
        }
      }
    }

    // TODO: GL also passes m_nonLinStretch, m_intermediateGammaCorrection.
    // Add when non-linear stretch and gamma correction are ported to GLES.
    if (m_scalingMethod == VS_SCALINGMETHOD_FSR1 || m_scalingMethod == VS_SCALINGMETHOD_FSR1_RCAS)
      m_pVideoFilterShader = new FSR1FilterShader();
    else
      m_pVideoFilterShader = new ConvolutionFilterShader(m_scalingMethod, m_useDithering);
    if (!m_pVideoFilterShader->CompileAndLink())
    {
      CLog::Log(LOGERROR, "GLES: Error compiling and linking video filter shader");
      break;
    }

    if (m_scalingMethod != VS_SCALINGMETHOD_FSR1 &&
        m_scalingMethod != VS_SCALINGMETHOD_FSR1_RCAS && m_useDithering && m_ditherTex)
    {
      bool ditherActive = true;
      unsigned int effectiveDitherDepth = m_ditherDepth;
      if (m_ditherDepth == 0)
      {
        GLint redBits = 0;
        glGetIntegerv(GL_RED_BITS, &redBits);
        // Use framebuffer depth on HDR displays; assume 8-bit on SDR displays
        // since the CRTC or display will truncate to 8-bit regardless of BO depth.
        if (CServiceBroker::GetWinSystem()->IsHDRDisplay())
          effectiveDitherDepth = (redBits > 0) ? redBits : 8;
        else
          effectiveDitherDepth = 8;

        float scaleFactor = ScalingAboveThreshold();
        bool downscaling = scaleFactor > 0.0f && scaleFactor < 1.0f;
        ditherActive = (m_srcFullRange != m_fullRange) || m_srcColorBits > effectiveDitherDepth ||
                       m_toneMap || downscaling;
      }
      static_cast<ConvolutionFilterShader*>(m_pVideoFilterShader)
          ->SetDitherUniforms(ditherActive, m_ditherTex, effectiveDitherDepth, dither_size);
    }

    CLog::Log(LOGINFO, "GLES: FBO intermediate format {:#x}",
              static_cast<unsigned>(m_intermediateFormat));
    CLog::Log(LOGINFO, "GLES: Selecting multi pass rendering");
    SetTextureFilter(GL_LINEAR);
    m_renderQuality = RQ_MULTIPASS;
      return;
  }
  case VS_SCALINGMETHOD_FSRCNNX:
  case VS_SCALINGMETHOD_ANIME4K:
  case VS_SCALINGMETHOD_BICUBIC_SOFTWARE:
  case VS_SCALINGMETHOD_LANCZOS_SOFTWARE:
  case VS_SCALINGMETHOD_SINC_SOFTWARE:
  case VS_SCALINGMETHOD_SINC8:
  {
    CLog::Log(LOGERROR, "GLES: TODO: This scaler has not yet been implemented");
    break;
  }
  default:
    break;
  }

  CLog::Log(LOGERROR, "GLES: Falling back to bilinear due to failure to init scaler");
  m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }

  m_fbo.fbo.Cleanup();

  m_pVideoFilterShader = new DefaultFilterShader();
  if (!m_pVideoFilterShader->CompileAndLink())
  {
    CLog::Log(LOGERROR, "CLinuxRendererGLES::UpdateVideoFilter: Error compiling and linking "
                        "default video filter shader");
  }

  SetTextureFilter(GL_LINEAR);
  m_renderQuality = RQ_SINGLEPASS;
}

void CLinuxRendererGLES::LoadShaders(int field)
{
  m_reloadShaders = false;

  if (!LoadShadersHook())
  {
    int requestedMethod = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_RENDERMETHOD);
    CLog::Log(LOGDEBUG, "GLES: Requested render method: {}", requestedMethod);

    ReleaseShaders();

    switch(requestedMethod)
    {
      case RENDER_METHOD_AUTO:
      case RENDER_METHOD_GLSL:
      {
        // Try GLSL shaders if supported and user requested auto or GLSL.
        if (glCreateProgram())
        {
          // Auto dithering (depth=0): decide per-video whether dithering is beneficial.
          // Shaders are always compiled with XBMC_DITHER when the setting is on.
          // The m_ditherEnabled uniform controls whether the shader applies dithering.
          bool ditherActive = true;
          unsigned int effectiveDitherDepth = m_ditherDepth;

          if (m_ditherDepth == 0 && m_ditherTex)
          {
            GLint redBits = 0;
            glGetIntegerv(GL_RED_BITS, &redBits);
            if (CServiceBroker::GetWinSystem()->IsHDRDisplay())
              effectiveDitherDepth = (redBits > 0) ? redBits : 8;
            else
              effectiveDitherDepth = 8;

            float scaleFactor = ScalingAboveThreshold();
            bool downscaling = scaleFactor > 0.0f && scaleFactor < 1.0f;
            ditherActive = (m_srcFullRange != m_fullRange) ||
                           m_srcColorBits > effectiveDitherDepth || m_toneMap || downscaling;

            CLog::Log(LOGDEBUG,
                      "GLES: auto dither {} (depth={}, source={}bit, "
                      "rangeMismatch={}, toneMap={}, downscaling={})",
                      ditherActive ? "on" : "off", effectiveDitherDepth, m_srcColorBits,
                      m_srcFullRange != m_fullRange, m_toneMap, downscaling);
          }

          EShaderFormat shaderFormat = GetShaderFormat();
          const auto dstPrimaries =
              m_passthroughHDR || m_shaderSDRBT2020 ? m_srcPrimaries : AVCOL_PRI_BT709;
          m_toneMapMethod = m_videoSettings.m_ToneMapMethod;

          // Try single-pass filter shader for FAST scalers on GLES 3.1+
          if (m_renderQuality == RQ_SINGLEPASS &&
              (m_scalingMethod == VS_SCALINGMETHOD_LANCZOS3_FAST ||
               m_scalingMethod == VS_SCALINGMETHOD_SPLINE36_FAST))
          {
            m_pYUVProgShader = new YUV2RGBFilterShader(
                shaderFormat, dstPrimaries,
                m_srcPrimaries, m_toneMap, m_toneMapMethod, m_scalingMethod, m_useDithering);
            // TODO: GL gates this on !m_cmsOn. Add when CMS is ported to GLES.
            m_pYUVProgShader->SetConvertFullColorRange(m_fullRange);

            CLog::Log(LOGINFO, "GLES: Selecting YUV 2 RGB shader with filter");

            if (m_pYUVProgShader->CompileAndLink())
            {
              if (m_useDithering && m_ditherTex)
                m_pYUVProgShader->SetDitherUniforms(ditherActive, m_ditherTex, effectiveDitherDepth,
                                                    dither_size);
              m_renderMethod = RENDER_GLSL;
              UpdateVideoFilter();
              break;
            }
            else
            {
              CLog::Log(LOGERROR, "GLES: Error enabling YUV2RGB GLSL shader");
              m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
              delete m_pYUVProgShader;
              m_pYUVProgShader = nullptr;
            }
          }

          // Fall back to regular progressive shader
          m_pYUVProgShader = new YUV2RGBProgressiveShader(
              shaderFormat, dstPrimaries,
              m_srcPrimaries, m_toneMap, m_toneMapMethod, m_useDithering);
          m_pYUVProgShader->SetConvertFullColorRange(m_fullRange);

          CLog::Log(LOGINFO, "GLES: Selecting YUV 2 RGB shader");

          m_pYUVBobShader = new YUV2RGBBobShader(
              shaderFormat, dstPrimaries,
              m_srcPrimaries, m_toneMap, m_toneMapMethod, m_useDithering);
          m_pYUVBobShader->SetConvertFullColorRange(m_fullRange);

          if ((m_pYUVProgShader && m_pYUVProgShader->CompileAndLink())
              && (m_pYUVBobShader && m_pYUVBobShader->CompileAndLink()))
          {
            if (m_useDithering && m_ditherTex)
            {
              m_pYUVProgShader->SetDitherUniforms(ditherActive, m_ditherTex, effectiveDitherDepth,
                                                  dither_size);
              m_pYUVBobShader->SetDitherUniforms(ditherActive, m_ditherTex, effectiveDitherDepth,
                                                 dither_size);
            }
            m_renderMethod = RENDER_GLSL;
            UpdateVideoFilter();
            break;
          }
          else
          {
            ReleaseShaders();
            CLog::Log(LOGERROR, "GLES: Error enabling YUV2RGB GLSL shader");
            m_renderMethod = -1;
            break;
          }
        }

        break;
      }
      default:
      {
        m_renderMethod = -1 ;
        CLog::Log(LOGERROR, "GLES: render method not supported");
      }
    }
  }
}

void CLinuxRendererGLES::ReleaseShaders()
{
  if (m_pYUVProgShader)
  {
    delete m_pYUVProgShader;
    m_pYUVProgShader = nullptr;
  }

  if (m_pYUVBobShader)
  {
    delete m_pYUVBobShader;
    m_pYUVBobShader = nullptr;
  }
}

void CLinuxRendererGLES::UnInit()
{
  CLog::Log(LOGDEBUG, "LinuxRendererGLES: Cleaning up GLES resources");
  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  glFinish();

  bool outputStopped = false;
#ifdef HAVE_DVBRIDGE
  m_shaderUpscaler.reset();
  m_rcasShader.reset();
  m_rcasInput.Cleanup();
  if (m_dvRequested)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    if (window)
    {
      outputStopped = true;
      if (!window->StopDVBridgePlayback())
        CLog::Log(LOGDEBUG, "DVBridge: output cleanup deferred until scanout retires");
    }
    m_dvRequested = false;
  }
#endif

  // YUV textures
  for (int i = 0; i < MAX_RENDER_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }

  if (m_bConfigured)
  {
    m_hdrFboActive = false;
#ifdef CB1_HAS_HDR10_AI
    outputStopped |= m_preserveHLGOutput;
#endif
    if (!outputStopped)
    {
      CServiceBroker::GetWinSystem()->SetGuiCompositing(false);
      CServiceBroker::GetWinSystem()->SetHDR(nullptr);
      CServiceBroker::GetWinSystem()->SetColorimetry(nullptr);
      CServiceBroker::GetWinSystem()->SetVideoOutput(nullptr);
    }
    m_passthroughHDR = false;
  }

  // cleanup framebuffer object if it was in use
  m_fbo.fbo.Cleanup();
  m_bValidated = false;
  m_bConfigured = false;
}

bool CLinuxRendererGLES::CreateTexture(int index)
{
  if (m_format == AV_PIX_FMT_NV12)
    return CreateNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 || m_format == AV_PIX_FMT_UYVY422)
    return CreatePackedYUVTexture(index);
  else
    return CreatePlanarYUVTexture(index);
}

void CLinuxRendererGLES::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  if (m_format == AV_PIX_FMT_NV12)
    DeleteNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 || m_format == AV_PIX_FMT_UYVY422)
    DeletePackedYUVTexture(index);
  else
    DeletePlanarYUVTexture(index);
}

bool CLinuxRendererGLES::UploadTexture(int index)
{
  if (!m_buffers[index].videoBuffer)
  {
    return false;
  }

  bool ret{true};

  if (!m_buffers[index].loaded)
  {
    YuvImage& dst = m_buffers[index].image;
    m_buffers[index].videoBuffer->GetPlanes(dst.plane);
    m_buffers[index].videoBuffer->GetStrides(dst.stride);

    if (m_format == AV_PIX_FMT_NV12)
    {
      ret = UploadNV12Texture(index);
    }
    else if (m_format == AV_PIX_FMT_YUYV422 || m_format == AV_PIX_FMT_UYVY422)
    {
      ret = UploadPackedYUVTexture(index);
    }
    else
    {
      // default to planar YUV texture handlers
      ret = UploadPlanarYUVTexture(index);
    }

    if (ret)
    {
      m_buffers[index].loaded = true;
    }
  }

  // recalculate on every render, not just on upload: ManageRenderArea() moves
  // m_sourceRect between the two eyes of a stereoscopic source
  if (ret)
  {
    CalculateTextureSourceRects(index, 3);
  }
#ifdef HAVE_DVBRIDGE
  if (auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem()))
  {
    CDVBridgeGLES::Layer layer{};
    const bool usable=ret && BuildDVBridgeLayer(index,layer);
    const auto& buffer=m_buffers[index];
    const auto binding=buffer.doviSourcePicture && buffer.doviSourcePicture->doviSourceBinding.Valid() ?
        buffer.doviSourcePicture->doviSourceBinding : buffer.doviMetadata.Binding();
    window->ObserveDVImport(binding,DVBridgeGeometry(),usable);
  }
#endif
  return ret;
}

#ifdef HAVE_DVBRIDGE
bool CLinuxRendererGLES::BuildDVBridgeLayer(int index, CDVBridgeGLES::Layer& layer)
{
  auto& buffer=m_buffers[index];
  if (!buffer.doviSourcePicture || m_textureTarget!=GL_TEXTURE_2D || m_renderOrientation!=0)
    return false;
  const auto& source=*buffer.doviSourcePicture;
  const bool hdr10 = !source.doviSourceInfo.profile && source.doviSourceBinding.Valid() &&
      DVBRIDGE::IsHDR10Family(source.hdrType);
  if (!hdr10 && (!buffer.doviMetadata.Data() || buffer.doviPts==DVD_NOPTS_VALUE ||
      !std::isfinite(buffer.doviPts))) return false;
  if (m_sourceWidth<=0 || m_sourceWidth>3840 || m_sourceHeight<=0 || m_sourceHeight>2160)
    return false;
  switch (m_format)
  {
#ifdef CB1_HAS_HDR10_AI
    case AV_PIX_FMT_YUV420P:
      if (!hdr10 || !m_planarHLG8 || !source.IsLimitedHLG8()) return false;
      layer.layout=DVBRIDGE_YUV420P8;
      break;
#endif
    case AV_PIX_FMT_YUV420P10LE: layer.layout=DVBRIDGE_YUV420P10; break;
    case AV_PIX_FMT_YUV444P10LE: layer.layout=DVBRIDGE_YUV444P10; break;
    default: return false;
  }
  layer.width=m_sourceWidth; layer.height=m_sourceHeight;
  layer.scalingMethod=SelectScalingMethod();
  layer.autoScalingMethod=AutoUpscalingOverride();
  layer.pts=hdr10 ? source.pts / DVD_TIME_BASE : buffer.doviPts;
  layer.binding=hdr10 ? source.doviSourceBinding : buffer.doviMetadata.Binding();
  if (!std::isfinite(layer.pts) || (hdr10 && (source.pts==DVD_NOPTS_VALUE ||
      source.doviSourcePts==AV_NOPTS_VALUE))) return false;
  if (hdr10)
  {
    layer.fullRange=source.color_range!=0;
    if (source.hasDisplayMetadata && source.displayMetadata.has_luminance)
    {
      layer.hdr.max_luma=av_q2d(source.displayMetadata.max_luminance);
      layer.hdr.min_luma=av_q2d(source.displayMetadata.min_luminance);
    }
    if (source.hasLightMetadata)
    { layer.hdr.max_cll=source.lightMetadata.MaxCLL; layer.hdr.max_fall=source.lightMetadata.MaxFALL; }
  }
  layer.lease=buffer.doviSourcePicture;
  layer.sourceInfo=buffer.doviSourcePicture->doviSourceInfo;
  layer.sourceSideData=buffer.doviSourcePicture->sourceSideData;
  layer.sourceSerial=layer.sourceSideData ? layer.sourceSideData->Serial() : 0;
  layer.hdr10PlusSource = source.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS;
  layer.hlg = source.hdrType == StreamHdrType::HDR_TYPE_HLG;
  layer.chromaLocation=buffer.doviSourcePicture->chroma_position;
  for (int p=0;p<3;++p)
  {
    const auto& plane=buffer.fields[FIELD_FULL][p];
    const bool subsampled = p && m_format != AV_PIX_FMT_YUV444P10LE;
    const unsigned width=subsampled ? (m_sourceWidth+1)/2 : m_sourceWidth;
    const unsigned height=subsampled ? (m_sourceHeight+1)/2 : m_sourceHeight;
    if (!plane.id || plane.texwidth!=width || plane.texheight!=height) return false;
    layer.planes[p]={plane.id,static_cast<int>(plane.texwidth),static_cast<int>(plane.texheight)};
  }
  return layer.binding.Valid();
}

#ifdef CB1_HAS_HDR10_AI
bool CLinuxRendererGLES::PrepareHDR10AIQueue(const std::deque<int>& queue, bool end, bool paused,
    uint64_t presentationDeadline)
{
  auto preparation = CaptureHDR10AIQueue(queue,end,paused,presentationDeadline);
  if (!preparation.execute) return preparation.ready;
  const auto outcome = preparation.execute();
  return preparation.publish(outcome) && outcome.ready;
}

HDR10AIQueuePreparation CLinuxRendererGLES::CaptureHDR10AIQueue(
    const std::deque<int>& queue, bool end, bool paused, uint64_t presentationDeadline)
{
  if (queue.empty() || !HDR10AIQueueEligible()) return {};
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
      CServiceBroker::GetWinSystem());
  if (!window) { m_aiLookahead=false; return {}; }
  std::vector<CDVBridgeGLES::Layer> layers;
  // Copy ownership, not a reference to a slot that the decoder may replace.
  const auto head = m_buffers[queue.front()].doviSourcePicture;
  if (!head) return {};
  const auto horizon = head->pts / DVD_TIME_BASE + 1.0;
  const bool metadataOnly = window->HDR10AIMetadataOnly();
  for (const auto idx : queue)
  {
    if (!HDR10AITextureReady(idx) && !UploadTexture(idx)) break;
    CDVBridgeGLES::Layer layer{};
    if (!BuildDVBridgeLayer(idx,layer)) break;
    layers.push_back(std::move(layer));
    if (metadataOnly) break;
    if (layers.back().pts >= horizon - 1e-9) break;
  }
  // Future imports must not replace the current picture's admission identity.
  if (!UploadTexture(queue.front())) return {false, {}, {}};
  const auto index = queue.front();
  const auto binding = head->doviSourceBinding;
  const bool buffered = !layers.empty() &&
      std::all_of(layers.begin(), layers.end(), [&](const auto& layer) {
        return layer.lease && layer.binding.Valid() && std::isfinite(layer.pts) &&
            layer.binding.stream == binding.stream && layer.binding.revision == binding.revision;
      }) && ((end && layers.size() == queue.size()) ||
             layers.back().pts >= layers.front().pts + 1.0 - 1e-9);
  const auto preferences = DVBRIDGE::FrameOutputSettings(head->hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS, head->hdrType == StreamHdrType::HDR_TYPE_HLG);
  if (preferences.hdr10Source == DVBRIDGE::HDR10SourceMode::Native ||
      preferences.hdr10Source == DVBRIDGE::HDR10SourceMode::SDR)
  {
    // Preserve the original locked/native stop and readiness path.
    const bool ready = window->PrepareHDR10AIQueue(*head,layers,end,paused,presentationDeadline);
    m_aiLookahead = window->HDR10AILookaheadActive();
    m_aiOutputPending |= m_aiLookahead != m_dvRequested;
    if (ready && m_aiOutputPending)
    {
      if (!ConfigureVideoOutput(*head)) return {false, {}, {}};
      m_aiOutputPending = false;
    }
    return {ready, {}, {}};
  }
  return {false,
      [window,head,layers=std::move(layers),end,paused,presentationDeadline]() {
        const bool ready = window->PrepareHDR10AIQueue(*head,layers,end,paused,presentationDeadline);
        return HDR10AIQueuePreparation::Outcome{ready,window->HDR10AILookaheadActive()};
      },
      [this,index,binding,preferences](const HDR10AIQueuePreparation::Outcome& outcome) {
        const auto& current = m_buffers[index].doviSourcePicture;
        const auto settings = DVBRIDGE::FrameOutputSettings(current && current->hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS, current && current->hdrType == StreamHdrType::HDR_TYPE_HLG);
        if (!current || !(current->doviSourceBinding == binding) ||
            settings.revision != preferences.revision || !settings.SameValues(preferences))
          return false;
        m_aiLookahead = outcome.lookahead;
        // Automatic fallback can change the resolved route without a mode switch.
        m_aiOutputPending |= m_aiLookahead != m_dvRequested;
        if (outcome.ready && m_aiOutputPending)
        {
          if (!ConfigureVideoOutput(*current)) return false;
          m_aiOutputPending = false;
        }
        return true;
      }, buffered && !metadataOnly};
}

bool CLinuxRendererGLES::HDR10AIFrameCommitted() const
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  return window && window->HDR10AIFrameCommitted();
}
#endif

bool CLinuxRendererGLES::RenderDVBridge(int index)
{
  auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  CDVBridgeGLES::Layer layer{};
  if (!window || !BuildDVBridgeLayer(index,layer)) return false;
  const auto& buffer=m_buffers[index];
  return window->RenderDVBridge(layer,buffer.doviEnhancement,buffer.doviEnhancementBinding,
      buffer.doviMetadata,DVBridgeGeometry());
}

dvbridge_geometry CLinuxRendererGLES::DVBridgeGeometry()
{
  auto* window = CServiceBroker::GetWinSystem();
  CRect source, dest, view;
  GetVideoRect(source, dest, view);
  const int width = window->GetWidth(), height = window->GetHeight();
  const auto& context = window->GetGfxContext();
  if (width <= 0 || height <= 0 || context.GetWidth() <= 0 || context.GetHeight() <= 0)
    return {};
  const CRect original = dest;
  dest.Intersect(CRect(0, 0, context.GetWidth(), context.GetHeight()));
  if (dest.Width() <= 0 || dest.Height() <= 0 || original.Width() <= 0 || original.Height() <= 0)
    return {};
  const float sourceX = source.Width() / original.Width();
  const float sourceY = source.Height() / original.Height();
  source.x1 += (dest.x1 - original.x1) * sourceX;
  source.x2 += (dest.x2 - original.x2) * sourceX;
  source.y1 += (dest.y1 - original.y1) * sourceY;
  source.y2 += (dest.y2 - original.y2) * sourceY;
  const float sx = float(width) / context.GetWidth();
  const float sy = float(height) / context.GetHeight();
  const float left = dest.x1 * sx, top = dest.y1 * sy;
  const float right = dest.x2 * sx, bottom = dest.y2 * sy;
  const int x = std::floor(left), y = std::floor(top);
  return {static_cast<int>(m_sourceWidth), static_cast<int>(m_sourceHeight),
          x, y, static_cast<int>(std::ceil(right)) - x,
          static_cast<int>(std::ceil(bottom)) - y, width, height,
          {source.x1, source.y1, source.x2, source.y2}, {left, top, right, bottom}, false};
}
#endif

#ifdef HAVE_DVBRIDGE
bool CLinuxRendererGLES::RenderShaderUpscaler(int index, int preset)
{
  const auto& buf = m_buffers[index];
  const bool hdr = m_passthroughHDR;
  const bool pq = buf.m_srcColTransfer == AVCOL_TRC_SMPTE2084;
  const bool hlg = buf.m_srcColTransfer == AVCOL_TRC_ARIB_STD_B67;
  const bool bt709 = (buf.m_srcColSpace == AVCOL_SPC_BT709 ||
                      buf.m_srcColSpace == AVCOL_SPC_UNSPECIFIED) &&
                     (buf.m_srcPrimaries == AVCOL_PRI_BT709 ||
                      buf.m_srcPrimaries == AVCOL_PRI_UNSPECIFIED) &&
                     (buf.m_srcColTransfer == AVCOL_TRC_BT709 ||
                      buf.m_srcColTransfer == AVCOL_TRC_UNSPECIFIED);
  const bool supportedColor = hdr ?
      ((pq || hlg) && buf.m_srcPrimaries == AVCOL_PRI_BT2020 &&
       buf.m_srcColSpace == AVCOL_SPC_BT2020_NCL && buf.m_srcBits == 10) :
      (bt709 && buf.m_srcBits == 8);
  if (m_currentField != FIELD_FULL || m_renderOrientation || m_toneMap ||
      m_textureTarget != GL_TEXTURE_2D || !supportedColor ||
      m_videoSettings.m_Brightness != 50.f || m_videoSettings.m_Contrast != 50.f ||
      glIsEnabled(GL_BLEND))
    return false;

  CRect src, dst, view;
  GetVideoRect(src, dst, view);
  // All bundled CNN presets need enlargement on both axes.
  if (dst.Width() <= 1.3f * src.Width() || dst.Height() <= 1.3f * src.Height() ||
      src.Width() <= 0 || src.Height() <= 0)
    return false;
  const auto format = GetShaderFormat();
  if (format != SHADER_NV12 && format != SHADER_NV12_RRG && format != SHADER_YV12)
    return false;

  if (!m_shaderUpscaler || m_shaderUpscaler->preset != preset)
  {
    m_shaderUpscaler = std::make_unique<ShaderUpscaler>();
    m_shaderUpscaler->ready = m_shaderUpscaler->Load(preset);
    if (!m_shaderUpscaler->ready)
      CLog::Log(LOGWARNING, "Unable to initialize SDR upscaler {}", preset);
  }
  if (!m_shaderUpscaler->ready)
    return false;

  const auto& scaler = *m_shaderUpscaler;
  const pl_gpu gpu = scaler.gl->gpu;
  struct pl_frame image{};
  image.num_planes = format == SHADER_YV12 ? 3 : 2;
  image.repr.sys = hdr ? PL_COLOR_SYSTEM_BT_2020_NC : PL_COLOR_SYSTEM_BT_709;
  image.repr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  image.color = hdr ? pl_color_space_hdr10 : pl_color_space_bt709;
  if (hlg)
    image.color.transfer = PL_COLOR_TRC_HLG;
  if (hdr)
  {
    image.repr.bits.sample_depth = 16;
    image.repr.bits.color_depth = buf.m_srcBits;
    image.repr.bits.bit_shift = image.num_planes == 2 ? 16 - buf.m_srcBits : 0;
  }
  image.crop = {src.x1, src.y1, src.x2, src.y2};
  for (int i = 0; i < image.num_planes; ++i)
  {
    const auto& plane = buf.fields[FIELD_FULL][i];
    pl_opengl_wrap_params params{};
    params.texture = plane.id;
    params.width = static_cast<int>(plane.texwidth);
    params.height = static_cast<int>(plane.texheight);
    const bool uv = i == 1 && image.num_planes == 2;
    params.iformat = hdr ? (uv ? GL_RG16_EXT : GL_R16_EXT) : (uv ? GL_RG8 : GL_R8);
    image.planes[i].texture = plane.id ? pl_opengl_wrap(gpu, &params) : nullptr;
    image.planes[i].components = i == 1 && image.num_planes == 2 ? 2 : 1;
    image.planes[i].component_mapping[0] = i;
    image.planes[i].component_mapping[1] = i + 1;
  }
  pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
  GLint framebuffer, viewport[4], redBits;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetIntegerv(GL_RED_BITS, &redBits);
  const int width = viewport[2], height = viewport[3];
  pl_opengl_wrap_params targetParams{};
  targetParams.framebuffer = framebuffer;
  targetParams.width = width;
  targetParams.height = height;
  targetParams.iformat = redBits == 8 && !hdr ? GL_RGBA8 :
      redBits == 10 ? GL_RGB10_A2 : redBits == 16 ? GL_RGBA16F : 0;
  const pl_tex target = !viewport[0] && !viewport[1] && targetParams.iformat
      ? pl_opengl_wrap(gpu, &targetParams) : nullptr;
  struct pl_frame output{};
  output.num_planes = 1;
  output.planes[0].texture = target;
  output.planes[0].flipped = true;
  output.planes[0].components = 4;
  for (int i = 0; i < 4; ++i)
    output.planes[0].component_mapping[i] = i;
  output.repr = pl_color_repr_rgb;
  output.repr.levels = m_fullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  output.color = image.color;
  output.crop = {dst.x1, dst.y1, dst.x2, dst.y2};
  const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
  GLint scissor[4];
  glGetIntegerv(GL_SCISSOR_BOX, scissor);
  const bool fullScissor = !scissorEnabled ||
      (scissor[0] == 0 && scissor[1] == 0 && scissor[2] == width && scissor[3] == height);

  bool rendered = false;
  const bool sourceReady = std::all_of(image.planes, image.planes + image.num_planes,
                                      [](const auto& plane) { return plane.texture != nullptr; });
  if (sourceReady && target && fullScissor)
  {
    glDisable(GL_SCISSOR_TEST);
    auto params = pl_render_default_params;
    params.hooks = scaler.hooks.data();
    params.num_hooks = static_cast<int>(scaler.hooks.size());
    params.upscaler = &pl_filter_bilinear;
    params.skip_caching_single_frame = true;
    rendered = pl_render_image(scaler.renderer, &image, &output, &params);
    const auto errors = pl_renderer_get_errors(scaler.renderer);
    rendered = rendered && !errors.errors && !errors.num_disabled_hooks;
    if (!rendered)
    {
      m_shaderUpscaler->ready = false;
      CLog::Log(LOGWARNING, "SDR upscaler {} failed, using Kodi scaling", preset);
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glViewport(viewport[0], viewport[1], width, height);
  glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
  if (scissorEnabled)
    glEnable(GL_SCISSOR_TEST);
  pl_tex_destroy(gpu, &output.planes[0].texture);
  for (int i = 0; i < image.num_planes; ++i)
    pl_tex_destroy(gpu, &image.planes[i].texture);
  return rendered;
}
#endif

bool CLinuxRendererGLES::Render(unsigned int flags, int index)
{
#ifdef HAVE_DVBRIDGE
  // A decoder flush can leave the previous picture selected until new video arrives.
  if (const auto& picture = m_buffers[index].doviSourcePicture;
      picture && picture->doviSourceInfo.informationGeneration &&
      picture->doviSourceInfo.informationGeneration !=
          CServiceBroker::GetDataCacheCore().GetVideoProcessingGeneration())
  {
    return false;
  }
#endif

  // obtain current field, if interlaced
  if( flags & RENDER_FLAG_TOP)
  {
    m_currentField = FIELD_TOP;
  }
  else if (flags & RENDER_FLAG_BOT)
  {
    m_currentField = FIELD_BOT;
  }
  else
  {
    m_currentField = FIELD_FULL;
  }

  // call texture load function
  if (!UploadTexture(index))
  {
    return false;
  }

#ifdef HAVE_DVBRIDGE
  if (m_buffers[index].doviSourcePicture)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    const auto& picture = *m_buffers[index].doviSourcePicture;
#ifdef CB1_HAS_HDR10_AI
    // A retained native frame cannot start the new output before the AI queue grows.
    if (SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers)
      return false;
#endif
    if (!window || !window->PrepareDVOutputFrame(picture))
      return false;
    m_dvRequested = window->DVBridgeRequested(&picture);
  }
  if (m_dvRequested)
  {
    m_reportedUpscaler = -2;
    const bool rendered = m_currentField == FIELD_FULL && RenderDVBridge(index);
    AfterRenderHook(index);
    if (rendered)
      DVBRIDGE::PlaybackNotification::RenderedNative(
          m_buffers[index].videoBuffer ? m_buffers[index].videoBuffer->PlaybackGeneration() : 0,
          m_buffers[index].doviMetadata.Binding());
    return rendered;
  }
#endif
#ifdef HAVE_DVBRIDGE
  int upscaler = -1;
  const int preset = KODI::VIDEO::UPSCALER::Get(
      m_sourceWidth, m_sourceHeight, KODI::VIDEO::UPSCALER::KODI_DEFAULT);
  if (KODI::VIDEO::UPSCALER::UsesShaderHooks(preset) && RenderShaderUpscaler(index, preset))
    upscaler = preset;
  else
#endif
  if (RenderHook(index))
  {
    ;
  }
  else if (m_renderMethod & RENDER_GLSL)
  {
    UpdateVideoFilter();
    switch(m_renderQuality)
    {
    case RQ_LOW:
    case RQ_SINGLEPASS:
    {
      RenderSinglePass(index, m_currentField);
      VerifyGLState();
      break;
    }
    case RQ_MULTIPASS:
    {
      RenderToFBO(index, m_currentField);
      RenderFromFBO();
      VerifyGLState();
      break;
    }
    default:
      break;
    }
#ifdef HAVE_DVBRIDGE
    if (KODI::VIDEO::UPSCALER::Enlarges(m_sourceRect.Width(), m_sourceRect.Height(),
                                       m_destRect.Width(), m_destRect.Height()))
      upscaler = m_currentField != FIELD_FULL && m_renderQuality != RQ_MULTIPASS &&
                 m_scalingMethod != VS_SCALINGMETHOD_NEAREST
                     ? VS_SCALINGMETHOD_LINEAR : m_scalingMethod;
#endif
  }
  else
  {
    return false;
  }

  AfterRenderHook(index);
#ifdef HAVE_DVBRIDGE
  const auto generation =
      m_buffers[index].videoBuffer ? m_buffers[index].videoBuffer->PlaybackGeneration() : 0;
  const auto informationGeneration = m_buffers[index].informationGeneration;
  if (informationGeneration != m_reportedUpscalerGeneration || m_reportedUpscaler != upscaler)
  {
    CServiceBroker::GetDataCacheCore().SetVideoUpscaler(upscaler, informationGeneration);
    m_reportedUpscalerGeneration = informationGeneration;
    m_reportedUpscaler = upscaler;
  }
  DVBRIDGE::PlaybackNotification::RenderedOrdinary(
      generation);
#ifdef CB1_HAS_HDR10_AI
  if (m_passthroughHDR)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
        CServiceBroker::GetWinSystem());
    if (window && window->HDR10HandoffPending())
    {
      const auto& buffer = m_buffers[index];
      VideoPicture signal{};
      signal.hdrType = buffer.hdrType;
      signal.color_transfer = buffer.m_srcColTransfer;
      signal.color_space = buffer.m_srcColSpace;
      signal.color_primaries = buffer.m_srcPrimaries;
      signal.color_range = buffer.m_srcFullRange;
      signal.colorBits = buffer.m_srcBits;
      signal.hasDisplayMetadata = buffer.hasDisplayMetadata;
      signal.displayMetadata = buffer.displayMetadata;
      signal.hasLightMetadata = buffer.hasHDR10LightMetadata;
      signal.lightMetadata = buffer.lightMetadata;
      window->StageHDR10Handoff(signal);
    }
  }
#endif
#endif
  return true;
}

void CLinuxRendererGLES::RenderSinglePass(int index, int field)
{
  CPictureBuffer &buf = m_buffers[index];
  CYuvPlane (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[field];

  CheckVideoParameters(index);

  if (m_reloadShaders)
  {
    m_reloadShaders = false;
    LoadShaders(field);
  }

  // Y
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(m_textureTarget, planes[0].id);

  // U
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(m_textureTarget, planes[1].id);

  // V
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(m_textureTarget, planes[2].id);

  glActiveTexture(GL_TEXTURE0);
  VerifyGLState();

  Shaders::GLES::BaseYUV2RGBGLSLShader* pYUVShader;
  if (field != FIELD_FULL)
  {
    pYUVShader = m_pYUVBobShader;
  }
  else
  {
    pYUVShader = m_pYUVProgShader;
  }

  pYUVShader->SetBlack(m_videoSettings.m_Brightness * 0.01f - 0.5f);
  pYUVShader->SetContrast(m_videoSettings.m_Contrast * 0.02f);
  pYUVShader->SetWidth(planes[0].texwidth);
  pYUVShader->SetHeight(planes[0].texheight);
  pYUVShader->SetColParams(buf.m_srcColSpace, buf.m_srcBits, !buf.m_srcFullRange, buf.m_srcTextureBits);
  pYUVShader->SetDisplayMetadata(buf.hasDisplayMetadata, buf.displayMetadata,
                                 buf.hasLightMetadata, buf.lightMetadata);
  pYUVShader->SetToneMapParam(m_videoSettings.m_ToneMapParam);

  if (field == FIELD_TOP)
  {
    pYUVShader->SetField(1);
  }
  else if(field == FIELD_BOT)
  {
    pYUVShader->SetField(0);
  }

  pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  pYUVShader->Enable();

  GLubyte idx[4] = {0, 1, 3, 2}; // determines order of triangle strip
  GLfloat m_vert[4][3];
  GLfloat m_tex[3][4][2];

  GLint vertLoc = pYUVShader->GetVertexLoc();
  GLint Yloc = pYUVShader->GetYcoordLoc();
  GLint Uloc = pYUVShader->GetUcoordLoc();
  GLint Vloc = pYUVShader->GetVcoordLoc();

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, 0, m_vert);
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, 0, m_tex[0]);
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, 0, m_tex[1]);
  glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, 0, m_tex[2]);

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  glEnableVertexAttribArray(Vloc);

  // Setup vertex position values
  for(int i = 0; i < 4; i++)
  {
    m_vert[i][0] = m_rotatedDestCoords[i].x;
    m_vert[i][1] = m_rotatedDestCoords[i].y;
    m_vert[i][2] = 0.0f;// set z to 0
  }

  // Setup texture coordinates
  for (int i = 0; i < 3; i++)
  {
    m_tex[i][0][0] = m_tex[i][3][0] = planes[i].rect.x1;
    m_tex[i][0][1] = m_tex[i][1][1] = planes[i].rect.y1;
    m_tex[i][1][0] = m_tex[i][2][0] = planes[i].rect.x2;
    m_tex[i][2][1] = m_tex[i][3][1] = planes[i].rect.y2;
  }

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  VerifyGLState();

  pYUVShader->Disable();
  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  glDisableVertexAttribArray(Vloc);

  VerifyGLState();
}

void CLinuxRendererGLES::RenderToFBO(int index, int field)
{
  CPictureBuffer &buf = m_buffers[index];
  CYuvPlane (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[field];

  CheckVideoParameters(index);

  if (m_reloadShaders)
  {
    m_reloadShaders = false;
    LoadShaders(m_currentField);
  }

  //! @todo Believed dead: every FBO invalidation clears m_bValidated, and
  //! ValidateRenderTarget resets the filter cache so UpdateVideoFilter
  //! recreates the FBO before any multipass render. Kept as a failsafe
  //! against unenumerated invalidation paths.
  if (!m_fbo.fbo.IsValid())
  {
    CLog::Log(LOGWARNING, "GLES: multipass FBO invalid at render time, recreating");

    if (!m_fbo.fbo.Initialize())
    {
      CLog::Log(LOGERROR, "GLES: Error initializing FBO");
      return;
    }

    // Recreate with the format/type UpdateVideoFilter settled on, so the
    // intermediate FBO keeps the bit depth configured by hqscalerprecision.
    if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight,
                                          m_intermediateFormat, m_intermediateType, GL_NEAREST))
    {
      CLog::Log(LOGERROR, "GLES: Error creating texture and binding to FBO");
      return;
    }
  }

  // Y
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(m_textureTarget, planes[0].id);
  VerifyGLState();

  // U
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(m_textureTarget, planes[1].id);
  VerifyGLState();

  // V
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(m_textureTarget, planes[2].id);
  VerifyGLState();

  glActiveTexture(GL_TEXTURE0);
  VerifyGLState();

  Shaders::GLES::BaseYUV2RGBGLSLShader* pYUVShader = m_pYUVProgShader;
  // make sure the yuv shader is loaded and ready to go
  if (!pYUVShader || (!pYUVShader->OK()))
  {
    CLog::Log(LOGERROR, "GLES: YUV shader not active, cannot do multipass render");
    return;
  }

  m_fbo.fbo.BeginRender();
  VerifyGLState();

  pYUVShader->SetBlack(m_videoSettings.m_Brightness * 0.01f - 0.5f);
  pYUVShader->SetContrast(m_videoSettings.m_Contrast * 0.02f);
  pYUVShader->SetWidth(planes[0].texwidth);
  pYUVShader->SetHeight(planes[0].texheight);
  pYUVShader->SetColParams(buf.m_srcColSpace, buf.m_srcBits, !buf.m_srcFullRange, buf.m_srcTextureBits);
  pYUVShader->SetDisplayMetadata(buf.hasDisplayMetadata, buf.displayMetadata,
                                 buf.hasLightMetadata, buf.lightMetadata);
  pYUVShader->SetToneMapParam(m_videoSettings.m_ToneMapParam);

  if (field == FIELD_TOP)
  {
    pYUVShader->SetField(1);
  }
  else if(field == FIELD_BOT)
  {
    pYUVShader->SetField(0);
  }

  VerifyGLState();

  glMatrixModview.Push();
  glMatrixModview->LoadIdentity();
  glMatrixModview.Load();

  glMatrixProject.Push();
  glMatrixProject->LoadIdentity();
  glMatrixProject->Ortho2D(0, m_sourceWidth, 0, m_sourceHeight);
  glMatrixProject.Load();

  pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());

  CRect viewport;
  m_renderSystem->GetViewPort(viewport);
  glViewport(0, 0, m_sourceWidth, m_sourceHeight);
  glScissor(0, 0, m_sourceWidth, m_sourceHeight);

  if (!pYUVShader->Enable())
  {
    CLog::Log(LOGERROR, "GLES: Error enabling YUV shader");
  }

  m_fbo.width  = planes[0].rect.x2 - planes[0].rect.x1;
  m_fbo.height = planes[0].rect.y2 - planes[0].rect.y1;

  if (m_textureTarget == GL_TEXTURE_2D)
  {
    m_fbo.width  *= planes[0].texwidth;
    m_fbo.height *= planes[0].texheight;
  }

  m_fbo.width  *= planes[0].pixpertex_x;
  m_fbo.height *= planes[0].pixpertex_y;

  // 1st Pass to video frame size
  GLubyte idx[4] = {0, 1, 3, 2}; // determines order of triangle strip
  GLfloat vert[4][3];
  GLfloat tex[3][4][2];

  GLint vertLoc = pYUVShader->GetVertexLoc();
  GLint Yloc = pYUVShader->GetYcoordLoc();
  GLint Uloc = pYUVShader->GetUcoordLoc();
  GLint Vloc = pYUVShader->GetVcoordLoc();

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, 0, vert);
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, 0, tex[0]);
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, 0, tex[1]);
  glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, 0, tex[2]);

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  glEnableVertexAttribArray(Vloc);

  // Setup vertex position values
  // Set vertex coordinates
  vert[0][0] = vert[3][0] = 0.0f;
  vert[0][1] = vert[1][1] = 0.0f;
  vert[1][0] = vert[2][0] = m_fbo.width;
  vert[2][1] = vert[3][1] = m_fbo.height;
  vert[0][2] = vert[1][2] = vert[2][2] = vert[3][2] = 0.0f;

  // Setup texture coordinates
  for (int i = 0; i < 3; i++)
  {
    tex[i][0][0] = tex[i][3][0] = planes[i].rect.x1;
    tex[i][0][1] = tex[i][1][1] = planes[i].rect.y1;
    tex[i][1][0] = tex[i][2][0] = planes[i].rect.x2;
    tex[i][2][1] = tex[i][3][1] = planes[i].rect.y2;
  }

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  VerifyGLState();

  pYUVShader->Disable();

  glMatrixModview.PopLoad();
  glMatrixProject.PopLoad();

  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  glDisableVertexAttribArray(Vloc);

  m_renderSystem->SetViewPort(viewport);

  m_fbo.fbo.EndRender();
#ifdef HAVE_DVBRIDGE
  if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(
          CServiceBroker::GetWinSystem()))
    window->BindNativePascalHDRFramebuffer();
#endif

  VerifyGLState();
}

#ifdef HAVE_DVBRIDGE
bool CLinuxRendererGLES::RunRCAS()
{
  const int width = static_cast<int>(std::lround(m_destRect.Width()));
  const int height = static_cast<int>(std::lround(m_destRect.Height()));
  if (!m_rcasInput.IsBound() || m_rcasWidth != width || m_rcasHeight != height)
  {
    GLint draw, read;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    m_rcasInput.Cleanup();
    const bool allocated = m_rcasInput.Initialize() &&
        m_rcasInput.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA16F,
                                           GL_HALF_FLOAT, GL_NEAREST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    if (!allocated)
      return false;
    m_rcasWidth = width;
    m_rcasHeight = height;
  }
  if (!m_rcasShader)
  {
    auto shader = std::make_unique<FSR1FilterShader>(true);
    if (!shader->CompileAndLink())
      return false;
    m_rcasShader = std::move(shader);
  }

  GLint draw, read, viewport[4];
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (!m_rcasInput.BeginRender())
    return false;
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, width, height);
  glActiveTexture(GL_TEXTURE0);
  m_fbo.fbo.SetFiltering(GL_TEXTURE_2D, GL_NEAREST);
  auto* easu = static_cast<FSR1FilterShader*>(m_pVideoFilterShader);
  easu->SetSourceTexture(0);
  easu->SetWidth(m_sourceWidth);
  easu->SetHeight(m_sourceHeight);
  easu->SetInputSize(m_fbo.width, m_fbo.height);
  easu->SetAlpha(1.f);
  constexpr GLfloat identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
  easu->SetMatrices(identity, identity);
  const bool ready = easu->Enable();
  if (ready)
  {
    const GLfloat vert[] = {-1,-1,1,-1,-1,1,1,1};
    const GLfloat u = m_fbo.width / m_sourceWidth;
    const GLfloat v = m_fbo.height / m_sourceHeight;
    const GLfloat tex[] = {0,0,u,0,0,v,u,v};
    const GLint position = easu->GetVertexLoc();
    const GLint coord = easu->GetcoordLoc();
    glVertexAttribPointer(position, 2, GL_FLOAT, 0, 0, vert);
    glVertexAttribPointer(coord, 2, GL_FLOAT, 0, 0, tex);
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(coord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(position);
    glDisableVertexAttribArray(coord);
    easu->Disable();
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  return ready;
}
#endif

void CLinuxRendererGLES::RenderFromFBO()
{
  auto* filterShader = m_pVideoFilterShader;
  bool sharpened = false;
#ifdef HAVE_DVBRIDGE
  sharpened = m_scalingMethod == VS_SCALINGMETHOD_FSR1_RCAS && RunRCAS();
  if (sharpened)
    filterShader = m_rcasShader.get();
  else if (m_scalingMethod == VS_SCALINGMETHOD_FSR1_RCAS)
    m_scalingMethod = VS_SCALINGMETHOD_FSR1;
#endif
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_fbo.fbo.Texture());
  VerifyGLState();

  // Use regular normalized texture coordinates

  // 2nd Pass to screen size with optional video filter

  if (filterShader)
  {
    GLint filter;
    if (!filterShader->GetTextureFilter(filter))
    {
      filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;
    }

#ifdef HAVE_DVBRIDGE
    if (sharpened)
      m_rcasInput.SetFiltering(GL_TEXTURE_2D, filter);
    else
#endif
      m_fbo.fbo.SetFiltering(GL_TEXTURE_2D, filter);
    filterShader->SetSourceTexture(0);
    int sampleWidth = m_sourceWidth, sampleHeight = m_sourceHeight;
#ifdef HAVE_DVBRIDGE
    if (sharpened)
    {
      sampleWidth = m_rcasWidth;
      sampleHeight = m_rcasHeight;
    }
#endif
    filterShader->SetWidth(sampleWidth);
    filterShader->SetHeight(sampleHeight);
    if (m_scalingMethod == VS_SCALINGMETHOD_FSR1)
      static_cast<FSR1FilterShader*>(filterShader)->SetInputSize(m_fbo.width, m_fbo.height);
    filterShader->SetAlpha(1.0f);
    filterShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
    filterShader->Enable();
  }
  else
  {
    GLint filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;
    m_fbo.fbo.SetFiltering(GL_TEXTURE_2D, filter);
  }

  VerifyGLState();

  float imgwidth = sharpened ? 1.f : m_fbo.width / m_sourceWidth;
  float imgheight = sharpened ? 1.f : m_fbo.height / m_sourceHeight;

  GLubyte idx[4] = {0, 1, 3, 2}; // determines order of triangle strip
  GLfloat vert[4][3];
  GLfloat tex[4][2];

  GLint vertLoc = filterShader->GetVertexLoc();
  GLint loc = filterShader->GetcoordLoc();

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, 0, vert);
  glVertexAttribPointer(loc, 2, GL_FLOAT, 0, 0, tex);

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(loc);

  // Setup vertex position values
  for(int i = 0; i < 4; i++)
  {
    vert[i][0] = m_rotatedDestCoords[i].x;
    vert[i][1] = m_rotatedDestCoords[i].y;
    vert[i][2] = 0.0f; // set z to 0
  }

  // Setup texture coordinates
  tex[0][0] = tex[3][0] = 0.0f;
  tex[0][1] = tex[1][1] = 0.0f;
  tex[1][0] = tex[2][0] = imgwidth;
  tex[2][1] = tex[3][1] = imgheight;

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(loc);
  glDisableVertexAttribArray(vertLoc);

  VerifyGLState();

  if (filterShader)
  {
    filterShader->Disable();
  }

  VerifyGLState();

  glBindTexture(GL_TEXTURE_2D, 0);
  VerifyGLState();
}

//********************************************************************************************************/
// YUV texture creation, deletion, copying + clearing
//********************************************************************************************************/
bool CLinuxRendererGLES::UploadPlanarYUVTexture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  bool useRedTextures = im->bpp == 2;
#ifdef CB1_HAS_HDR10_AI
  useRedTextures |= m_planarHLG8;
#endif
  if (useRedTextures)
  {
    LoadPlane(buf.fields[FIELD_FULL][0], GL_RED, im->width, im->height, im->stride[0], im->bpp,
              im->plane[0]);

    LoadPlane(buf.fields[FIELD_FULL][1], GL_RED, im->width >> im->cshift_x,
              im->height >> im->cshift_y, im->stride[1], im->bpp, im->plane[1]);

    LoadPlane(buf.fields[FIELD_FULL][2], GL_RED, im->width >> im->cshift_x,
              im->height >> im->cshift_y, im->stride[2], im->bpp, im->plane[2]);
  }
  else
  {
    // 8-bit: GL_LUMINANCE for Y/U, GL_ALPHA for V
    LoadPlane(buf.fields[FIELD_FULL][0], GL_LUMINANCE, im->width, im->height, im->stride[0],
              im->bpp, im->plane[0]);

    LoadPlane(buf.fields[FIELD_FULL][1], GL_LUMINANCE, im->width >> im->cshift_x,
              im->height >> im->cshift_y, im->stride[1], im->bpp, im->plane[1]);

    LoadPlane(buf.fields[FIELD_FULL][2], GL_ALPHA, im->width >> im->cshift_x,
              im->height >> im->cshift_y, im->stride[2], im->bpp, im->plane[2]);
  }

  VerifyGLState();

  return true;
}

void CLinuxRendererGLES::DeletePlanarYUVTexture(int index)
{
  YuvImage &im = m_buffers[index].image;

  if (m_buffers[index].fields[FIELD_FULL][0].id == 0)
  {
    return;
  }

  // finish up all textures, and delete them
  for(int f = 0; f < MAX_FIELDS; f++)
  {
    for(int p = 0; p < YuvImage::MAX_PLANES; p++)
    {
      if (m_buffers[index].fields[f][p].id)
      {
        if (glIsTexture(m_buffers[index].fields[f][p].id))
        {
          glDeleteTextures(1, &m_buffers[index].fields[f][p].id);
        }

        m_buffers[index].fields[f][p].id = 0;
      }
    }
  }

  for(int p = 0; p < YuvImage::MAX_PLANES; p++)
  {
    im.plane[p] = nullptr;
  }
}

bool CLinuxRendererGLES::CreatePlanarYUVTexture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  unsigned p;
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;

  DeletePlanarYUVTexture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;

  // Chroma subsampling derived from libavutil's pixdesc API. Works for any
  // planar YUV pix_fmt (4:2:0 / 4:2:2 / 4:4:4) without per-format checks.
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(m_format);
  im.cshift_x = desc ? desc->log2_chroma_w : 1;
  im.cshift_y = desc ? desc->log2_chroma_h : 1;

  buf.m_srcTextureBits = desc ? desc->comp[0].depth : 8;
  im.bpp = (buf.m_srcTextureBits > 8) ? 2 : 1;

  im.stride[0] = im.bpp * im.width;
  im.stride[1] = im.bpp * (im.width >> im.cshift_x);
  im.stride[2] = im.bpp * (im.width >> im.cshift_x);

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = im.stride[1] * (im.height >> im.cshift_y);
  im.planesize[2] = im.stride[2] * (im.height >> im.cshift_y);

  for (int i = 0; i < 3; i++)
  {
    im.plane[i] = nullptr; // will be set in UploadTexture()
  }

  for(int f = 0; f < MAX_FIELDS; f++)
  {
    for(p = 0; p < YuvImage::MAX_PLANES; p++)
    {
      if (!glIsTexture(m_buffers[index].fields[f][p].id))
      {
        glGenTextures(1, &m_buffers[index].fields[f][p].id);
        VerifyGLState();
      }
    }
  }

  // YUV
  for (int f = FIELD_FULL; f <= FIELD_BOT ; f++)
  {
    int fieldshift = (f == FIELD_FULL) ? 0 : 1;
    CYuvPlane (&planes)[YuvImage::MAX_PLANES] = m_buffers[index].fields[f];

    planes[0].texwidth  = im.width;
    planes[0].texheight = im.height >> fieldshift;

    planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[1].texheight = planes[0].texheight >> im.cshift_y;
    planes[2].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[2].texheight = planes[0].texheight >> im.cshift_y;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 1;
      planes[p].pixpertex_y = 1;
    }

    for(int p = 0; p < 3; p++)
    {
      CYuvPlane &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
      {
        continue;
      }

      glBindTexture(m_textureTarget, plane.id);

      if (im.bpp == 2)
      {
#if defined(GL_R16_EXT)
        glTexImage2D(m_textureTarget, 0, GL_R16_EXT, plane.texwidth, plane.texheight, 0, GL_RED,
                     GL_UNSIGNED_SHORT, nullptr);
#else
        CLog::Log(LOGERROR, "GLES: GL_R16_EXT undefined; cannot render >8-bit YUV");
        return false;
#endif
      }
#ifdef CB1_HAS_HDR10_AI
      else if (m_planarHLG8)
      {
        glTexImage2D(m_textureTarget, 0, GL_R8, plane.texwidth, plane.texheight, 0, GL_RED,
                     GL_UNSIGNED_BYTE, nullptr);
        // Kodi's legacy shaders read U from green and V from alpha. Keep
        // red available for the engine's single-channel plane imports.
        if (p != 0)
          glTexParameteri(m_textureTarget, p == 1 ? GL_TEXTURE_SWIZZLE_G : GL_TEXTURE_SWIZZLE_A,
                          GL_RED);
      }
#endif
      else
      {
        GLint format;
        if (p == 2) // V plane needs an alpha texture
          format = GL_ALPHA;
        else
          format = GL_LUMINANCE;

        glTexImage2D(m_textureTarget, 0, format, plane.texwidth, plane.texheight, 0, format,
                     GL_UNSIGNED_BYTE, nullptr);
      }

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }
  return true;
}

//********************************************************************************************************
// NV12 Texture loading, creation and deletion
//********************************************************************************************************
bool CLinuxRendererGLES::UploadNV12Texture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
  {
    deinterlacing = false;
  }
  else
  {
    deinterlacing = true;
  }

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT, im->bpp);

  if (deinterlacing)
  {
    // Load Odd Y field
    LoadPlane(buf.fields[FIELD_TOP][0] , GL_LUMINANCE,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0]);

    // Load Even Y field
    LoadPlane(buf.fields[FIELD_BOT][0], GL_LUMINANCE,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] + im->stride[0]) ;

    // Load Odd UV Fields
    LoadPlane(buf.fields[FIELD_TOP][1], GL_LUMINANCE_ALPHA,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1]);

    // Load Even UV Fields
    LoadPlane(buf.fields[FIELD_BOT][1], GL_LUMINANCE_ALPHA,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1] + im->stride[1]);

  }
  else
  {
    // Load Y plane
    LoadPlane(buf. fields[FIELD_FULL][0], GL_LUMINANCE,
              im->width, im->height,
              im->stride[0], im->bpp, im->plane[0]);

    // Load UV plane
    LoadPlane(buf.fields[FIELD_FULL][1], GL_LUMINANCE_ALPHA,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[1], im->bpp, im->plane[1]);
  }

  VerifyGLState();

  return true;
}

bool CLinuxRendererGLES::CreateNV12Texture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;

  // Delete any old texture
  DeleteNV12Texture(index);

  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;
  im.bpp = 1;

  im.stride[0] = im.width;
  im.stride[1] = im.width;
  im.stride[2] = 0;

  im.plane[0] = nullptr;
  im.plane[1] = nullptr;
  im.plane[2] = nullptr;

  // Y plane
  im.planesize[0] = im.stride[0] * im.height;
  // packed UV plane
  im.planesize[1] = im.stride[1] * im.height / 2;
  // third plane is not used
  im.planesize[2] = 0;

  for (int i = 0; i < 2; i++)
  {
    im.plane[i] = nullptr; // will be set in UploadTexture()
  }

  for(int f = 0; f < MAX_FIELDS; f++)
  {
    for(int p = 0; p < 2; p++)
    {
      if (!glIsTexture(buf.fields[f][p].id))
      {
        glGenTextures(1, &buf.fields[f][p].id);
        VerifyGLState();
      }
    }

    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f <= FIELD_BOT; f++)
  {
    int fieldshift = (f == FIELD_FULL) ? 0 : 1;
    CYuvPlane (&planes)[YuvImage::MAX_PLANES] = buf.fields[f];

    planes[0].texwidth  = im.width;
    planes[0].texheight = im.height >> fieldshift;

    planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
    planes[1].texheight = planes[0].texheight >> im.cshift_y;
    planes[2].texwidth  = planes[1].texwidth;
    planes[2].texheight = planes[1].texheight;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 1;
      planes[p].pixpertex_y = 1;
    }

    for(int p = 0; p < 2; p++)
    {
      CYuvPlane &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
      {
        continue;
      }

      glBindTexture(m_textureTarget, plane.id);

      if (p == 1)
      {
        glTexImage2D(m_textureTarget, 0, GL_LUMINANCE_ALPHA, plane.texwidth, plane.texheight, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, nullptr);
      }
      else
      {
        glTexImage2D(m_textureTarget, 0, GL_LUMINANCE, plane.texwidth, plane.texheight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
      }

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }

  return true;
}

void CLinuxRendererGLES::DeleteNV12Texture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;

  if (buf.fields[FIELD_FULL][0].id == 0)
  {
    return;
  }

  // finish up all textures, and delete them
  for(int f = 0; f < MAX_FIELDS; f++)
  {
    for(int p = 0; p < 2; p++)
    {
      if (buf.fields[f][p].id)
      {
        if (glIsTexture(buf.fields[f][p].id))
        {
          glDeleteTextures(1, &buf.fields[f][p].id);
        }

        buf.fields[f][p].id = 0;
      }
    }

    buf.fields[f][2].id = 0;
  }

  for(int p = 0; p < 2; p++)
  {
    im.plane[p] = nullptr;
  }
}

//********************************************************************************************************
// YUV422 packed Texture creation, deletion, copying + clearing
//********************************************************************************************************
bool CLinuxRendererGLES::UploadPackedYUVTexture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  // Load YUYV plane
  LoadPlane(buf.fields[FIELD_FULL][0], GL_BGRA_EXT, im->width / 2, im->height, im->stride[0],
            im->bpp, im->plane[0]);

  VerifyGLState();

  return true;
}

void CLinuxRendererGLES::DeletePackedYUVTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;

  if (buf.fields[FIELD_FULL][0].id == 0)
    return;

  // finish up all textures, and delete them
  for (int f = 0; f < MAX_FIELDS; f++)
  {
    if (buf.fields[f][0].id)
    {
      if (glIsTexture(buf.fields[f][0].id))
      {
        glDeleteTextures(1, &buf.fields[f][0].id);
      }
      buf.fields[f][0].id = 0;
    }
    buf.fields[f][1].id = 0;
    buf.fields[f][2].id = 0;
  }

  im.plane[0] = nullptr;
}

bool CLinuxRendererGLES::CreatePackedYUVTexture(int index)
{
  if (!CGLExtensions::IsExtensionSupported(CGLExtensions::EXT_texture_format_BGRA8888))
  {
    CLog::Log(LOGERROR, "CLinuxRendererGLES::CreatePackedYUVTexture - "
                        "GL_EXT_texture_format_BGRA8888 not supported");
    return false;
  }

  // since we also want the field textures, pitch must be texture aligned
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;

  // Delete any old texture
  DeletePackedYUVTexture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  im.cshift_x = 0;
  im.cshift_y = 0;
  im.bpp = 1;

  im.stride[0] = im.width * 2;
  im.stride[1] = 0;
  im.stride[2] = 0;

  im.planesize[0] = im.stride[0] * im.height;
  im.planesize[1] = 0;
  im.planesize[2] = 0;

  im.plane[0] = nullptr; // will be set in UploadTexture()
  im.plane[1] = nullptr;
  im.plane[2] = nullptr;

  for (int f = 0; f < MAX_FIELDS; f++)
  {
    if (!glIsTexture(buf.fields[f][0].id))
    {
      glGenTextures(1, &buf.fields[f][0].id);
      VerifyGLState();
    }
    buf.fields[f][1].id = buf.fields[f][0].id;
    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f <= FIELD_BOT; f++)
  {
    int fieldshift = (f == FIELD_FULL) ? 0 : 1;
    CYuvPlane(&planes)[YuvImage::MAX_PLANES] = buf.fields[f];

    planes[0].texwidth = im.width / 2;
    planes[0].texheight = im.height >> fieldshift;
    planes[1].texwidth = planes[0].texwidth;
    planes[1].texheight = planes[0].texheight;
    planes[2].texwidth = planes[1].texwidth;
    planes[2].texheight = planes[1].texheight;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 2;
      planes[p].pixpertex_y = 1;
    }

    CYuvPlane& plane = planes[0];
    if (plane.texwidth * plane.texheight == 0)
      continue;

    glBindTexture(m_textureTarget, plane.id);

    glTexImage2D(m_textureTarget, 0, GL_RGBA, plane.texwidth, plane.texheight, 0, GL_BGRA_EXT,
                 GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    VerifyGLState();
  }

  return true;
}

//********************************************************************************************************
// SurfaceTexture creation, deletion, copying + clearing
//********************************************************************************************************
void CLinuxRendererGLES::SetTextureFilter(GLenum method)
{
  for (int i = 0; i < m_NumYUVBuffers; i++)
  {
    CPictureBuffer& buf = m_buffers[i];

    for (int f = FIELD_FULL; f <= FIELD_BOT; f++)
    {
      for (int p = 0; p < 3; p++)
      {
        if(glIsTexture(buf.fields[f][p].id))
        {
          glBindTexture(m_textureTarget, buf.fields[f][p].id);
          glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, method);
          glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, method);
          VerifyGLState();
        }
      }
    }
  }
}

bool CLinuxRendererGLES::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_GAMMA ||
      feature == RENDERFEATURE_NOISE ||
      feature == RENDERFEATURE_SHARPNESS ||
      feature == RENDERFEATURE_NONLINSTRETCH)
  {
    return false;
  }

  if (feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_POSTPROCESS ||
      feature == RENDERFEATURE_ROTATION ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_TONEMAP)
  {
    return true;
  }

  return false;
}

bool CLinuxRendererGLES::SupportsMultiPassRendering()
{
  return true;
}

float CLinuxRendererGLES::ScalingAboveThreshold() const
{
  // Empty until ManageRenderArea has run, then the per-eye crop of a stereo source
  CRect source = m_sourceRect.IsEmpty() ? CRect(0, 0, m_sourceWidth, m_sourceHeight) : m_sourceRect;
  float scaleX = m_destRect.Width() / source.Width();
  float scaleY = m_destRect.Height() / source.Height();
  float scaleFactor = (scaleX + scaleY) / 2.0f;
  float scalePercent = fabs(1.0f - scaleFactor) * 100;
  int minScale = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_VIDEOPLAYER_HQSCALERS);

  if (scalePercent < minScale)
    return 0.0f;

  // < 1.0 = downscale, > 1.0 = upscale
  return scaleFactor;
}

bool CLinuxRendererGLES::Supports(ESCALINGMETHOD method) const
{
  // nearest neighbor doesn't work on YUY2 and UYVY
  if (method == VS_SCALINGMETHOD_NEAREST && m_format != AV_PIX_FMT_YUYV422 &&
      m_format != AV_PIX_FMT_UYVY422)
  {
    return true;
  }

  if (method == VS_SCALINGMETHOD_LINEAR || method == VS_SCALINGMETHOD_AUTO)
  {
    return true;
  }

#ifdef HAVE_DVBRIDGE
  if (KODI::VIDEO::UPSCALER::UsesFSR1(method))
    return (m_renderMethod & RENDER_GLSL) && !m_renderOrientation &&
           m_currentField == FIELD_FULL &&
           KODI::VIDEO::UPSCALER::Enlarges(m_sourceRect.Width(), m_sourceRect.Height(),
                                         m_destRect.Width(), m_destRect.Height());
#endif

  if (method == VS_SCALINGMETHOD_CUBIC_B_SPLINE ||
      method == VS_SCALINGMETHOD_CUBIC_MITCHELL ||
      method == VS_SCALINGMETHOD_CUBIC_CATMULL ||
      method == VS_SCALINGMETHOD_CUBIC_0_075 ||
      method == VS_SCALINGMETHOD_CUBIC_0_1 ||
      method == VS_SCALINGMETHOD_LANCZOS2 ||
      method == VS_SCALINGMETHOD_SPLINE36_FAST ||
      method == VS_SCALINGMETHOD_LANCZOS3_FAST ||
      method == VS_SCALINGMETHOD_SPLINE36 ||
      method == VS_SCALINGMETHOD_LANCZOS3)
  {
    // if scaling is below level, avoid hq scaling
    if (ScalingAboveThreshold() == 0.0f)
      return false;

    if (m_renderMethod & RENDER_GLSL)
    {
      return true;
    }
  }

  return false;
}

CRenderInfo CLinuxRendererGLES::GetRenderInfo()
{
  CRenderInfo info;
#ifdef CB1_HAS_HDR10_AI
  info.max_buffer_size = m_softwareAIBuffers;
#else
  info.max_buffer_size = NUM_BUFFERS;
#endif

  return info;
}

bool CLinuxRendererGLES::IsGuiLayer()
{
  return !m_hdrFboActive && !CServiceBroker::GetWinSystem()->UsesNativeSDRBT2020();
}

void CLinuxRendererGLES::CheckVideoParameters(int index)
{
  const CPictureBuffer& buf = m_buffers[index];
  const ETONEMAPMETHOD& toneMapMethod = m_videoSettings.m_ToneMapMethod;
  const bool wide = CServiceBroker::GetWinSystem()->UsesNativeSDRBT2020();
  if (wide != m_shaderSDRBT2020)
  {
    m_shaderSDRBT2020 = wide;
    m_reloadShaders = true;
  }

  if (buf.m_srcPrimaries != m_srcPrimaries)
  {
    m_srcPrimaries = buf.m_srcPrimaries;
    m_reloadShaders = true;
  }

  bool toneMap = false;
  const bool streamIsHDRPQ =
      (buf.m_srcColTransfer == AVCOL_TRC_SMPTE2084 && buf.m_srcPrimaries == AVCOL_PRI_BT2020);

  if (streamIsHDRPQ && !m_passthroughHDR && toneMapMethod != VS_TONEMAPMETHOD_OFF)
  {
    toneMap = true;
  }

  if (toneMap != m_toneMap || toneMapMethod != m_toneMapMethod)
  {
    m_reloadShaders = true;

    //! @todo HACK for #28468; clean up properly in Piers beta 2
    if (toneMap != m_toneMap)
    {
      VideoPicture tmp{};
      tmp.color_space = toneMap ? AVCOL_SPC_UNSPECIFIED : buf.m_srcColSpace;
      tmp.color_transfer = buf.m_srcColTransfer;
      tmp.color_primaries = buf.m_srcPrimaries;
      tmp.iWidth = m_sourceWidth;
      tmp.iHeight = m_sourceHeight;
      // Keep Adaptive on the format already selected for this source depth.
      tmp.colorBits = buf.m_srcBits;
      CServiceBroker::GetWinSystem()->SetColorimetry(&tmp, true);
    }

    m_toneMap = toneMap;
    m_toneMapMethod = toneMapMethod;
  }
}
