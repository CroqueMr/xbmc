/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VaapiEGL.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#endif

#include <array>
#include <memory>

namespace KODI
{
namespace UTILS
{
namespace EGL
{
class CEGLFence;
}
}
}

namespace VAAPI
{
class IVaapiWinSystem;
}

class CRendererVAAPIGLES : public CLinuxRendererGLES
{
public:
  CRendererVAAPIGLES();
  ~CRendererVAAPIGLES() override;

  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static void Register(VAAPI::IVaapiWinSystem *winSystem, VADisplay vaDpy, EGLDisplay eglDisplay, bool &general, bool &deepColor);

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation,
                 RetainedOutput retainedOutput);

  // Player functions
  bool ConfigChanged(const VideoPicture &picture) override;
#ifdef CB1_HAS_HDR10_AI
  CRenderInfo GetRenderInfo() override;
  enum class DVHandoff { None, HDR10AI, Native, HDR10 };
  DVHandoff RetainDVOutput(const VideoPicture& picture, float fps, unsigned int orientation);
  bool ConfigureWithHandoff(const VideoPicture& picture, float fps, unsigned int orientation,
                            DVHandoff handoff);
  bool HDR10AIQueueEligible() const override { return m_aiBuffers > NUM_BUFFERS; }
  bool HDR10AILookahead() const override;
  bool HDR10AITextureReady(int index) const override { return m_vaapiTextures[index] != nullptr; }
  unsigned m_aiBuffers{6};
#endif
  void ReleaseBuffer(int idx) override;
  bool NeedBuffer(int idx) override;
#ifdef HAVE_DVBRIDGE
  void SetDVBridgeFELSuccessor(int index) override;
#endif

protected:
  bool LoadShadersHook() override;
  bool RenderHook(int idx) override;
#ifdef HAVE_DVBRIDGE
  bool RenderDVBridge(int idx) override;
  bool HasShaderUpscalerP010(int index) const override;
  bool BuildDVBridgeLayer(int idx, CDVBridgeGLES::Layer& layer) override;
  CDVBridgeGLES::FELSuccessor m_felSuccessor;
  bool DVBridgeOrientationSupported(unsigned int orientation) const;
#endif
  void AfterRenderHook(int idx) override;

  // textures
  bool UploadTexture(int index) override;
  void DeleteTexture(int index) override;
  bool CreateTexture(int index) override;

  EShaderFormat GetShaderFormat() override;

  bool m_isVAAPIBuffer = true;
  bool m_nv12Allocated[MAX_RENDER_BUFFERS]{};
  // VA fourcc of the surfaces being received (NV12 / P010 / P012 / P016 /
  // YUY2 / Y210 / Y212 / Y216). Captured at Configure and read by
  // GetShaderFormat to pick the matching sampling path.
  std::int32_t m_vaapiFourcc{};
  VAAPI::CVaapiTexturePool m_texturePool;
  VAAPI::CVaapiTexture* m_vaapiTextures[MAX_RENDER_BUFFERS]{};
  std::array<std::unique_ptr<KODI::UTILS::EGL::CEGLFence>, MAX_RENDER_BUFFERS> m_fences;
  static VAAPI::IVaapiWinSystem *m_pWinSystem;
};
