/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererVAAPIGL.h"

#include "../RenderFactory.h"
#include "cores/VideoPlayer/DVDCodecs/Video/VAAPI.h"
#include "utils/GLUtils.h"
#include "utils/log.h"
#ifdef HAVE_DVBRIDGE
#include "ServiceBroker.h"
#include "cores/VideoSourceSideData.h"
#include "cores/VideoPlayer/DVDCodecs/Video/HDR10SourceType.h"
#include "cores/VideoPlayer/DVDCodecs/Video/SDRSourceType.h"
#include "windowing/gbm/WinSystemGbmDVContext.h"
#include <cmath>
#include <algorithm>
#endif

#include <memory>

using namespace VAAPI;

IVaapiWinSystem* CRendererVAAPIGL::m_pWinSystem = nullptr;

CBaseRenderer* CRendererVAAPIGL::Create(CVideoBuffer* buffer)
{
  CVaapiRenderPicture *vb = dynamic_cast<CVaapiRenderPicture*>(buffer);
  if (vb)
    return new CRendererVAAPIGL();

  return nullptr;
}

void CRendererVAAPIGL::Register(IVaapiWinSystem* winSystem,
                                VADisplay vaDpy,
                                EGLDisplay eglDisplay,
                                bool& general,
                                bool& deepColor)
{
  general = deepColor = false;

  int major_version, minor_version;
  if (vaInitialize(vaDpy, &major_version, &minor_version) != VA_STATUS_SUCCESS)
  {
    vaTerminate(vaDpy);
    return;
  }

  // Probe importable surface formats via vaExportSurfaceHandle.
  CCapabilities& caps = CDecoder::GetCaps();
  CVaapi2Texture::TestInteropFormats(vaDpy, eglDisplay, caps);

  CLog::Log(LOGDEBUG, "VAAPI EGL interop: {}", caps.ToString());

  // Bool out-params are views over caps for the OptionalsReg / WinSystem
  // boundary that still expresses capability as the general/deepColor pair.
  general = caps.Supports(AV_PIX_FMT_NV12);
  deepColor = caps.Supports(AV_PIX_FMT_P010);

  vaTerminate(vaDpy);

  if (general)
  {
    VIDEOPLAYER::CRendererFactory::RegisterRenderer("vaapi", CRendererVAAPIGL::Create);
    m_pWinSystem = winSystem;
  }
}

CRendererVAAPIGL::CRendererVAAPIGL() = default;

CRendererVAAPIGL::~CRendererVAAPIGL()
{
  for (int i = 0; i < MAX_RENDER_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }
  // renderer destruction runs on the render thread with the GL context current
  m_texturePool.ReleaseAll();
}

bool CRendererVAAPIGL::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  return Configure(picture, fps, orientation, RetainedOutput::None);
}

bool CRendererVAAPIGL::Configure(const VideoPicture& picture, float fps,
                                 unsigned int orientation, RetainedOutput retainedOutput)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
  m_isVAAPIBuffer = pic->procPic.videoSurface != VA_INVALID_ID;
  m_vaapiFourcc = pic->procPic.fourcc;
#ifdef CB1_HAS_HDR10_AI
  m_aiBuffers = m_isVAAPIBuffer && DVBRIDGE::IsHDR10Family(picture.hdrType) &&
      !picture.doviSourceInfo.profile && !(picture.iFlags & DVP_FLAG_INTERLACED) &&
      DVBridgeOrientationSupported(orientation) ? pic->renderBufferCount : NUM_BUFFERS;
  m_aiLookahead=m_aiBuffers>NUM_BUFFERS;
  SetFps(fps);
  m_srcColorBits = picture.colorBits;
  m_srcFullRange = picture.color_range == 1;
