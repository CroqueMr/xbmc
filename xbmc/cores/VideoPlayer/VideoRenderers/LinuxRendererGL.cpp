/*
 *  Copyright (c) 2007 Frodo/jcmarshall/vulkanr/d4rk
 *      Based on XBoxRenderer by Frodo/jcmarshall
 *      Portions Copyright (c) by the authors of ffmpeg / xvid /mplayer
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "LinuxRendererGL.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#endif

#include "RenderFactory.h"
#include "ServiceBroker.h"
#include "VideoShaders/VideoFilterShaderGL.h"
#include "VideoShaders/YUV2RGBShaderGL.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/IPlayer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "rendering/GLExtensions.h"
#include "rendering/MatrixGL.h"
#include "rendering/gl/RenderSystemGL.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/GLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoSourceSideData.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10SourceType.h"
#include "cores/VideoPlayer/DVDCodecs/Video/SDRSourceType.h"
#ifdef CB1_HAS_HDR10_AI
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10AIBuffers.h"
#endif
#include "UpscalerSettings.h"
#include "ShaderUpscalerPresets.h"
#include "windowing/gbm/WinSystemGbmDVContext.h"

extern "C"
{
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
}
#endif

#ifdef TARGET_DARWIN_OSX
#include "platform/darwin/osx/CocoaInterface.h"
#endif

#include <algorithm>
#include <locale.h>
#include <memory>
#include <cmath>
#include <mutex>

extern "C"
{
#include <libavutil/dovi_meta.h>
}

#if defined(TARGET_DARWIN_OSX)
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/CGLIOSurface.h>
#endif

//! @bug
//! due to a bug on osx nvidia, using gltexsubimage2d with a pbo bound and a null pointer
//! screws up the alpha, an offset fixes this, there might still be a problem if stride + PBO_OFFSET
//! is a multiple of 128 and deinterlacing is on
#define PBO_OFFSET 16

using namespace Shaders;
using namespace Shaders::GL;

#ifdef HAVE_DVBRIDGE
struct CLinuxRendererGL::ShaderUpscaler
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

static const GLubyte stipple_weave[] = {
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,
};

CLinuxRendererGL::CPictureBuffer::CPictureBuffer()
{
  memset(&fields, 0, sizeof(fields));
  memset(&image , 0, sizeof(image));
  memset(&pbo   , 0, sizeof(pbo));
  videoBuffer = nullptr;
  loaded = false;
}

CBaseRenderer* CLinuxRendererGL::Create(CVideoBuffer *buffer)
{
  return new CLinuxRendererGL();
}

bool CLinuxRendererGL::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("default", CLinuxRendererGL::Create);
  return true;
}

CLinuxRendererGL::CLinuxRendererGL()
{
  m_iFlags = 0;
  m_format = AV_PIX_FMT_NONE;

  std::tie(m_useDithering, m_ditherDepth) = CServiceBroker::GetWinSystem()->GetDitherSettings();
  // TODO: GL does not support Auto dithering yet; treat as 8-bit (previous default)
  if (m_ditherDepth == 0)
    m_ditherDepth = 8;

  m_fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();

  m_fbo.width = 0.0;
  m_fbo.height = 0.0;

  m_ColorManager = std::make_unique<CColorManager>();
  m_tCLUTTex = 0;
  m_CLUT = NULL;
  m_CLUTsize = 0;
  m_cmsToken = -1;
  m_cmsOn = false;

  m_renderSystem = dynamic_cast<CRenderSystemGL*>(CServiceBroker::GetRenderSystem());

  int32_t intermediatePrecision = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_VIDEOPLAYER_HQSCALERPRECISION);
  if (intermediatePrecision == 16)
  {
    m_intermediateFormat = GL_RGBA16;
    m_intermediateType = GL_SHORT;
    m_intermediateGammaCorrection = true;
  }
  else if (intermediatePrecision == 10)
  {
    m_intermediateFormat = GL_RGB10_A2;
    m_intermediateType = GL_UNSIGNED_INT_2_10_10_10_REV;
    m_intermediateGammaCorrection = true;
  }
  CLog::Log(LOGDEBUG, "GL: HQ scaler precision={}, intermediate format={:#x}",
            intermediatePrecision, static_cast<unsigned>(m_intermediateFormat));
}

CLinuxRendererGL::~CLinuxRendererGL()
{
  UnInit();

  if (m_pYUVShader)
  {
    delete m_pYUVShader;
    m_pYUVShader = nullptr;
  }

  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }
}

bool CLinuxRendererGL::ValidateRenderer()
{
  if (!m_bConfigured)
    return false;

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
    return false;

  int index = m_iYUVRenderBuffer;
  const CPictureBuffer& buf = m_buffers[index];

  if (!buf.fields[FIELD_FULL][0].id)
    return false;

  return true;
}

bool CLinuxRendererGL::ValidateRenderTarget()
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
      return false;

    if (m_textureTarget == GL_TEXTURE_RECTANGLE)
      CLog::Log(LOGINFO, "Using GL_TEXTURE_RECTANGLE");
    else
      CLog::Log(LOGINFO, "Using GL_TEXTURE_2D");

    for (int i = 0; i < m_NumYUVBuffers; i++)
      CreateTexture(i);

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

bool SoftwarePlanarRed8(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != AV_PIX_FMT_YUV420P || !(picture.IsLimitedHLG8() || DVBRIDGE::IsSDRExpansionSource(picture)))
    return false;
  unsigned int major, minor;
  CServiceBroker::GetRenderSystem()->GetRenderVersion(major, minor);
  return major >= 3;
}

unsigned SoftwareHDR10AIBufferCount(const VideoPicture& picture, float fps, unsigned orientation)
{
  const auto format = picture.videoBuffer->GetFormat();
  const bool planar = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_YUV444P10LE ||
      SoftwarePlanarRed8(picture);
  const bool supported = planar && orientation == 0 && !picture.doviSourceInfo.profile &&
      picture.stereoMode.empty() && picture.iWidth <= 3840 && picture.iHeight <= 2160;
  const auto mode = SoftwareHDR10AIMode(picture.hdrType);
  const bool source = supported &&
      DVBRIDGE::IsHDR10AIInput(picture.hdrType, mode == DVBRIDGE::HDR10SourceMode::HDR10PlusDV);
  return DVBRIDGE::HDR10AIBuffers::Count(source, mode, fps, picture.iFlags & DVP_FLAG_INTERLACED);
}
}
#endif

bool CLinuxRendererGL::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  return Configure(picture, fps, orientation, RetainedOutput::None);
}

bool CLinuxRendererGL::Configure(const VideoPicture& picture, float fps,
                                 unsigned int orientation, RetainedOutput retainedOutput)
{
  m_format = picture.videoBuffer->GetFormat();
#ifdef CB1_HAS_HDR10_AI
  m_planarRed8 = SoftwarePlanarRed8(picture);
#endif
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;
  m_fps = fps;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsStereoMode(picture.stereoMode);

  m_srcPrimaries = picture.color_primaries;
  m_toneMap = false;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  m_bConfigured = true;
  m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  m_scalingMethod = m_videoSettings.m_ScalingMethod;
#ifdef HAVE_DVBRIDGE
  m_pendingHdrFbo.reset();
  m_reportedUpscaler = -2;
#endif

  // Ensure that textures are recreated and rendering starts only after the 1st
  // frame is loaded after every call to Configure().
  m_bValidated = false;

  m_nonLinStretch = false;
  m_nonLinStretchGui = false;
  m_pixelRatio = 1.0;

  m_pboSupported = CGLExtensions::IsExtensionSupported(CGLExtensions::ARB_pixel_buffer_object);

#ifdef CB1_HAS_HDR10_AI
  if (retainedOutput == RetainedOutput::HDR10)
  {
    auto& window = static_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext&>(
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
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
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

bool CLinuxRendererGL::ConfigureVideoOutput(const VideoPicture& picture)
{
#ifdef CB1_HAS_HDR10_AI
  if (m_aiOutputPending &&
      KODI::WINDOWING::GBM::CWinSystemGbmDVContext::DVBridgeRequested(&picture))
  {
    // Queue preparation already owns the final surface and its first DV frame.
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
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
    CLog::Log(LOGWARNING, "LinuxRendererGL::Configure: SetVideoOutput failed");
#ifdef HAVE_DVBRIDGE
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
        CServiceBroker::GetWinSystem());
    if (KODI::WINDOWING::GBM::CWinSystemGbmDVContext::DVBridgeRequested(&picture) ||
        (window && window->HasPendingDVBridgeRestore()))
    {
      m_bConfigured = false;
      return false;
    }
#endif
  }

#ifdef HAVE_DVBRIDGE
  m_dvRequested = KODI::WINDOWING::GBM::CWinSystemGbmDVContext::DVBridgeRequested(&picture);
  if (m_dvRequested)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
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

  CServiceBroker::GetWinSystem()->SetColorimetry(&picture, !m_ColorManager->IsEnabled());
  m_passthroughHDR = CServiceBroker::GetWinSystem()->SetHDR(&picture);
  CLog::Log(LOGDEBUG, "LinuxRendererGL::Configure: HDR passthrough: {}",
            m_passthroughHDR ? "on" : "off");

  m_hdrFboActive =
      m_passthroughHDR && CServiceBroker::GetWinSystem()->SetGuiCompositing(picture.color_transfer);
  if (m_passthroughHDR && !m_hdrFboActive)
    CLog::Log(LOGWARNING, "LinuxRendererGL::Configure: HDR passthrough active but GUI "
                          "compositing not supported by windowing system");

  // load 3DLUT
  if (m_ColorManager->IsEnabled())
  {
    if (!m_ColorManager->CheckConfiguration(m_cmsToken, m_srcPrimaries))
    {
      CLog::Log(LOGDEBUG, "CMS configuration changed, reload LUT");
      if (!LoadCLUT())
        return false;
    }
    m_cmsOn = true;
  }
  else
  {
    m_cmsOn = false;
  }

  // AI priming may have compiled the ordinary shader before HDR was selected.
  m_reloadShaders = true;
  m_bConfigured = true;
  return true;
}

#ifdef CB1_HAS_HDR10_AI
void CLinuxRendererGL::PreserveHDR10AIOutputOnReconfigure(const VideoPicture& picture)
{
  // Growing the software analysis queue must keep native HLG until the DV frame is ready.
  m_preserveHLGOutput = picture.hdrType == StreamHdrType::HDR_TYPE_HLG &&
      m_passthroughHDR && !m_dvRequested && picture.videoBuffer->GetFormat() == m_format &&
      SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers;
}
#endif

bool CLinuxRendererGL::ConfigChanged(const VideoPicture &picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

#ifdef CB1_HAS_HDR10_AI
  // Kodi can rebuild the queue when Quick Switch first enables analysis.
  return SoftwarePlanarRed8(picture) != m_planarRed8 ||
      SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers;
#else
  return false;
#endif
}

void CLinuxRendererGL::AddVideoPicture(const VideoPicture &picture, int index)
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
    if (DVBRIDGE::IsSDRExpansionSource(picture) && buf.recycledSDRPicture)
      buf.doviSourcePicture = std::move(buf.recycledSDRPicture);
    else
    {
      buf.recycledSDRPicture.reset();
      buf.doviSourcePicture = std::make_shared<VideoPicture>();
    }
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
    buf.hasLightMetadata = picture.hasLightMetadata;
}

void CLinuxRendererGL::ReleaseBuffer(int idx)
{
  CPictureBuffer &buf = m_buffers[idx];
  buf.doviMetadata.Reset();
#ifdef HAVE_DVBRIDGE
  buf.doviEnhancement.reset();
  buf.doviEnhancementBinding = {};
  // Reuse the allocation only after all renderer/GPU leases retired.
  if (buf.doviSourcePicture.use_count() == 1 &&
      DVBRIDGE::IsSDRExpansionSource(*buf.doviSourcePicture))
  {
    buf.doviSourcePicture->Reset();
    buf.recycledSDRPicture = std::move(buf.doviSourcePicture);
  }
  else
    buf.doviSourcePicture.reset();
#endif
  buf.doviPts = DVD_NOPTS_VALUE;
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

void CLinuxRendererGL::GetPlaneTextureSize(CYuvPlane& plane)
{
  /* texture is assumed to be bound */
  GLint width  = 0
      , height = 0
      , border = 0;
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_WIDTH , &width);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_HEIGHT, &height);
  glGetTexLevelParameteriv(m_textureTarget, 0, GL_TEXTURE_BORDER, &border);
  plane.texwidth  = width  - 2 * border;
  plane.texheight = height - 2 * border;
  if(plane.texwidth <= 0 || plane.texheight <= 0)
  {
    CLog::Log(LOGDEBUG, "CLinuxRendererGL::GetPlaneTextureSize - invalid size {}x{} - {}", width,
              height, border);
    /* to something that avoid division by zero */
    plane.texwidth  = 1;
    plane.texheight = 1;
  }
}

