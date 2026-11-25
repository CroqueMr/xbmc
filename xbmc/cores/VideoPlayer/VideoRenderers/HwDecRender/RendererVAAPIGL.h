/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VaapiEGL.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"
#ifdef HAVE_DVBRIDGE
#include "cores/VideoPlayer/VideoRenderers/DVBridgeGLES.h"
#endif

#include <cstdint>
#include <memory>

namespace VAAPI
{
class IVaapiWinSystem;
}

class CRendererVAAPIGL : public CLinuxRendererGL
{
public:
  CRendererVAAPIGL();
  ~CRendererVAAPIGL() override;

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
  bool Flush(bool saveBuffers) override;
#ifdef HAVE_DVBRIDGE
  void SetDVBridgeFELSuccessor(int index) override;
#endif

  // Feature support
  bool Supports(ERENDERFEATURE feature) const override;
  bool Supports(ESCALINGMETHOD method) const override;

protected:
  bool LoadShadersHook() override;
  bool RenderHook(int idx) override;
#ifdef HAVE_DVBRIDGE
  bool RenderDVBridge(int idx) override;
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
  // VA fourcc of the surfaces being received, captured at Configure.
  std::int32_t m_vaapiFourcc{};
#ifdef CB1_HAS_HDR10_AI
  unsigned int m_srcColorBits{8};
  bool m_srcFullRange{false};
#endif
  VAAPI::CVaapiTexturePool m_texturePool;
  VAAPI::CVaapiTexture* m_vaapiTextures[MAX_RENDER_BUFFERS]{};
  GLsync m_fences[MAX_RENDER_BUFFERS]{};
  static VAAPI::IVaapiWinSystem *m_pWinSystem;
};