#endif

  if (m_isVAAPIBuffer)
  {

    InteropInfo interop;
    interop.textureTarget = GL_TEXTURE_2D;
    interop.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    interop.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    interop.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    interop.eglDisplay = CRendererVAAPIGL::m_pWinSystem->GetEGLDisplay();

    m_texturePool.ReleaseAll();
    size_t importedSurfaces = VAAPI::CVaapiTexturePool::MAX_ENTRIES;
#ifdef CB1_HAS_HDR10_AI
    importedSurfaces += m_aiBuffers > NUM_BUFFERS ? m_aiBuffers - NUM_BUFFERS : 0;
#endif
#ifdef HAVE_DVBRIDGE
    // Pairing retains decoded surfaces, not additional rendering slots.
    if (picture.doviSourceInfo.profile)
      importedSurfaces = std::max(importedSurfaces, size_t(pic->decodeSurfaceCount));
#endif
    m_texturePool.Init(interop, importedSurfaces);
    for (auto &tex : m_vaapiTextures)
      tex = nullptr;
  }

  for (auto &fence : m_fences)
  {
    fence = {};
  }

  return CLinuxRendererGL::Configure(picture, fps, orientation, retainedOutput);
}

bool CRendererVAAPIGL::Flush(bool saveBuffers)
{
  for (auto &vaapiTexture : m_vaapiTextures)
  {
    if (m_isVAAPIBuffer)
      vaapiTexture = nullptr;
  }
  return CLinuxRendererGL::Flush(saveBuffers);
}

#ifdef CB1_HAS_HDR10_AI
CRenderInfo CRendererVAAPIGL::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_aiBuffers;
  return info;
}

CRendererVAAPIGL::DVHandoff CRendererVAAPIGL::RetainDVOutput(
    const VideoPicture& picture, float fps, unsigned int orientation)
{
  const auto* next = dynamic_cast<const CVaapiRenderPicture*>(picture.videoBuffer);
  if (!m_isVAAPIBuffer || !next || !next->valid ||
      next->procPic.videoSurface == VA_INVALID_ID || next->procPic.fourcc != m_vaapiFourcc ||
      picture.iWidth != m_sourceWidth || picture.iHeight != m_sourceHeight ||
      picture.colorBits != m_srcColorBits || !DVBridgeOrientationSupported(orientation) ||
      (picture.iFlags & DVP_FLAG_INTERLACED) || std::abs(fps - m_fps) > 0.001f)
    return DVHandoff::None;

  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
      CServiceBroker::GetWinSystem());
  if (!window)
    return DVHandoff::None;

  if (!m_dvRequested)
  {
    if (!m_bConfigured || !m_passthroughHDR ||
        m_srcFullRange != (picture.color_range == 1) ||
        m_srcPrimaries != picture.color_primaries || !window->CanRetainHDR10Output(picture))
      return DVHandoff::None;

    m_bConfigured = false;
    return DVHandoff::HDR10;
  }

  const auto handoff = window->RetainNativeDVOutput(picture) ? DVHandoff::Native :
      m_aiLookahead && next->renderBufferCount > NUM_BUFFERS && window->RetainHDR10DVOutput(picture)
          ? DVHandoff::HDR10AI : DVHandoff::None;
  if (handoff != DVHandoff::None)
    m_dvRequested = m_bConfigured = false; // The successor owns output cleanup.
  return handoff;
}

bool CRendererVAAPIGL::ConfigureWithHandoff(const VideoPicture& picture,
                                            float fps,
                                            unsigned int orientation,
                                            DVHandoff handoff)
{
  return Configure(picture, fps, orientation,
                   handoff == DVHandoff::HDR10 ? RetainedOutput::HDR10 : RetainedOutput::NativeDV);
}

bool CRendererVAAPIGL::HDR10AILookahead() const
{
  return m_aiBuffers > NUM_BUFFERS && m_aiLookahead;
}

#endif

bool CRendererVAAPIGL::ConfigChanged(const VideoPicture& picture)
{
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(picture.videoBuffer);
#ifdef CB1_HAS_HDR10_AI
  const unsigned buffers = pic->procPic.videoSurface != VA_INVALID_ID &&
      DVBRIDGE::IsHDR10Family(picture.hdrType) &&
      !picture.doviSourceInfo.profile &&
      !(picture.iFlags & DVP_FLAG_INTERLACED) && DVBridgeOrientationSupported(m_renderOrientation) ?
      pic->renderBufferCount : NUM_BUFFERS;
  if (m_aiBuffers != buffers)
    return true;
#endif
  if ((pic->procPic.videoSurface != VA_INVALID_ID && !m_isVAAPIBuffer) ||
      (pic->procPic.videoSurface == VA_INVALID_ID && m_isVAAPIBuffer))
    return true;

  return false;
}

