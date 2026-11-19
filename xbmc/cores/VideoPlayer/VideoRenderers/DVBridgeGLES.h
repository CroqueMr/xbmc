/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cores/VideoPlayer/DVDCodecs/Video/DVMetadataBuffer.h"
#include "utils/DVBridgeDiagnosticState.h"

extern "C"
{
#include "dvbridge_gl_frame.h"
#include "dvbridge_gl_pack.h"
#include "dvbridge_render.h"
#include "dvbridge_dmabuf.h"
}

class CDVBridgeGLES
{
public:
  struct Layer
  {
    dvbridge_gl_layout layout;
    int width;
    int height;
    dvbridge_gl_plane planes[3];
    double pts;
  };

  CDVBridgeGLES() = default;
  ~CDVBridgeGLES();
  CDVBridgeGLES(const CDVBridgeGLES&) = delete;
  CDVBridgeGLES& operator=(const CDVBridgeGLES&) = delete;

  bool Initialize(bool allowSoftware = false);
  void Reset();
  bool Prepare(const Layer& base, const Layer* enhancement,
               const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked = false);
  bool PrepareHardware(const Layer& base, const AVFrame* enhancement,
                       const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked = false);
  unsigned int GetPQTexture() const;
  unsigned int GetPQFramebuffer();
  bool Pack(unsigned int framebuffer, int width, int height, bool flipY);
  bool Presented(bool success);

private:
  bool Failure(const char* stage);
  void Summary(const char* event);
  DVBRIDGE::DiagnosticFailures m_failures;
  uint64_t m_prepares{0}, m_directFrames{0}, m_presentations{0}, m_failedPresentations{0};
  uint64_t m_lastSummary{0}, m_lastReported{0};
  bool m_firstFrame{true};
  // All methods, including destruction, require the owning GL context/thread.
  pl_opengl m_gl = nullptr;
  pl_log m_log = nullptr;
  dvbridge_renderer* m_renderer = nullptr;
  dvbridge_gl_packer* m_packer = nullptr;
  pl_tex m_scanout = nullptr;
  bool m_direct = false;
  bool m_directUsable = true;
  bool m_prepared = false;
  bool m_packed = false;
  unsigned m_pendingInfo = 0;
  unsigned int m_framebuffer = 0;
};