void CLinuxRendererGL::CalculateTextureSourceRects(int source, int num_planes)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im  = &buf.image;

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
        /* correct for field offsets and chroma offsets */
        float offset_y = 0.5f;
        if(plane != 0)
          offset_y += 0.5f;
        if(field == FIELD_BOT)
          offset_y *= -1;

        p.rect.y1 += offset_y;
        p.rect.y2 += offset_y;

        /* half the height if this is a field */
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

void CLinuxRendererGL::LoadPlane(CYuvPlane& plane, int type,
                                 unsigned width, unsigned height,
                                 int stride, int bpp, void* data)
{

  if (plane.pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, plane.pbo);

  int bps = bpp * KODI::UTILS::GL::glFormatElementByteCount(type);

  unsigned datatype;
  if (bpp == 2)
    datatype = GL_UNSIGNED_SHORT;
  else
    datatype = GL_UNSIGNED_BYTE;

  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / bps);
  glBindTexture(m_textureTarget, plane.id);
  glTexSubImage2D(m_textureTarget, 0, 0, 0, width, height, type, datatype, data);

  /* check if we need to load any border pixels */
  if (height < plane.texheight)
    glTexSubImage2D( m_textureTarget, 0
                   , 0, height, width, 1
                   , type, datatype
                   , (unsigned char*)data + stride * (height-1));

  if (width  < plane.texwidth)
    glTexSubImage2D( m_textureTarget, 0
                   , width, 0, 1, height
                   , type, datatype
                   , (unsigned char*)data + bps * (width-1));

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindTexture(m_textureTarget, 0);
  if (plane.pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

bool CLinuxRendererGL::Flush(bool saveBuffers)
{
  bool safe = saveBuffers && CanSaveBuffers();
  glFinish();

#ifdef HAVE_DVBRIDGE
  if (!safe && m_dvRequested)
    if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
            CServiceBroker::GetWinSystem()))
      window->DiscardDVBridgeFrame();
  if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
          CServiceBroker::GetWinSystem()))
    window->CancelDeferredOrdinaryVideo();
  m_pendingHdrFbo.reset();
#endif

  for (int i = 0; i < m_NumYUVBuffers; i++)
  {
    if (!safe)
      ReleaseBuffer(i);
    DeleteTexture(i);
  }

  delete m_pYUVShader;
  m_pYUVShader = nullptr;
  delete m_pVideoFilterShader;
  m_pVideoFilterShader = nullptr;

  glFinish();
  m_bValidated = false;
  m_fbo.fbo.Cleanup();
  m_iYUVRenderBuffer = 0;

  return safe;
}

void CLinuxRendererGL::Update()
{
  if (!m_bConfigured)
    return;

  ManageRenderArea();
  ValidateRenderTarget();
}

void CLinuxRendererGL::UpdateActiveArea(int index)
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

void CLinuxRendererGL::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
#ifdef HAVE_DVBRIDGE
  if (const auto& picture = m_buffers[index].doviSourcePicture)
  {
    if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
            CServiceBroker::GetWinSystem()))
    {
      window->MarkDeferredOrdinaryVideoAttempted(*picture);
      // The selected CB1 picture owns the transaction, not Kodi's catch-up slot.
      if ((m_hdrFboActive && m_dvRequested) || m_pendingHdrFbo ||
          window->HasDeferredOrdinaryVideo())
        index2 = -1;
    }
  }
#endif
  m_frameRendered = false;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrameRendered = false;
#endif
#ifdef HAVE_DVBRIDGE
  if (m_pendingHdrFbo)
  {
    if (m_hdrFboActive != *m_pendingHdrFbo)
      return;
    m_pendingHdrFbo.reset();
  }
#endif
  if (index2 >= 0)
    m_iYUVRenderBuffer = index2;
  else
    m_iYUVRenderBuffer = index;

  if (!ValidateRenderer())
  {
    if (clear) //if clear is set, we're expected to overwrite all backbuffer pixels, even if we have nothing to render
      ClearBackBuffer();

    return;
  }

  if (const bool fullRange = !CServiceBroker::GetWinSystem()->UseLimitedColor();
      fullRange != m_fullRange)
  {
    m_fullRange = fullRange;
    m_reloadShaders = true;
    // the CMS output shader bakes in the range
    m_scalingMethodGui = VS_SCALINGMETHOD_MAX;
  }

  UpdateActiveArea(index);
  ManageRenderArea();

  if (clear)
  {
    //draw black bars when video is not transparent, clear the entire backbuffer when it is
    if (alpha == 255)
      DrawBlackBars();
    else
      ClearBackBuffer();
  }

  if (alpha < 255)
  {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
  else
  {
    glDisable(GL_BLEND);
  }

  if (m_pYUVShader)
    m_pYUVShader->SetAlpha(alpha/255);
  if (m_pVideoFilterShader)
    m_pVideoFilterShader->SetAlpha(alpha/255);

  const bool rendered = Render(flags, m_iYUVRenderBuffer);
  m_frameRendered = rendered;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrameRendered = rendered;
#endif
  if (!rendered && clear)
    ClearBackBuffer();

  if (index2 >= 0)
  {
    m_iYUVRenderBuffer = index;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (m_pYUVShader)
      m_pYUVShader->SetAlpha(alpha/255/2);
    if (m_pVideoFilterShader)
      m_pVideoFilterShader->SetAlpha(alpha/255/2);

    Render(flags, m_iYUVRenderBuffer);
  }

  VerifyGLState();
  glEnable(GL_BLEND);
  glFlush();
}

void CLinuxRendererGL::ClearBackBuffer()
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

void CLinuxRendererGL::ClearBackBufferQuad()
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

  m_renderSystem->EnableShader(ShaderMethodGL::SM_DEFAULT);
  GLint posLoc = m_renderSystem->ShaderGetPos();
  GLint uniCol = m_renderSystem->ShaderGetUniCol();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);

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

  m_renderSystem->DisableShader();
}

//draw black bars around the video quad, this is more efficient than glClear()
//since it only sets pixels to black that aren't going to be overwritten by the video
void CLinuxRendererGL::DrawBlackBars()
{
  glDisable(GL_BLEND);

  struct Svertex
  {
    float x,y,z;
  };
  Svertex vertices[24];
  GLubyte count = 0;

  m_renderSystem->EnableShader(ShaderMethodGL::SM_DEFAULT);
  GLint posLoc = m_renderSystem->ShaderGetPos();
  GLint uniCol = m_renderSystem->ShaderGetUniCol();

  glUniform4f(uniCol, 0.0f, 0.0f, 0.0f, 1.0f);

  int osWindowWidth = CServiceBroker::GetWinSystem()->GetGfxContext().GetWidth();
  int osWindowHeight = CServiceBroker::GetWinSystem()->GetGfxContext().GetHeight();

  //top quad
  if (m_destRect.y1 > 0.0f)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = 0.0;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = 0;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = m_destRect.y1;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_destRect.y1;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // bottom quad
  if (m_destRect.y2 < osWindowHeight)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_destRect.y2;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = m_destRect.y2;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = osWindowHeight;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = osWindowHeight;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // left quad
  if (m_destRect.x1 > 0.0f)
  {
    GLubyte quad = count;
    vertices[quad].x = 0.0;
    vertices[quad].y = m_destRect.y1;
    vertices[quad].z = 0;
    vertices[quad+1].x = m_destRect.x1;
    vertices[quad+1].y = m_destRect.y1;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = m_destRect.x1;
    vertices[quad+2].y = m_destRect.y2;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = 0;
    vertices[quad+4].y = m_destRect.y2;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  // right quad
  if (m_destRect.x2 < osWindowWidth)
  {
    GLubyte quad = count;
    vertices[quad].x = m_destRect.x2;
    vertices[quad].y = m_destRect.y1;
    vertices[quad].z = 0;
    vertices[quad+1].x = osWindowWidth;
    vertices[quad+1].y = m_destRect.y1;
    vertices[quad+1].z = 0;
    vertices[quad+2].x = osWindowWidth;
    vertices[quad+2].y = m_destRect.y2;
    vertices[quad+2].z = 0;
    vertices[quad+3] = vertices[quad+2];
    vertices[quad+4].x = m_destRect.x2;
    vertices[quad+4].y = m_destRect.y2;
    vertices[quad+4].z = 0;
    vertices[quad+5] = vertices[quad];
    count += 6;
  }

  GLuint vertexVBO;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Svertex)*count, &vertices[0], GL_STATIC_DRAW);

  glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, sizeof(Svertex), 0);
  glEnableVertexAttribArray(posLoc);

  glDrawArrays(GL_TRIANGLES, 0, count);

  glDisableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);

  m_renderSystem->DisableShader();
}

ESCALINGMETHOD CLinuxRendererGL::SelectScalingMethod() const
{
  return ResolveScalingMethod(m_videoSettings.m_ScalingMethod);
}