bool CRendererVAAPIGL::Supports(ERENDERFEATURE feature) const
{
  return CLinuxRendererGL::Supports(feature);
}

bool CRendererVAAPIGL::Supports(ESCALINGMETHOD method) const
{
  return CLinuxRendererGL::Supports(method);
}

EShaderFormat CRendererVAAPIGL::GetShaderFormat()
{
  return SHADER_NV12;
}

bool CRendererVAAPIGL::LoadShadersHook()
{
  return false;
}

bool CRendererVAAPIGL::RenderHook(int idx)
{
  return false;
}

#ifdef HAVE_DVBRIDGE
bool CRendererVAAPIGL::HasShaderUpscalerP010(int index) const
{
  const auto* picture = dynamic_cast<const CVaapiRenderPicture*>(m_buffers[index].videoBuffer);
  return m_isVAAPIBuffer && m_vaapiTextures[index] && picture && picture->valid &&
         picture->procPic.videoSurface != VA_INVALID_ID && picture->procPic.fourcc == VA_FOURCC_P010;
}

bool CRendererVAAPIGL::DVBridgeOrientationSupported(unsigned int orientation) const
{
  // Nonzero native presentation rotation uses the established unsupported path.
  return orientation == 0;
}

bool CRendererVAAPIGL::BuildDVBridgeLayer(int idx, CDVBridgeGLES::Layer& layer)
{
  // CB1 reads the full decoded texture, independent of Kodi's view-mode crop.
  if (!m_isVAAPIBuffer || m_textureTarget != GL_TEXTURE_2D ||
      !DVBridgeOrientationSupported(m_renderOrientation))
    return false;
  auto& buffer = m_buffers[idx];
  if (!buffer.doviSourcePicture)
    return false;
  const auto& source = *buffer.doviSourcePicture;
  bool hdr10 = DVBRIDGE::IsHDR10Family(source.hdrType) &&
      source.doviSourceBinding.Valid() && !source.doviSourceInfo.profile;
  const bool sdr = DVBRIDGE::IsSDRExpansionSource(source) && source.doviSourceBinding.Valid();
  const bool plain = hdr10 || sdr;
  if (!plain && (!buffer.doviMetadata.Data() || buffer.doviPts == DVD_NOPTS_VALUE))
    return false;
  if (hdr10 && source.hdrType == StreamHdrType::HDR_TYPE_HLG &&
      m_vaapiFourcc == VA_FOURCC_NV12 && !source.IsLimitedHLG8())
    return false;
  switch (m_vaapiFourcc)
  {
    case VA_FOURCC_NV12: layer.layout = DVBRIDGE_NV12; break;
    case VA_FOURCC_P010: layer.layout = DVBRIDGE_P010; break;
    case VA_FOURCC_P012: layer.layout = DVBRIDGE_P012; break;
    case VA_FOURCC_P016: layer.layout = DVBRIDGE_P016; break;
    default: return false;
  }
  layer.scalingMethod = SelectScalingMethod();
  layer.autoScalingMethod = AutoUpscalingOverride();
  layer.width = m_sourceWidth;
  layer.height = m_sourceHeight;
  // AI follows Kodi's repaired presentation timestamps; DV pairing keeps source timestamps.
  layer.pts = plain ? source.pts / DVD_TIME_BASE : buffer.doviPts;
  layer.binding = plain ? source.doviSourceBinding : buffer.doviMetadata.Binding();
  if (!std::isfinite(layer.pts) ||
      (plain && (source.pts == DVD_NOPTS_VALUE || source.doviSourcePts == AV_NOPTS_VALUE)))
    return false;
  const auto texture = m_texturePool.Retain(m_vaapiTextures[idx]);
  if (!texture || !buffer.doviSourcePicture) return false;
  layer.sourceInfo = buffer.doviSourcePicture->doviSourceInfo;
  layer.sourceSideData = source.sourceSideData;
  layer.sourceSerial = source.sourceSideData ? source.sourceSideData->Serial() : 0;
  layer.hdr10PlusSource = source.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS;
  layer.chromaLocation = source.chroma_position;
  layer.sdrInput = sdr;
  layer.sdrMatrix = source.color_space;
  layer.sdrPrimaries = source.color_primaries;
  layer.sdrTransfer = source.color_transfer;
  layer.hlg = source.hdrType == StreamHdrType::HDR_TYPE_HLG;
  if (plain)
  {
    layer.fullRange = source.color_range != 0;
    if (source.hasDisplayMetadata && source.displayMetadata.has_luminance)
    {
      layer.hdr.max_luma = av_q2d(source.displayMetadata.max_luminance);
      layer.hdr.min_luma = av_q2d(source.displayMetadata.min_luminance);
    }
    if (source.hasLightMetadata)
    {
      layer.hdr.max_cll = source.lightMetadata.MaxCLL;
      layer.hdr.max_fall = source.lightMetadata.MaxFALL;
    }
  }
  struct ForeignLease
  {
    std::shared_ptr<void> imported;
    std::shared_ptr<VideoPicture> decoded;
  };
  layer.lease = std::make_shared<ForeignLease>(texture,buffer.doviSourcePicture);
  for (int p = 0; p < 2; ++p)
  {
    const auto& plane = buffer.fields[FIELD_FULL][p];
    layer.planes[p] = {plane.id, static_cast<int>(plane.texwidth),
                      static_cast<int>(plane.texheight)};
  }
  return true;
}

