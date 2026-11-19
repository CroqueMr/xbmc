/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <array>
#include <memory>

struct gbm_bo;
struct gbm_device;

namespace KODI::WINDOWING::GBM
{
class CDRMAtomic;

// Keep the render target stable while alternating two scanout buffers.
class CPascalHDR10Scanout
{
public:
  static std::unique_ptr<CPascalHDR10Scanout> Create(
      gbm_device* device, CDRMAtomic& drm, int width, int height);
  ~CPascalHDR10Scanout();

  unsigned int Framebuffer() const { return m_source; }
  std::shared_ptr<gbm_bo> Blit();
  void Presented() { m_next = 1 - m_next; }

private:
  struct Buffer
  {
    std::shared_ptr<gbm_bo> bo;
    unsigned int texture{};
    unsigned int framebuffer{};
  };

  bool Init(gbm_device* device, CDRMAtomic& drm);
  bool Probe() const;
  std::array<Buffer, 2> m_buffers;
  unsigned int m_source{};
  unsigned int m_texture{};
  unsigned int m_next{};
  int m_width{};
  int m_height{};
};
}