ESCALINGMETHOD CLinuxRendererGL::ResolveScalingMethod(ESCALINGMETHOD method) const
{
  if (!Supports(method))
    method = VS_SCALINGMETHOD_LINEAR;

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
ESCALINGMETHOD CLinuxRendererGL::AutoUpscalingOverride() const
{
  using namespace KODI::VIDEO::UPSCALER;
  if (Get(m_sourceWidth, m_sourceHeight, KODI_DEFAULT) == VS_SCALINGMETHOD_AUTO)
    return ResolveScalingMethod(VS_SCALINGMETHOD_AUTO);
  return VS_SCALINGMETHOD_LINEAR;
}
#endif

void CLinuxRendererGL::UpdateVideoFilter()
{
  if (!m_pVideoFilterShader)
  {
    m_pVideoFilterShader = new DefaultFilterShader();
    if (!m_pVideoFilterShader->CompileAndLink())
    {
      CLog::Log(LOGERROR, "CLinuxRendererGL::UpdateVideoFilter: Error compiling and linking video filter shader");
      return;
    }
  }

  bool pixelRatioChanged = (CDisplaySettings::GetInstance().GetPixelRatio() > 1.001f ||
                            CDisplaySettings::GetInstance().GetPixelRatio() < 0.999f) !=
                            (m_pixelRatio > 1.001f || m_pixelRatio < 0.999f);
  bool nonLinStretchChanged = false;
  bool cmsChanged = (m_cmsOn != m_ColorManager->IsEnabled()) ||
                    (m_cmsOn && !m_ColorManager->CheckConfiguration(m_cmsToken, m_srcPrimaries));
  if (m_nonLinStretchGui != CDisplaySettings::GetInstance().IsNonLinearStretched() || pixelRatioChanged)
  {
    m_nonLinStretchGui = CDisplaySettings::GetInstance().IsNonLinearStretched();
    m_pixelRatio = CDisplaySettings::GetInstance().GetPixelRatio();
    m_reloadShaders = true;
    nonLinStretchChanged = true;

    if (m_nonLinStretchGui && (m_pixelRatio < 0.999f || m_pixelRatio > 1.001f) && Supports(RENDERFEATURE_NONLINSTRETCH))
    {
      m_nonLinStretch = true;
      CLog::Log(LOGDEBUG, "GL: Enabling non-linear stretch");
    }
    else
    {
      m_nonLinStretch = false;
      CLog::Log(LOGDEBUG, "GL: Disabling non-linear stretch");
    }
  }

  CRect srcRect, dstRect, viewRect;
  GetVideoRect(srcRect, dstRect, viewRect);

  // TODO: UpdateVideoFilter may be called with a 0x0 viewport before the
  // display resolution is established (the WHITELIST/resolution ADJUST happens
  // ~500ms after the first RenderUpdate). The root-cause fix is in PR #28161
  // (viewport-0x0-fix). Remove this bail-out once #28161 lands on master.
  if (viewRect.Height() == 0 || viewRect.Width() == 0)
    return;

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
      srcRect.Height() == m_lastSourceRect.Height() &&
      srcRect.Width() == m_lastSourceRect.Width() && !nonLinStretchChanged && !cmsChanged)
    return;

  // Reload shader when the scaling method, non-linear stretch state, or CMS
  // configuration changes. Also reload when non-linear stretch is currently
  // active, because its shader bakes in viewport-dependent stretch parameters
  // and we may be here due to a viewport change.
  if (m_scalingMethod != requested || nonLinStretchChanged || cmsChanged || m_nonLinStretch)
    m_reloadShaders = true;

  if (cmsChanged)
  {
    const bool hadCms = m_cmsOn;
    if (m_ColorManager->IsEnabled())
    {
      if (!m_ColorManager->CheckConfiguration(m_cmsToken, m_srcPrimaries))
      {
        CLog::Log(LOGDEBUG, "CMS configuration changed, reload LUT");
        LoadCLUT();
      }
      m_cmsOn = true;
    }
    else
    {
      m_cmsOn = false;
    }
    const auto& buf = m_buffers[m_iYUVRenderBuffer];
    if (m_cmsOn != hadCms && buf.videoBuffer)
      SetBufferColorimetry(buf, m_toneMap);
  }

  const ESCALINGMETHOD previousMethod = m_scalingMethod;
  m_scalingMethodGui = requested;
  m_scalingMethod = m_scalingMethodGui;
  m_lastViewRect = viewRect;
  m_lastSourceRect = srcRect;

  if (!Supports(m_scalingMethod))
  {
    CLog::Log(LOGWARNING,
              "CLinuxRendererGL::UpdateVideoFilter - chosen scaling method {}, is not supported by "
              "renderer",
              m_scalingMethod);
    m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  }

  if (m_pVideoFilterShader)
  {
    delete m_pVideoFilterShader;
    m_pVideoFilterShader = nullptr;
  }
  m_fbo.fbo.Cleanup();

  VerifyGLState();

  if (m_scalingMethod == VS_SCALINGMETHOD_AUTO)
  {
    bool scaleSD = m_sourceHeight < 720 && m_sourceWidth < 1280;
    bool scaleUp = (int)m_sourceHeight < m_viewRect.Height() && (int)m_sourceWidth < m_viewRect.Width();
    bool scaleFps = m_fps < CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoAutoScaleMaxFps + 0.01f;

    if (Supports(VS_SCALINGMETHOD_LANCZOS3_FAST) && scaleSD && scaleUp && scaleFps)
      m_scalingMethod = VS_SCALINGMETHOD_LANCZOS3_FAST;
    else
      m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
  }

  if (m_scalingMethod != previousMethod)
    m_reloadShaders = true;

  switch (m_scalingMethod)
  {
  case VS_SCALINGMETHOD_NEAREST:
  case VS_SCALINGMETHOD_LINEAR:
    SetTextureFilter(m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR);
    m_renderQuality = RQ_SINGLEPASS;
    if (m_nonLinStretch)
    {
      m_pVideoFilterShader = new StretchFilterShader();
      if (!m_pVideoFilterShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
        break;
      }
    }
    else
    {
      m_pVideoFilterShader = new DefaultFilterShader();
      if (!m_pVideoFilterShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
        break;
      }
    }
    return;

  case VS_SCALINGMETHOD_LANCZOS3_FAST:
  case VS_SCALINGMETHOD_SPLINE36_FAST:
    {
      EShaderFormat fmt = GetShaderFormat();
      if (fmt == SHADER_NV12 || (fmt >= SHADER_YV12 && fmt <= SHADER_YV12_16))
      {
        unsigned int major, minor;
        m_renderSystem->GetRenderVersion(major, minor);
        if (major >= 4)
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
    if (m_renderMethod & RENDER_GLSL)
    {
      if (!m_fbo.fbo.Initialize())
      {
        CLog::Log(LOGERROR, "GL: Error initializing FBO");
        break;
      }

      // TODO: consider falling back to GL_RGBA8 if requested format fails,
      // as GLES does for GL_RGB10_A2 / GL_RGBA16F on GLES 2.0 contexts.
      if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight,
                                            m_intermediateFormat, m_intermediateType, GL_NEAREST))
      {
        CLog::Log(LOGERROR, "GL: Error creating texture and binding to FBO");
        break;
      }
    }

    if (m_scalingMethod == VS_SCALINGMETHOD_FSR1 || m_scalingMethod == VS_SCALINGMETHOD_FSR1_RCAS)
    {
      m_pVideoFilterShader = new FSR1FilterShader();
    }
    else
    {
      GLSLOutput *out;
      out = new GLSLOutput(3,
          m_useDithering,
          m_ditherDepth,
          m_cmsOn ? m_fullRange : false,
          m_cmsOn ? m_tCLUTTex : 0,
          m_CLUTsize);
      m_pVideoFilterShader = new ConvolutionFilterShader(m_scalingMethod, m_nonLinStretch,
                                                         m_intermediateGammaCorrection, out);
    }
    if (!m_pVideoFilterShader->CompileAndLink())
    {
      CLog::Log(LOGERROR, "GL: Error compiling and linking video filter shader");
      break;
    }

    CLog::Log(LOGINFO, "GL: FBO intermediate format {:#x}",
              static_cast<unsigned>(m_intermediateFormat));
    CLog::Log(LOGINFO, "GL: Selecting multi pass rendering");
    m_renderQuality = RQ_MULTIPASS;
    return;

  case VS_SCALINGMETHOD_FSRCNNX:
  case VS_SCALINGMETHOD_ANIME4K:
  case VS_SCALINGMETHOD_BICUBIC_SOFTWARE:
  case VS_SCALINGMETHOD_LANCZOS_SOFTWARE:
  case VS_SCALINGMETHOD_SINC_SOFTWARE:
  case VS_SCALINGMETHOD_SINC8:
    CLog::Log(LOGERROR, "GL: TODO: This scaler has not yet been implemented");
    break;

  default:
    break;
  }

  CLog::Log(LOGERROR, "GL: Falling back to bilinear due to failure to init scaler");
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
    CLog::Log(LOGERROR, "CLinuxRendererGL::UpdateVideoFilter: Error compiling and linking default "
                        "video filter shader");
  }

  SetTextureFilter(GL_LINEAR);
  m_renderQuality = RQ_SINGLEPASS;
}

void CLinuxRendererGL::LoadShaders(int field)
{
  m_reloadShaders = false;

  if (!LoadShadersHook())
  {
    int requestedMethod = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_RENDERMETHOD);
    CLog::Log(LOGDEBUG, "GL: Requested render method: {}", requestedMethod);

    if (m_pYUVShader)
    {
      delete m_pYUVShader;
      m_pYUVShader = NULL;
    }

    // create regular progressive scan shader
    // if single pass, create GLSLOutput helper and pass it to YUV2RGB shader
    EShaderFormat shaderFormat = GetShaderFormat();
    const auto dstPrimaries = m_passthroughHDR ? m_srcPrimaries :
                               m_shaderSDRBT2020 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    std::shared_ptr<GLSLOutput> out;
    m_toneMapMethod = m_videoSettings.m_ToneMapMethod;
    if (m_renderQuality == RQ_SINGLEPASS)
    {
      out = std::make_shared<GLSLOutput>(GLSLOutput(4, m_useDithering, m_ditherDepth,
                                                    m_cmsOn ? m_fullRange : false,
                                                    m_cmsOn ? m_tCLUTTex : 0,
                                                    m_CLUTsize));

      if (m_scalingMethod == VS_SCALINGMETHOD_LANCZOS3_FAST || m_scalingMethod == VS_SCALINGMETHOD_SPLINE36_FAST)
      {
        m_pYUVShader = new YUV2RGBFilterShader4(
            m_textureTarget == GL_TEXTURE_RECTANGLE, shaderFormat, m_nonLinStretch,
            dstPrimaries, m_srcPrimaries,
            m_toneMap, m_toneMapMethod, m_scalingMethod, out);
        if (!m_cmsOn)
          m_pYUVShader->SetConvertFullColorRange(m_fullRange);

        CLog::Log(LOGINFO, "GL: Selecting YUV 2 RGB shader with filter");

        if (m_pYUVShader && m_pYUVShader->CompileAndLink())
        {
          m_renderMethod = RENDER_GLSL;
          UpdateVideoFilter();
        }
        else
        {
          CLog::Log(LOGERROR, "GL: Error enabling YUV2RGB GLSL shader");
          m_scalingMethod = VS_SCALINGMETHOD_LINEAR;
          delete m_pYUVShader;
          m_pYUVShader = nullptr;
        }
      }
    }

    if (!m_pYUVShader)
    {
      m_pYUVShader = new YUV2RGBProgressiveShader(
          m_textureTarget == GL_TEXTURE_RECTANGLE, shaderFormat,
          m_nonLinStretch && m_renderQuality == RQ_SINGLEPASS,
          dstPrimaries, m_srcPrimaries,
          m_toneMap, m_toneMapMethod, out,
          // Unlike the convolution shaders, FSR1 does not square the intermediate back.
          m_intermediateGammaCorrection && m_renderQuality == RQ_MULTIPASS &&
              m_scalingMethod != VS_SCALINGMETHOD_FSR1 &&
              m_scalingMethod != VS_SCALINGMETHOD_FSR1_RCAS);

      if (!m_cmsOn)
        m_pYUVShader->SetConvertFullColorRange(m_fullRange);

      CLog::Log(LOGINFO, "GL: Selecting YUV 2 RGB shader");

      if (m_pYUVShader && m_pYUVShader->CompileAndLink())
      {
        m_renderMethod = RENDER_GLSL;
        UpdateVideoFilter();
      }
      else
      {
        CLog::Log(LOGERROR, "GL: Error enabling YUV2RGB GLSL shader");
      }
    }
  }

  if (m_pboSupported)
  {
    CLog::Log(LOGINFO, "GL: Using GL_ARB_pixel_buffer_object");
    m_pboUsed = true;
  }
  else
    m_pboUsed = false;
}