void CRendererVAAPIGL::SetDVBridgeFELSuccessor(int index)
{
  m_felSuccessor = {};
  if (index < 0 || index >= MAX_RENDER_BUFFERS) return;
  const auto& buffer = m_buffers[index];
  if (!buffer.doviEnhancement || !buffer.doviMetadata.Data() ||
      !(buffer.doviEnhancementBinding == buffer.doviMetadata.Binding())) return;
  m_felSuccessor = {buffer.doviEnhancement,buffer.doviEnhancementBinding,buffer.doviMetadata};
}

bool CRendererVAAPIGL::RenderDVBridge(int idx)
{
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
      CServiceBroker::GetWinSystem());
  CDVBridgeGLES::Layer layer{};
  if (!window || !BuildDVBridgeLayer(idx,layer)) return false;
  auto& buffer = m_buffers[idx];
  return window->RenderDVBridge(layer, buffer.doviEnhancement,
      buffer.doviEnhancementBinding, buffer.doviMetadata, DVBridgeGeometry(), &m_felSuccessor);
}
#endif

bool CRendererVAAPIGL::CreateTexture(int index)
{
  if (!m_isVAAPIBuffer)
  {
    return CreateNV12Texture(index);
  }

  CPictureBuffer &buf = m_buffers[index];
  YuvImage &im = buf.image;
  CYuvPlane (&planes)[YuvImage::MAX_PLANES] = buf.fields[0];

  DeleteTexture(index);

  memset(&im, 0, sizeof(im));
  memset(&planes, 0, sizeof(CYuvPlane[YuvImage::MAX_PLANES]));
  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;

  planes[0].id = 1;

  return true;
}

void CRendererVAAPIGL::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  if (!m_isVAAPIBuffer)
  {
    DeleteNV12Texture(index);
    return;
  }

  CPictureBuffer &buf = m_buffers[index];
  buf.fields[FIELD_FULL][0].id = 0;
  buf.fields[FIELD_FULL][1].id = 0;
  buf.fields[FIELD_FULL][2].id = 0;
}