void CLinuxRendererGL::UnInit()
{
  CLog::Log(LOGDEBUG, "LinuxRendererGL: Cleaning up GL resources");
  std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());

  glFinish();

  bool outputStopped = false;
#ifdef HAVE_DVBRIDGE
  m_pendingHdrFbo.reset();
  if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
          CServiceBroker::GetWinSystem()))
    outputStopped = window->CancelDeferredOrdinaryVideo();
  m_shaderUpscaler.reset();
  m_rcasShader.reset();
  m_rcasInput.Cleanup();
  if (m_dvRequested)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
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
    ReleaseBuffer(i);
    DeleteTexture(i);
  }

  DeleteCLUT();

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

bool CLinuxRendererGL::IsGuiLayer()
{
  return !m_hdrFboActive && !CServiceBroker::GetWinSystem()->UsesNativeSDRBT2020();
}

#ifdef HAVE_DVBRIDGE
void CLinuxRendererGL::UpdateVideoRenderLayer()
{
  if (!m_pendingHdrFbo && (!m_hdrFboActive || m_dvRequested))
    return;
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
      CServiceBroker::GetWinSystem());
  if (!window)
    return;
  if (m_pendingHdrFbo)
  {
    const auto& picture = m_buffers[m_iYUVRenderBuffer].doviSourcePicture;
    if (!picture || (picture->doviSourceInfo.informationGeneration &&
                     picture->doviSourceInfo.informationGeneration !=
                         CServiceBroker::GetDataCacheCore().GetVideoProcessingGeneration()))
      m_pendingHdrFbo.reset();
  }
  // A refused or cancelled ordinary handoff must not preserve the old late pass.
  const bool target = m_pendingHdrFbo.value_or(
      m_hdrFboActive && (m_dvRequested || window->IsHdrComposite()));
  if (m_hdrFboActive == target)
    return;
  m_hdrFboActive = target;
  window->InvalidateGuiComposite();
}
#endif

bool CLinuxRendererGL::Render(unsigned int flags, int renderBuffer)
{
#ifdef HAVE_DVBRIDGE
  // A decoder flush can leave the previous picture selected until new video arrives.
  if (const auto& picture = m_buffers[renderBuffer].doviSourcePicture;
      picture && picture->doviSourceInfo.informationGeneration &&
      picture->doviSourceInfo.informationGeneration !=
          CServiceBroker::GetDataCacheCore().GetVideoProcessingGeneration())
  {
    return false;
  }
#endif

  // obtain current field, if interlaced
  if( flags & RENDER_FLAG_TOP)
    m_currentField = FIELD_TOP;

  else if (flags & RENDER_FLAG_BOT)
    m_currentField = FIELD_BOT;

  else
    m_currentField = FIELD_FULL;

  // call texture load function
  if (!UploadTexture(renderBuffer))
  {
    return false;
  }

#ifdef HAVE_DVBRIDGE
  const auto& sourcePicture = m_buffers[renderBuffer].doviSourcePicture;
  auto* window = sourcePicture ? dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
      CServiceBroker::GetWinSystem()) : nullptr;
  if (sourcePicture)
  {
    const auto& picture = *sourcePicture;
#ifdef CB1_HAS_HDR10_AI
    // A retained native frame cannot start the new output before the AI queue grows.
    if (SoftwareHDR10AIBufferCount(picture, m_fps, m_renderOrientation) > m_softwareAIBuffers)
      return false;
#endif
    if (!window || !window->PrepareRendererFrame(picture, m_dvRequested))
      return false;
    if (m_dvRequested && window->IsHdrComposite() && IsGuiLayer())
    {
      // The composite video pass must run after this frame's GUI and subtitles.
      m_pendingHdrFbo = true;
      return false;
    }
    if (!m_dvRequested && m_hdrFboActive && !window->IsHdrComposite())
    {
      // Do not present ordinary pixels above a GUI drawn for the former composite.
      window->DeferOrdinaryVideo(m_buffers[renderBuffer].doviSourcePicture);
      m_pendingHdrFbo = false;
      return false;
    }
  }
  if (m_dvRequested)
  {
    m_reportedUpscaler = -2;
    const bool rendered = m_currentField == FIELD_FULL && RenderDVBridge(renderBuffer);
    AfterRenderHook(renderBuffer);
    if (rendered)
      DVBRIDGE::PlaybackNotification::RenderedNative(
          m_buffers[renderBuffer].videoBuffer ? m_buffers[renderBuffer].videoBuffer->PlaybackGeneration() : 0,
          m_buffers[renderBuffer].doviMetadata.Binding());
    return rendered;
  }
#endif
#ifdef HAVE_DVBRIDGE
  int upscaler = -1;
  const int preset = KODI::VIDEO::UPSCALER::Get(
      m_sourceWidth, m_sourceHeight, KODI::VIDEO::UPSCALER::KODI_DEFAULT);
  if (KODI::VIDEO::UPSCALER::UsesShaderHooks(preset) && RenderShaderUpscaler(renderBuffer, preset))
    upscaler = preset;
  else
#endif
  if (RenderHook(renderBuffer))
    ;
  else if (m_renderMethod & RENDER_GLSL)
  {
    UpdateVideoFilter();
    switch(m_renderQuality)
    {
    case RQ_LOW:
    case RQ_SINGLEPASS:
      RenderSinglePass(renderBuffer, m_currentField);
      VerifyGLState();
      break;

    case RQ_MULTIPASS:
      RenderToFBO(renderBuffer, m_currentField);
      RenderFromFBO();
      VerifyGLState();
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

  AfterRenderHook(renderBuffer);
#ifdef HAVE_DVBRIDGE
  const auto generation =
      m_buffers[renderBuffer].videoBuffer ? m_buffers[renderBuffer].videoBuffer->PlaybackGeneration() : 0;
  const auto informationGeneration = m_buffers[renderBuffer].informationGeneration;
  if (informationGeneration != m_reportedUpscalerGeneration || m_reportedUpscaler != upscaler)
  {
    CServiceBroker::GetDataCacheCore().SetVideoUpscaler(upscaler, informationGeneration);
    m_reportedUpscalerGeneration = informationGeneration;
    m_reportedUpscaler = upscaler;
  }
  if (window && sourcePicture)
    window->MarkDeferredOrdinaryVideoDrawn(*sourcePicture);
  DVBRIDGE::PlaybackNotification::RenderedOrdinary(generation);
#ifdef CB1_HAS_HDR10_AI
  if (m_passthroughHDR)
  {
    auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
        CServiceBroker::GetWinSystem());
    if (window && window->HDR10HandoffPending())
    {
      const auto& buffer = m_buffers[renderBuffer];
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

void CLinuxRendererGL::RenderSinglePass(int index, int field)
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

  m_pYUVShader->SetBlack(m_videoSettings.m_Brightness * 0.01f - 0.5f);
  m_pYUVShader->SetContrast(m_videoSettings.m_Contrast * 0.02f);
  m_pYUVShader->SetWidth(planes[0].texwidth);
  m_pYUVShader->SetHeight(planes[0].texheight);
  m_pYUVShader->SetColParams(buf.m_srcColSpace, buf.m_srcBits, !buf.m_srcFullRange, buf.m_srcTextureBits);
  m_pYUVShader->SetDisplayMetadata(buf.hasDisplayMetadata, buf.displayMetadata,
                                   buf.hasLightMetadata, buf.lightMetadata);
  m_pYUVShader->SetToneMapParam(m_toneMapMethod, m_videoSettings.m_ToneMapParam);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (appPlayer->IsInMenu())
    m_pYUVShader->SetNonLinStretch(1.0);
  else
    m_pYUVShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoNonLinStretchRatio));

  if (field == FIELD_TOP)
    m_pYUVShader->SetField(1);
  else if(field == FIELD_BOT)
    m_pYUVShader->SetField(0);

  m_pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  m_pYUVShader->Enable();

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;

  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
    float u2, v2;
    float u3, v3;
  }vertex[4];

  GLint vertLoc = m_pYUVShader->GetVertexLoc();
  GLint Yloc = m_pYUVShader->GetYcoordLoc();
  GLint Uloc = m_pYUVShader->GetUcoordLoc();
  GLint Vloc = m_pYUVShader->GetVcoordLoc();

  // Setup vertex position values
  for(int i = 0; i < 4; i++)
  {
    vertex[i].x = m_rotatedDestCoords[i].x;
    vertex[i].y = m_rotatedDestCoords[i].y;
    vertex[i].z = 0.0f;// set z to 0
  }

  // bottom left
  vertex[0].u1 = planes[0].rect.x1;
  vertex[0].v1 = planes[0].rect.y1;
  vertex[0].u2 = planes[1].rect.x1;
  vertex[0].v2 = planes[1].rect.y1;
  vertex[0].u3 = planes[2].rect.x1;
  vertex[0].v3 = planes[2].rect.y1;

  // bottom right
  vertex[1].u1 = planes[0].rect.x2;
  vertex[1].v1 = planes[0].rect.y1;
  vertex[1].u2 = planes[1].rect.x2;
  vertex[1].v2 = planes[1].rect.y1;
  vertex[1].u3 = planes[2].rect.x2;
  vertex[1].v3 = planes[2].rect.y1;

  // top right
  vertex[2].u1 = planes[0].rect.x2;
  vertex[2].v1 = planes[0].rect.y2;
  vertex[2].u2 = planes[1].rect.x2;
  vertex[2].v2 = planes[1].rect.y2;
  vertex[2].u3 = planes[2].rect.x2;
  vertex[2].v3 = planes[2].rect.y2;

  // top left
  vertex[3].u1 = planes[0].rect.x1;
  vertex[3].v1 = planes[0].rect.y2;
  vertex[3].u2 = planes[1].rect.x1;
  vertex[3].v2 = planes[1].rect.y2;
  vertex[3].u3 = planes[2].rect.x1;
  vertex[3].v3 = planes[2].rect.y2;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, x)));
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u1)));
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u2)));
  if (Vloc != -1)
    glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                          reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u3)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  if (Vloc != -1)
    glEnableVertexAttribArray(Vloc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);
  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  if (Vloc != -1)
    glDisableVertexAttribArray(Vloc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  m_pYUVShader->Disable();
  VerifyGLState();

  VerifyGLState();
}

void CLinuxRendererGL::RenderToFBO(int index, int field, bool weave /*= false*/)
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
    CLog::Log(LOGWARNING, "GL: multipass FBO invalid at render time, recreating");

    if (!m_fbo.fbo.Initialize())
    {
      CLog::Log(LOGERROR, "GL: Error initializing FBO");
      return;
    }

    // Recreate with the format/type UpdateVideoFilter settled on, so the
    // intermediate FBO keeps the bit depth configured by hqscalerprecision.
    if (!m_fbo.fbo.CreateAndBindToTexture(GL_TEXTURE_2D, m_sourceWidth, m_sourceHeight,
                                          m_intermediateFormat, m_intermediateType, GL_NEAREST))
    {
      CLog::Log(LOGERROR, "GL: Error creating texture and binding to FBO");
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

  // make sure the yuv shader is loaded and ready to go
  if (!m_pYUVShader || (!m_pYUVShader->OK()))
  {
    CLog::Log(LOGERROR, "GL: YUV shader not active, cannot do multipass render");
    return;
  }

  m_fbo.fbo.BeginRender();
  VerifyGLState();

  m_pYUVShader->SetBlack(m_videoSettings.m_Brightness * 0.01f - 0.5f);
  m_pYUVShader->SetContrast(m_videoSettings.m_Contrast * 0.02f);
  m_pYUVShader->SetWidth(planes[0].texwidth);
  m_pYUVShader->SetHeight(planes[0].texheight);
  m_pYUVShader->SetNonLinStretch(1.0);
  m_pYUVShader->SetColParams(buf.m_srcColSpace, buf.m_srcBits, !buf.m_srcFullRange, buf.m_srcTextureBits);
  m_pYUVShader->SetDisplayMetadata(buf.hasDisplayMetadata, buf.displayMetadata,
                                   buf.hasLightMetadata, buf.lightMetadata);
  m_pYUVShader->SetToneMapParam(m_toneMapMethod, m_videoSettings.m_ToneMapParam);

  if (field == FIELD_TOP)
    m_pYUVShader->SetField(1);
  else if (field == FIELD_BOT)
    m_pYUVShader->SetField(0);

  VerifyGLState();

  glMatrixModview.Push();
  glMatrixModview->LoadIdentity();
  glMatrixModview.Load();

  glMatrixProject.Push();
  glMatrixProject->LoadIdentity();
  glMatrixProject->Ortho2D(0, m_sourceWidth, 0, m_sourceHeight);
  glMatrixProject.Load();

  CRect viewport;
  m_renderSystem->GetViewPort(viewport);
  glViewport(0, 0, m_sourceWidth, m_sourceHeight);
  glScissor (0, 0, m_sourceWidth, m_sourceHeight);

  m_pYUVShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  if (!m_pYUVShader->Enable())
  {
    CLog::Log(LOGERROR, "GL: Error enabling YUV shader");
  }

  m_fbo.width = planes[0].rect.x2 - planes[0].rect.x1;
  m_fbo.height = planes[0].rect.y2 - planes[0].rect.y1;

  if (m_textureTarget == GL_TEXTURE_2D)
  {
    m_fbo.width  *= planes[0].texwidth;
    m_fbo.height *= planes[0].texheight;
  }
  m_fbo.width  *= planes[0].pixpertex_x;
  m_fbo.height *= planes[0].pixpertex_y;
  if (weave)
    m_fbo.height *= 2;

  // 1st Pass to video frame size
  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;
  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
    float u2, v2;
    float u3, v3;
  } vertex[4];

  GLint vertLoc = m_pYUVShader->GetVertexLoc();
  GLint Yloc = m_pYUVShader->GetYcoordLoc();
  GLint Uloc = m_pYUVShader->GetUcoordLoc();
  GLint Vloc = m_pYUVShader->GetVcoordLoc();

  // top left
  vertex[0].x = 0.0f;
  vertex[0].y = 0.0f;
  vertex[0].z = 0.0f;
  vertex[0].u1 = planes[0].rect.x1;
  vertex[0].v1 = planes[0].rect.y1;
  vertex[0].u2 = planes[1].rect.x1;
  vertex[0].v2 = planes[1].rect.y1;
  vertex[0].u3 = planes[2].rect.x1;
  vertex[0].v3 = planes[2].rect.y1;

  // top right
  vertex[1].x = m_fbo.width;
  vertex[1].y = 0.0f;
  vertex[1].z = 0.0f;
  vertex[1].u1 = planes[0].rect.x2;
  vertex[1].v1 = planes[0].rect.y1;
  vertex[1].u2 = planes[1].rect.x2;
  vertex[1].v2 = planes[1].rect.y1;
  vertex[1].u3 = planes[2].rect.x2;
  vertex[1].v3 = planes[2].rect.y1;

  // bottom right
  vertex[2].x = m_fbo.width;
  vertex[2].y = m_fbo.height;
  vertex[2].z = 0.0f;
  vertex[2].u1 = planes[0].rect.x2;
  vertex[2].v1 = planes[0].rect.y2;
  vertex[2].u2 = planes[1].rect.x2;
  vertex[2].v2 = planes[1].rect.y2;
  vertex[2].u3 = planes[2].rect.x2;
  vertex[2].v3 = planes[2].rect.y2;

  // bottom left
  vertex[3].x = 0.0f;
  vertex[3].y = m_fbo.height;
  vertex[3].z = 0.0f;
  vertex[3].u1 = planes[0].rect.x1;
  vertex[3].v1 = planes[0].rect.y2;
  vertex[3].u2 = planes[1].rect.x1;
  vertex[3].v2 = planes[1].rect.y2;
  vertex[3].u3 = planes[2].rect.x1;
  vertex[3].v3 = planes[2].rect.y2;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, x)));
  glVertexAttribPointer(Yloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u1)));
  glVertexAttribPointer(Uloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u2)));
  if (Vloc != -1)
    glVertexAttribPointer(Vloc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                          reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u3)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(Yloc);
  glEnableVertexAttribArray(Uloc);
  if (Vloc != -1)
    glEnableVertexAttribArray(Vloc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);
  VerifyGLState();

  glDisableVertexAttribArray(vertLoc);
  glDisableVertexAttribArray(Yloc);
  glDisableVertexAttribArray(Uloc);
  if (Vloc != -1)
    glDisableVertexAttribArray(Vloc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  m_pYUVShader->Disable();

  glMatrixModview.PopLoad();
  glMatrixProject.PopLoad();

  m_renderSystem->SetViewPort(viewport);

  m_fbo.fbo.EndRender();
#ifdef HAVE_DVBRIDGE
  if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
          CServiceBroker::GetWinSystem()))
    window->BindNativePascalHDRFramebuffer();
#endif

  VerifyGLState();
}

#ifdef HAVE_DVBRIDGE
bool CLinuxRendererGL::RunRCAS()
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
    const GLfloat u = m_fbo.width / m_sourceWidth;
    const GLfloat v = m_fbo.height / m_sourceHeight;
    const GLfloat vertices[] = {-1,-1,0,0, 1,-1,u,0, -1,1,0,v, 1,1,u,v};
    const GLint position = easu->GetVertexLoc();
    const GLint coord = easu->GetCoordLoc();
    GLuint vertexVBO;
    glGenBuffers(1, &vertexVBO);
    glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(position, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat), nullptr);
    glVertexAttribPointer(coord, 2, GL_FLOAT, 0, 4 * sizeof(GLfloat),
                          reinterpret_cast<const GLvoid*>(2 * sizeof(GLfloat)));
    glEnableVertexAttribArray(position);
    glEnableVertexAttribArray(coord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(position);
    glDisableVertexAttribArray(coord);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vertexVBO);
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

void CLinuxRendererGL::RenderFromFBO()
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
  VerifyGLState();

  // Use regular normalized texture coordinates
  // 2nd Pass to screen size with optional video filter

  if (!filterShader)
  {
    CLog::Log(LOGERROR, "CLinuxRendererGL::RenderFromFBO - no videofilter shader");
    return;
  }

  GLint filter;
  if (!filterShader->GetTextureFilter(filter))
    filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;

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
    filterShader->SetAlpha(1.0f);
  }
#endif
  filterShader->SetWidth(sampleWidth);
  filterShader->SetHeight(sampleHeight);
  if (m_scalingMethod == VS_SCALINGMETHOD_FSR1)
    static_cast<FSR1FilterShader*>(filterShader)->SetInputSize(m_fbo.width, m_fbo.height);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (appPlayer->IsInMenu())
    filterShader->SetNonLinStretch(1.0);
  else
    filterShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoNonLinStretchRatio));

  filterShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  filterShader->Enable();

  VerifyGLState();

  float imgwidth = sharpened ? 1.f : m_fbo.width / m_sourceWidth;
  float imgheight = sharpened ? 1.f : m_fbo.height / m_sourceHeight;

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;
  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
  } vertex[4];

  GLint vertLoc = filterShader->GetVertexLoc();
  GLint loc = filterShader->GetCoordLoc();

  // Setup vertex position values
  // top left
  vertex[0].x = m_rotatedDestCoords[0].x;
  vertex[0].y = m_rotatedDestCoords[0].y;
  vertex[0].z = 0.0f;
  vertex[0].u1 = 0.0;
  vertex[0].v1 = 0.0;

  // top right
  vertex[1].x = m_rotatedDestCoords[1].x;
  vertex[1].y = m_rotatedDestCoords[1].y;
  vertex[1].z = 0.0f;
  vertex[1].u1 = imgwidth;
  vertex[1].v1 = 0.0f;

  // bottom right
  vertex[2].x = m_rotatedDestCoords[2].x;
  vertex[2].y = m_rotatedDestCoords[2].y;
  vertex[2].z = 0.0f;
  vertex[2].u1 = imgwidth;
  vertex[2].v1 = imgheight;

  // bottom left
  vertex[3].x = m_rotatedDestCoords[3].x;
  vertex[3].y = m_rotatedDestCoords[3].y;
  vertex[3].z = 0.0f;
  vertex[3].u1 = 0.0f;
  vertex[3].v1 = imgheight;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, x)));
  glVertexAttribPointer(loc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u1)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(loc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);
  VerifyGLState();

  glDisableVertexAttribArray(loc);
  glDisableVertexAttribArray(vertLoc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  filterShader->Disable();

  VerifyGLState();

  glBindTexture(GL_TEXTURE_2D, 0);
  VerifyGLState();
}