bool CRendererVAAPIGL::UploadTexture(int index)
{
#ifdef HAVE_DVBRIDGE
  auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmDVContext*>(
      CServiceBroker::GetWinSystem());
  if (window) window->ObserveDVImport({}, {}, false);
#endif
  CPictureBuffer &buf = m_buffers[index];
  CVaapiRenderPicture *pic = dynamic_cast<CVaapiRenderPicture*>(buf.videoBuffer);

  if (!pic || !pic->valid)
  {
    return false;
  }

  if (!m_isVAAPIBuffer)
  {
    if (!m_buffers[index].loaded)
    {
      YuvImage &dst = m_buffers[index].image;
      YuvImage src;
      pic->GetPlanes(src.plane);
      pic->GetStrides(src.stride);
      UnBindPbo(m_buffers[index]);
      CVideoBuffer::CopyNV12Picture(&dst, &src);
      BindPbo(m_buffers[index]);
    }
    CalculateTextureSourceRects(index, 3);
    return UploadNV12Texture(index);
  }

  m_vaapiTextures[index] = m_texturePool.Get(pic, m_vaapiTextures);
  if (!m_vaapiTextures[index])
    return false;

  const YuvImage& im = buf.image;
  CYuvPlane (&planes)[3] = buf.fields[0];

  auto size = m_vaapiTextures[index]->GetTextureSize();
  planes[0].texwidth  = size.Width();
  planes[0].texheight = size.Height();

  planes[1].texwidth  = planes[0].texwidth  >> im.cshift_x;
  planes[1].texheight = planes[0].texheight >> im.cshift_y;
  planes[2].texwidth  = planes[1].texwidth;
  planes[2].texheight = planes[1].texheight;

  for (int p = 0; p < 3; p++)
  {
    planes[p].pixpertex_x = 1;
    planes[p].pixpertex_y = 1;
  }

  // set textures
  planes[0].id = m_vaapiTextures[index]->GetTextureY();
  planes[1].id = m_vaapiTextures[index]->GetTextureVU();
  planes[2].id = m_vaapiTextures[index]->GetTextureVU();

  for (int p=0; p<2; p++)
  {
    glBindTexture(m_textureTarget, planes[p].id);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(m_textureTarget, 0);
    VerifyGLState();
  }

  CalculateTextureSourceRects(index, 3);
#ifdef HAVE_DVBRIDGE
  if (window && buf.doviSourcePicture)
  {
    const bool layout = m_vaapiFourcc == VA_FOURCC_NV12 || m_vaapiFourcc == VA_FOURCC_P010 ||
        m_vaapiFourcc == VA_FOURCC_P012 || m_vaapiFourcc == VA_FOURCC_P016;
    const auto& picture = *buf.doviSourcePicture;
    const bool hlg8 = m_vaapiFourcc == VA_FOURCC_NV12 &&
        picture.hdrType == StreamHdrType::HDR_TYPE_HLG && !picture.doviSourceInfo.profile;
    const bool usable = layout && (!hlg8 || picture.IsLimitedHLG8()) &&
        m_textureTarget == GL_TEXTURE_2D && DVBridgeOrientationSupported(m_renderOrientation);
    const auto binding = buf.doviSourcePicture->doviSourceBinding.Valid() ?
        buf.doviSourcePicture->doviSourceBinding : buf.doviMetadata.Binding();
    window->ObserveDVImport(binding, DVBridgeGeometry(), usable);
  }
#endif
  return true;
}

void CRendererVAAPIGL::AfterRenderHook(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  m_fences[idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool CRendererVAAPIGL::NeedBuffer(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    GLint state;
    GLsizei length;
    glGetSynciv(m_fences[idx], GL_SYNC_STATUS, 1, &length, &state);
    if (state == GL_SIGNALED)
    {
      glDeleteSync(m_fences[idx]);
      m_fences[idx] = {};
    }
    else
    {
      return true;
    }
  }

  return false;
}

void CRendererVAAPIGL::ReleaseBuffer(int idx)
{
  if (glIsSync(m_fences[idx]))
  {
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  if (m_isVAAPIBuffer)
    m_vaapiTextures[idx] = nullptr;

  CLinuxRendererGL::ReleaseBuffer(idx);
}