void CLinuxRendererGL::RenderProgressiveWeave(int index, int field)
{
  bool scale = (int)m_sourceHeight != m_destRect.Height() ||
               (int)m_sourceWidth != m_destRect.Width();

  if (m_fbo.fbo.IsSupported() && (scale || m_renderQuality == RQ_MULTIPASS))
  {
    glEnable(GL_POLYGON_STIPPLE);
    glPolygonStipple(stipple_weave);
    RenderToFBO(index, FIELD_TOP, true);
    glPolygonStipple(stipple_weave+4);
    RenderToFBO(index, FIELD_BOT, true);
    glDisable(GL_POLYGON_STIPPLE);
    RenderFromFBO();
  }
  else
  {
    glEnable(GL_POLYGON_STIPPLE);
    glPolygonStipple(stipple_weave);
    RenderSinglePass(index, FIELD_TOP);
    glPolygonStipple(stipple_weave+4);
    RenderSinglePass(index, FIELD_BOT);
    glDisable(GL_POLYGON_STIPPLE);
  }
}

void CLinuxRendererGL::RenderRGB(int index, int field)
{
  CYuvPlane &plane = m_buffers[index].fields[FIELD_FULL][0];

  glActiveTexture(GL_TEXTURE0);

  glBindTexture(m_textureTarget, plane.id);

  // make sure we know the correct texture size
  GetPlaneTextureSize(plane);

  if (!m_pVideoFilterShader)
  {
    CLog::Log(LOGERROR, "CLinuxRendererGL::RenderRGB - no videofilter shader");
    return;
  }

  GLint filter;
  if (!m_pVideoFilterShader->GetTextureFilter(filter))
    filter = m_scalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;

  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, filter);
  m_pVideoFilterShader->SetSourceTexture(0);
  m_pVideoFilterShader->SetWidth(m_sourceWidth);
  m_pVideoFilterShader->SetHeight(m_sourceHeight);

  //disable non-linear stretch when a dvd menu is shown, parts of the menu are rendered through the overlay renderer
  //having non-linear stretch on breaks the alignment
  const auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  if (appPlayer->IsInMenu())
    m_pVideoFilterShader->SetNonLinStretch(1.0);
  else
    m_pVideoFilterShader->SetNonLinStretch(pow(CDisplaySettings::GetInstance().GetPixelRatio(), CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoNonLinStretchRatio));

  m_pVideoFilterShader->SetMatrices(glMatrixProject.Get(), glMatrixModview.Get());
  m_pVideoFilterShader->Enable();

  GLubyte idx[4] = {0, 1, 3, 2};  //determines order of the vertices
  GLuint vertexVBO;
  GLuint indexVBO;
  struct PackedVertex
  {
    float x, y, z;
    float u1, v1;
  } vertex[4];

  GLint vertLoc = m_pVideoFilterShader->GetVertexLoc();
  GLint loc = m_pVideoFilterShader->GetCoordLoc();

  // Setup vertex position values
  // top left
  vertex[0].x = m_rotatedDestCoords[0].x;
  vertex[0].y = m_rotatedDestCoords[0].y;
  vertex[0].z = 0.0f;
  vertex[0].u1 = plane.rect.x1;
  vertex[0].v1 = plane.rect.y1;

  // top right
  vertex[1].x = m_rotatedDestCoords[1].x;
  vertex[1].y = m_rotatedDestCoords[1].y;
  vertex[1].z = 0.0f;
  vertex[1].u1 = plane.rect.x2;
  vertex[1].v1 = plane.rect.y1;

  // bottom right
  vertex[2].x = m_rotatedDestCoords[2].x;
  vertex[2].y = m_rotatedDestCoords[2].y;
  vertex[2].z = 0.0f;
  vertex[2].u1 = plane.rect.x2;
  vertex[2].v1 = plane.rect.y2;

  // bottom left
  vertex[3].x = m_rotatedDestCoords[3].x;
  vertex[3].y = m_rotatedDestCoords[3].y;
  vertex[3].z = 0.0f;
  vertex[3].u1 = plane.rect.x1;
  vertex[3].v1 = plane.rect.y2;

  glGenBuffers(1, &vertexVBO);
  glBindBuffer(GL_ARRAY_BUFFER, vertexVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex)*4, &vertex[0], GL_STATIC_DRAW);

  glVertexAttribPointer(vertLoc, 3, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, x)));
  glVertexAttribPointer(loc, 2, GL_FLOAT, 0, sizeof(PackedVertex),
                        reinterpret_cast<const GLvoid*>(offsetof(PackedVertex, u1)));

  glEnableVertexAttribArray(vertLoc);
  glEnableVertexAttribArray(loc);

  glGenBuffers(1, &indexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexVBO);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLubyte)*4, idx, GL_STATIC_DRAW);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &vertexVBO);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, &indexVBO);

  VerifyGLState();

  m_pVideoFilterShader->Disable();

  glBindTexture(m_textureTarget, 0);
}

GLint CLinuxRendererGL::GetInternalFormat(GLint format, int bpp)
{
  unsigned int major, minor;
  m_renderSystem->GetRenderVersion(major, minor);
  if (bpp == 2)
  {
    if (format == GL_RED)
    {
      if (major > 2)
        return GL_R16;
      else
        return GL_LUMINANCE16;
    }
  }
  else
  {
    if (format == GL_RED)
    {
      if (major > 2)
        return GL_RED;
      else
        return GL_LUMINANCE;
    }
  }

  return format;
}

//-----------------------------------------------------------------------------
// Textures
//-----------------------------------------------------------------------------

bool CLinuxRendererGL::CreateTexture(int index)
{
  if (m_format == AV_PIX_FMT_NV12)
    return CreateNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
    return CreatePackedYUVTexture(index);
  else
    return CreatePlanarYUVTexture(index);
}

void CLinuxRendererGL::DeleteTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  buf.loaded = false;

  if (m_format == AV_PIX_FMT_NV12)
    DeleteNV12Texture(index);
  else if (m_format == AV_PIX_FMT_YUYV422 ||
           m_format == AV_PIX_FMT_UYVY422)
    DeletePackedYUVTexture(index);
  else
    DeletePlanarYUVTexture(index);
}

bool CLinuxRendererGL::UploadTexture(int index)
{
  if (!m_buffers[index].videoBuffer)
    return false;

  bool ret = true;

  if (!m_buffers[index].loaded)
  {
    YuvImage &dst = m_buffers[index].image;
    YuvImage src;
    m_buffers[index].videoBuffer->GetPlanes(src.plane);
    m_buffers[index].videoBuffer->GetStrides(src.stride);

    UnBindPbo(m_buffers[index]);

    if (m_format == AV_PIX_FMT_NV12)
    {
      CVideoBuffer::CopyNV12Picture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadNV12Texture(index);
    }
    else if (m_format == AV_PIX_FMT_YUYV422 ||
             m_format == AV_PIX_FMT_UYVY422)
    {
      CVideoBuffer::CopyYUV422PackedPicture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadPackedYUVTexture(index);
    }
    else
    {
      CVideoBuffer::CopyPicture(&dst, &src);
      BindPbo(m_buffers[index]);
      ret = UploadPlanarYUVTexture(index);
    }

    if (ret)
      m_buffers[index].loaded = true;
  }

  if (ret)
    CalculateTextureSourceRects(index, 3);

#ifdef HAVE_DVBRIDGE
  if (auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(CServiceBroker::GetWinSystem()))
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
bool CLinuxRendererGL::BuildDVBridgeLayer(int index, CDVBridgeGLES::Layer& layer)
{
  auto& buffer=m_buffers[index];
  if (!buffer.doviSourcePicture || m_textureTarget!=GL_TEXTURE_2D || m_renderOrientation!=0)
    return false;
  const auto& source=*buffer.doviSourcePicture;
  const bool hdr10 = !source.doviSourceInfo.profile && source.doviSourceBinding.Valid() &&
      DVBRIDGE::IsHDR10Family(source.hdrType);
  const bool sdr = DVBRIDGE::IsSDRExpansionSource(source) && source.doviSourceBinding.Valid();
  const bool plain = hdr10 || sdr;
  if (!plain && (!buffer.doviMetadata.Data() || buffer.doviPts==DVD_NOPTS_VALUE ||
      !std::isfinite(buffer.doviPts))) return false;
  if (m_sourceWidth<=0 || m_sourceWidth>3840 || m_sourceHeight<=0 || m_sourceHeight>2160)
    return false;
  switch (m_format)
  {
#ifdef CB1_HAS_HDR10_AI
    case AV_PIX_FMT_YUV420P:
      if (!m_planarRed8 || !(sdr || (hdr10 && source.IsLimitedHLG8()))) return false;
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
  layer.pts=plain ? source.pts / DVD_TIME_BASE : buffer.doviPts;
  layer.binding=plain ? source.doviSourceBinding : buffer.doviMetadata.Binding();
  if (!std::isfinite(layer.pts) || (plain && (source.pts==DVD_NOPTS_VALUE ||
      source.doviSourcePts==AV_NOPTS_VALUE))) return false;
  if (plain)
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
  layer.sdrInput = sdr;
  layer.sdrMatrix = source.color_space;
  layer.sdrPrimaries = source.color_primaries;
  layer.sdrTransfer = source.color_transfer;
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
bool CLinuxRendererGL::PrepareHDR10AIQueue(const std::deque<int>& queue, bool end, bool paused,
    uint64_t presentationDeadline)
{
  auto preparation = CaptureHDR10AIQueue(queue,end,paused,presentationDeadline);
  if (!preparation.execute) return preparation.ready;
  const auto outcome = preparation.execute();
  return preparation.publish(outcome) && outcome.ready;
}

HDR10AIQueuePreparation CLinuxRendererGL::CaptureHDR10AIQueue(
    const std::deque<int>& queue, bool end, bool paused, uint64_t presentationDeadline)
{
  if (queue.empty() || !HDR10AIQueueEligible()) return {};
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
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

bool CLinuxRendererGL::HDR10AIFrameCommitted() const
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(CServiceBroker::GetWinSystem());
  return window && window->HDR10AIFrameCommitted();
}
#endif

bool CLinuxRendererGL::RenderDVBridge(int index)
{
  auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(CServiceBroker::GetWinSystem());
  CDVBridgeGLES::Layer layer{};
  if (!window || !BuildDVBridgeLayer(index,layer)) return false;
  const auto& buffer=m_buffers[index];
  return window->RenderDVBridge(layer,buffer.doviEnhancement,buffer.doviEnhancementBinding,
      buffer.doviMetadata,DVBridgeGeometry());
}

dvbridge_geometry CLinuxRendererGL::DVBridgeGeometry()
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
bool CLinuxRendererGL::RenderShaderUpscaler(int index, int preset)
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
      (bt709 && (buf.m_srcBits == 8 || buf.m_srcBits == 10));
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
  const bool planar10 = format == SHADER_YV12_10 && m_format == AV_PIX_FMT_YUV420P10LE &&
      GetInternalFormat(GL_RED, 2) == GL_R16;
  const auto sourceLayout = KODI::VIDEO::UPSCALER::ShaderSource(
      hdr, buf.m_srcBits, format == SHADER_YV12,
      format == SHADER_NV12 || format == SHADER_NV12_RRG, planar10,
      !hdr && buf.m_srcBits == 10 && HasShaderUpscalerP010(index));
  if (!sourceLayout.planes)
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
  image.num_planes = sourceLayout.planes;
  image.repr.sys = hdr ? PL_COLOR_SYSTEM_BT_2020_NC : PL_COLOR_SYSTEM_BT_709;
  image.repr.levels = buf.m_srcFullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  image.color = hdr ? pl_color_space_hdr10 : pl_color_space_bt709;
  if (hlg)
    image.color.transfer = PL_COLOR_TRC_HLG;
  if (sourceLayout.sampleDepth == 16)
  {
    image.repr.bits.sample_depth = sourceLayout.sampleDepth;
    image.repr.bits.color_depth = buf.m_srcBits;
    image.repr.bits.bit_shift = sourceLayout.bitShift;
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
    params.iformat = sourceLayout.sampleDepth == 16 ? (uv ? GL_RG16 : GL_R16)
                                                  : (uv ? GL_RG8 : GL_R8);
    image.planes[i].texture = plane.id ? pl_opengl_wrap(gpu, &params) : nullptr;
    image.planes[i].components = i == 1 && image.num_planes == 2 ? 2 : 1;
    image.planes[i].component_mapping[0] = i;
    image.planes[i].component_mapping[1] = i + 1;
  }
  pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
  GLint framebuffer, viewport[4], redBits, vertexArray;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
  glGetIntegerv(GL_VIEWPORT, viewport);
  // Core profiles dropped GL_RED_BITS and call the default colour buffer GL_BACK_LEFT.
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
      framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
      &redBits);
  // libplacebo leaves vertex array 0 bound, which a core profile cannot draw with.
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
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
  if (!hdr && CServiceBroker::GetWinSystem()->UsesNativeSDRBT2020())
    output.color.primaries = PL_COLOR_PRIM_BT_2020;
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
  glBindVertexArray(vertexArray);
  pl_tex_destroy(gpu, &output.planes[0].texture);
  for (int i = 0; i < image.num_planes; ++i)
    pl_tex_destroy(gpu, &image.planes[i].texture);
  return rendered;
}
#endif

//********************************************************************************************************
// YUV texture creation, deletion, copying + clearing
//********************************************************************************************************

bool CLinuxRendererGL::CreatePlanarYUVTexture(int index)
{
  /* since we also want the field textures, pitch must be texture aligned */
  unsigned p;

  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  DeletePlanarYUVTexture(index);

  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  // Chroma subsampling and bit depth derived from libavutil's pixdesc API,
  // avoiding a parallel per-pix_fmt switch. Works for any planar YUV pix_fmt
  // (4:2:0 / 4:2:2 / 4:4:4 at any supported bit depth).
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

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(3, pbo);

    for (int i = 0; i < 3; i++)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW);
      void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*) pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 3; i++)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      }
      glDeleteBuffers(3, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 3; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(p = 0;p<YuvImage::MAX_PLANES;p++)
    {
      if (!glIsTexture(m_buffers[index].fields[f][p].id))
      {
        glGenTextures(1, &m_buffers[index].fields[f][p].id);
        VerifyGLState();
      }
      m_buffers[index].fields[f][p].pbo = pbo[p];
    }
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
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

    for (int p = 0; p < 3; p++)
    {
      CYuvPlane &plane = planes[p];
      if (plane.texwidth * plane.texheight == 0)
        continue;

      glBindTexture(m_textureTarget, plane.id);
      GLint internalformat;
      internalformat = GetInternalFormat(GL_RED, im.bpp);
      if (im.bpp == 2)
        glTexImage2D(m_textureTarget, 0, internalformat, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_SHORT, NULL);
      else
        glTexImage2D(m_textureTarget, 0, internalformat, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }
  return true;
}

bool CLinuxRendererGL::UploadPlanarYUVTexture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT,1);

  if (deinterlacing)
  {
    // Load Even Y Field
    LoadPlane(buf.fields[FIELD_TOP][0] , GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] );

    //load Odd Y Field
    LoadPlane(buf.fields[FIELD_BOT][0], GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] + im->stride[0]);

    // Load Even U & V Fields
    LoadPlane(buf.fields[FIELD_TOP][1], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1]);

    LoadPlane(buf.fields[FIELD_TOP][2], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[2]*2, im->bpp, im->plane[2]);

    // Load Odd U & V Fields
    LoadPlane(buf.fields[FIELD_BOT][1], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1] + im->stride[1]);

    LoadPlane(buf.fields[FIELD_BOT][2], GL_RED,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[2]*2, im->bpp, im->plane[2] + im->stride[2]);
  }
  else
  {
    //Load Y plane
    LoadPlane(buf.fields[FIELD_FULL][0], GL_RED,
              im->width, im->height,
              im->stride[0], im->bpp, im->plane[0]);

    //load U plane
    LoadPlane(buf.fields[FIELD_FULL][1], GL_RED,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[1], im->bpp, im->plane[1]);

    //load V plane
    LoadPlane(buf.fields[FIELD_FULL][2], GL_RED,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[2], im->bpp, im->plane[2]);
  }

  VerifyGLState();

  return true;
}

void CLinuxRendererGL::DeletePlanarYUVTexture(int index)
{
  YuvImage &im = m_buffers[index].image;
  GLuint *pbo = m_buffers[index].pbo;

  if (m_buffers[index].fields[FIELD_FULL][0].id == 0)
    return;

  /* finish up all textures, and delete them */
  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<YuvImage::MAX_PLANES;p++)
    {
      if (m_buffers[index].fields[f][p].id)
      {
        if (glIsTexture(m_buffers[index].fields[f][p].id))
          glDeleteTextures(1, &m_buffers[index].fields[f][p].id);
        m_buffers[index].fields[f][p].id = 0;
      }
    }
  }

  for(int p = 0;p<YuvImage::MAX_PLANES;p++)
  {
    if (pbo[p])
    {
      if (im.plane[p])
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[p]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        im.plane[p] = NULL;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      }
      glDeleteBuffers(1, pbo + p);
      pbo[p] = 0;
    }
    else
    {
      if (im.plane[p])
      {
        delete[] im.plane[p];
        im.plane[p] = NULL;
      }
    }
  }
}

//********************************************************************************************************
// NV12 Texture loading, creation and deletion
//********************************************************************************************************
bool CLinuxRendererGL::UploadNV12Texture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT, im->bpp);

  if (deinterlacing)
  {
    // Load Odd Y field
    LoadPlane(buf.fields[FIELD_TOP][0] , GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0]);

    // Load Even Y field
    LoadPlane(buf.fields[FIELD_BOT][0], GL_RED,
              im->width, im->height >> 1,
              im->stride[0]*2, im->bpp, im->plane[0] + im->stride[0]) ;

    // Load Odd UV Fields
    LoadPlane(buf.fields[FIELD_TOP][1], GL_RG,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1]);

    // Load Even UV Fields
    LoadPlane(buf.fields[FIELD_BOT][1], GL_RG,
              im->width >> im->cshift_x, im->height >> (im->cshift_y + 1),
              im->stride[1]*2, im->bpp, im->plane[1] + im->stride[1]);

  }
  else
  {
    // Load Y plane
    LoadPlane(buf. fields[FIELD_FULL][0], GL_RED,
              im->width, im->height,
              im->stride[0], im->bpp, im->plane[0]);

    // Load UV plane
    LoadPlane(buf.fields[FIELD_FULL][1], GL_RG,
              im->width >> im->cshift_x, im->height >> im->cshift_y,
              im->stride[1], im->bpp, im->plane[1]);
  }

  VerifyGLState();

  return true;
}

bool CLinuxRendererGL::CreateNV12Texture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

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

  im.plane[0] = NULL;
  im.plane[1] = NULL;
  im.plane[2] = NULL;

  // Y plane
  im.planesize[0] = im.stride[0] * im.height;
  // packed UV plane
  im.planesize[1] = im.stride[1] * im.height / 2;
  // third plane is not used
  im.planesize[2] = 0;

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(2, pbo);

    for (int i = 0; i < 2; i++)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[i] + PBO_OFFSET, 0, GL_STREAM_DRAW);
      void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
      if (pboPtr)
      {
        im.plane[i] = (uint8_t*)pboPtr + PBO_OFFSET;
        memset(im.plane[i], 0, im.planesize[i]);
      }
      else
      {
        CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
        pboSetup = false;
        break;
      }
    }

    if (!pboSetup)
    {
      for (int i = 0; i < 2; i++)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[i]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      }
      glDeleteBuffers(2, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    for (int i = 0; i < 2; i++)
      im.plane[i] = new uint8_t[im.planesize[i]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<2;p++)
    {
      if (!glIsTexture(buf.fields[f][p].id))
      {
        glGenTextures(1, &buf.fields[f][p].id);
        VerifyGLState();
      }
      buf.fields[f][p].pbo = pbo[p];
    }
    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
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
        continue;

      glBindTexture(m_textureTarget, plane.id);
      if (p == 1)
        glTexImage2D(m_textureTarget, 0, GL_RG, plane.texwidth, plane.texheight, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
      else
        glTexImage2D(m_textureTarget, 0, GL_RED, plane.texwidth, plane.texheight, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

      glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      VerifyGLState();
    }
  }

  return true;
}

void CLinuxRendererGL::DeleteNV12Texture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  if (buf.fields[FIELD_FULL][0].id == 0)
    return;

  // finish up all textures, and delete them
  for(int f = 0;f<MAX_FIELDS;f++)
  {
    for(int p = 0;p<2;p++)
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

  for(int p = 0;p<2;p++)
  {
    if (pbo[p])
    {
      if (im.plane[p])
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[p]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        im.plane[p] = NULL;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      }
      glDeleteBuffers(1, pbo + p);
      pbo[p] = 0;
    }
    else
    {
      if (im.plane[p])
      {
        delete[] im.plane[p];
        im.plane[p] = NULL;
      }
    }
  }
}

bool CLinuxRendererGL::UploadPackedYUVTexture(int source)
{
  CPictureBuffer& buf = m_buffers[source];
  YuvImage* im = &buf.image;

  bool deinterlacing;
  if (m_currentField == FIELD_FULL)
    deinterlacing = false;
  else
    deinterlacing = true;

  VerifyGLState();

  glPixelStorei(GL_UNPACK_ALIGNMENT,1);

  if (deinterlacing)
  {
    // Load YUYV fields
    LoadPlane(buf.fields[FIELD_TOP][0], GL_BGRA,
              im->width / 2, im->height >> 1,
              im->stride[0] * 2, im->bpp, im->plane[0]);

    LoadPlane(buf.fields[FIELD_BOT][0], GL_BGRA,
              im->width / 2, im->height >> 1,
              im->stride[0] * 2, im->bpp, im->plane[0] + im->stride[0]);
  }
  else
  {
    // Load YUYV plane
    LoadPlane(buf.fields[FIELD_FULL][0], GL_BGRA,
              im->width / 2, im->height,
              im->stride[0], im->bpp, im->plane[0]);
  }

  VerifyGLState();

  return true;
}

void CLinuxRendererGL::DeletePackedYUVTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  if (buf.fields[FIELD_FULL][0].id == 0)
    return;

  // finish up all textures, and delete them
  for (int f = 0;f<MAX_FIELDS;f++)
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

  if (pbo[0])
  {
    if (im.plane[0])
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[0]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      im.plane[0] = NULL;
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    glDeleteBuffers(1, pbo);
    pbo[0] = 0;
  }
  else
  {
    if (im.plane[0])
    {
      delete[] im.plane[0];
      im.plane[0] = NULL;
    }
  }
}

bool CLinuxRendererGL::CreatePackedYUVTexture(int index)
{
  // since we also want the field textures, pitch must be texture aligned
  CPictureBuffer& buf = m_buffers[index];
  YuvImage &im = buf.image;
  GLuint *pbo = buf.pbo;

  // Delete any old texture
  DeletePackedYUVTexture(index);

  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 0;
  im.cshift_y = 0;
  im.bpp = 1;

  im.stride[0] = im.width * 2;
  im.stride[1] = 0;
  im.stride[2] = 0;

  im.plane[0] = NULL;
  im.plane[1] = NULL;
  im.plane[2] = NULL;

  // packed YUYV plane
  im.planesize[0] = im.stride[0] * im.height;
  // second plane is not used
  im.planesize[1] = 0;
  // third plane is not used
  im.planesize[2] = 0;

  bool pboSetup = false;
  if (m_pboUsed)
  {
    pboSetup = true;
    glGenBuffers(1, pbo);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[0]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, im.planesize[0] + PBO_OFFSET, 0, GL_STREAM_DRAW);
    void* pboPtr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (pboPtr)
    {
      im.plane[0] = (uint8_t*)pboPtr + PBO_OFFSET;
      memset(im.plane[0], 0, im.planesize[0]);
    }
    else
    {
      CLog::Log(LOGWARNING,"GL: failed to set up pixel buffer object");
      pboSetup = false;
    }

    if (!pboSetup)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, *pbo);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glDeleteBuffers(1, pbo);
      memset(m_buffers[index].pbo, 0, sizeof(m_buffers[index].pbo));
    }

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  if (!pboSetup)
  {
    im.plane[0] = new uint8_t[im.planesize[0]];
  }

  for(int f = 0;f<MAX_FIELDS;f++)
  {
    if (!glIsTexture(buf.fields[f][0].id))
    {
      glGenTextures(1, &buf.fields[f][0].id);
      VerifyGLState();
    }
    buf.fields[f][0].pbo = pbo[0];
    buf.fields[f][1].id = buf.fields[f][0].id;
    buf.fields[f][2].id = buf.fields[f][1].id;
  }

  // YUV
  for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
  {
    int fieldshift = (f==FIELD_FULL) ? 0 : 1;
    CYuvPlane (&planes)[YuvImage::MAX_PLANES] = buf.fields[f];

    planes[0].texwidth  = im.width / 2;
    planes[0].texheight = im.height >> fieldshift;
    planes[1].texwidth  = planes[0].texwidth;
    planes[1].texheight = planes[0].texheight;
    planes[2].texwidth  = planes[1].texwidth;
    planes[2].texheight = planes[1].texheight;

    for (int p = 0; p < 3; p++)
    {
      planes[p].pixpertex_x = 2;
      planes[p].pixpertex_y = 1;
    }

    CYuvPlane &plane = planes[0];
    if (plane.texwidth * plane.texheight == 0)
      continue;

    glBindTexture(m_textureTarget, plane.id);

    glTexImage2D(m_textureTarget, 0, GL_RGBA, plane.texwidth, plane.texheight, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    VerifyGLState();
  }

  return true;
}

void CLinuxRendererGL::SetTextureFilter(GLenum method)
{
  for (int i = 0; i < m_NumYUVBuffers; i++)
  {
    CPictureBuffer& buf = m_buffers[i];

    for (int f = FIELD_FULL; f<=FIELD_BOT ; f++)
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

bool CLinuxRendererGL::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_NONLINSTRETCH ||
      feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_POSTPROCESS ||
      feature == RENDERFEATURE_ROTATION ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_TONEMAP)
    return true;

  return false;
}

bool CLinuxRendererGL::SupportsMultiPassRendering()
{
  return CGLExtensions::IsExtensionSupported(CGLExtensions::EXT_framebuffer_object);
}

bool CLinuxRendererGL::Supports(ESCALINGMETHOD method) const
{
  //nearest neighbor doesn't work on YUY2 and UYVY
  if (method == VS_SCALINGMETHOD_NEAREST &&
      m_format != AV_PIX_FMT_YUYV422 &&
      m_format != AV_PIX_FMT_UYVY422)
    return true;

  if (method == VS_SCALINGMETHOD_LINEAR ||
      method == VS_SCALINGMETHOD_AUTO)
    return true;

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
    // Empty until ManageRenderArea has run, then the per-eye crop of a stereo source
    CRect source =
        m_sourceRect.IsEmpty() ? CRect(0, 0, m_sourceWidth, m_sourceHeight) : m_sourceRect;
    float scaleX = fabs((source.Width() - m_destRect.Width()) / source.Width()) * 100;
    float scaleY = fabs((source.Height() - m_destRect.Height()) / source.Height()) * 100;
    int minScale = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_HQSCALERS);
    if (scaleX < minScale && scaleY < minScale)
      return false;

    bool hasFramebuffer = false;
    unsigned int major, minor;
    m_renderSystem->GetRenderVersion(major, minor);
    if (major > 3 ||
        (major == 3 && minor >= 2))
      hasFramebuffer = true;
    if (CGLExtensions::IsExtensionSupported(CGLExtensions::EXT_framebuffer_object))
      hasFramebuffer = true;
    if (hasFramebuffer  && (m_renderMethod & RENDER_GLSL))
      return true;
  }

  return false;
}

void CLinuxRendererGL::BindPbo(CPictureBuffer& buff)
{
  bool pbo = false;
  for(int plane = 0; plane < YuvImage::MAX_PLANES; plane++)
  {
    if(!buff.pbo[plane] || buff.image.plane[plane] == (uint8_t*)PBO_OFFSET)
      continue;
    pbo = true;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buff.pbo[plane]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    buff.image.plane[plane] = (uint8_t*)PBO_OFFSET;
  }
  if (pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void CLinuxRendererGL::UnBindPbo(CPictureBuffer& buff)
{
  bool pbo = false;
  for(int plane = 0; plane < YuvImage::MAX_PLANES; plane++)
  {
    if(!buff.pbo[plane] || buff.image.plane[plane] != (uint8_t*)PBO_OFFSET)
      continue;
    pbo = true;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buff.pbo[plane]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, buff.image.planesize[plane] + PBO_OFFSET, NULL, GL_STREAM_DRAW);
    buff.image.plane[plane] = (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY) + PBO_OFFSET;
  }
  if (pbo)
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

CRenderInfo CLinuxRendererGL::GetRenderInfo()
{
  CRenderInfo info;
#ifdef CB1_HAS_HDR10_AI
  info.max_buffer_size = m_softwareAIBuffers;
#else
  info.max_buffer_size = NUM_BUFFERS;
#endif
  return info;
}

// Color management helpers

bool CLinuxRendererGL::LoadCLUT()
{
  DeleteCLUT();

  int clutSize, dataSize;
  if (!CColorManager::Get3dLutSize(CMS_DATA_FMT_RGB, &clutSize, &dataSize))
    return false;

  // allocate buffer
  m_CLUTsize = clutSize;
  m_CLUT = static_cast<uint16_t*>(malloc(dataSize));

  // load 3DLUT
  if (!m_ColorManager->GetVideo3dLut(m_srcPrimaries, &m_cmsToken, CMS_DATA_FMT_RGB, m_CLUTsize,
                                     m_CLUT))
  {
    free(m_CLUT);
    CLog::Log(LOGERROR, "Error loading the LUT");
    return false;
  }

  // create 3DLUT texture
  CLog::Log(LOGDEBUG, "LinuxRendererGL: creating 3DLUT");
  glGenTextures(1, &m_tCLUTTex);
  glActiveTexture(GL_TEXTURE4);
  if (m_tCLUTTex <= 0)
  {
    CLog::Log(LOGERROR, "Error creating 3DLUT texture");
    return false;
  }

  // bind and set 3DLUT texture parameters
  glBindTexture(GL_TEXTURE_3D, m_tCLUTTex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

  // load 3DLUT data
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16, m_CLUTsize, m_CLUTsize, m_CLUTsize, 0, GL_RGB,
               GL_UNSIGNED_SHORT, m_CLUT);
  free(m_CLUT);
  glActiveTexture(GL_TEXTURE0);
  return true;
}

void CLinuxRendererGL::DeleteCLUT()
{
  if (m_tCLUTTex)
  {
    CLog::Log(LOGDEBUG, "LinuxRendererGL: deleting 3DLUT");
    glDeleteTextures(1, &m_tCLUTTex);
    m_tCLUTTex = 0;
  }
}

void CLinuxRendererGL::SetBufferColorimetry(const CPictureBuffer& buf, bool toneMap)
{
  VideoPicture tmp{};
  tmp.color_space = toneMap ? AVCOL_SPC_BT709 : buf.m_srcColSpace;
  tmp.color_transfer = toneMap ? AVCOL_TRC_BT709 : buf.m_srcColTransfer;
  tmp.color_primaries = toneMap ? AVCOL_PRI_BT709 : buf.m_srcPrimaries;
  tmp.iWidth = m_sourceWidth;
  tmp.iHeight = m_sourceHeight;
  tmp.colorBits = buf.m_srcBits;
  CServiceBroker::GetWinSystem()->SetColorimetry(&tmp, !m_cmsOn);
}

void CLinuxRendererGL::CheckVideoParameters(int index)
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

  if (!m_passthroughHDR && streamIsHDRPQ && toneMapMethod != VS_TONEMAPMETHOD_OFF)
  {
    toneMap = true;
  }

  if (toneMap != m_toneMap || toneMapMethod != m_toneMapMethod)
  {
    m_reloadShaders = true;

    //! @todo HACK for #28468; clean up properly in Piers beta 2
    if (toneMap != m_toneMap)
      SetBufferColorimetry(buf, toneMap);

    m_toneMap = toneMap;
    m_toneMapMethod = toneMapMethod;
  }
}
