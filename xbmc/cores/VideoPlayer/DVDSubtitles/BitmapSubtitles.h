/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Bitmap positioning and linear-light saturation/PQ math adapted from
 * avdvplus/xbmc e0614d7a6713369bbcfe42152223f1cdaec3dbf1:
 * OverlayRenderer.cpp and GLES/2.0/gles_shader_texture_noblend.frag.
 * Copyright (C) 2005-2018 Team Kodi; (C) 2010-2013 Team XBMC.
 * Keep this native subtitle-only: never apply to menu/GUI or movie pixels.
 */
#pragma once

#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlay.h"
#include "utils/Geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace KODI::SUBTITLES
{
struct BitmapOptions
{
  // Authoring is latched by the decoder when opening the subtitle stream.
  // 0: native legacy interpretation, 1: SDR/BT.709, 2: PQ/BT.2020.
  int authoring{0};
  // 0: original, 1/2: bottom/top video, 3/4: bottom/top screen, 5: manual.
  int placement{0};
  int offset{0};
  int zoom{100};
  int sdrBrightness{100};
  int sdrSaturation{100};
  int hdrBrightness{100};
  int hdrSaturation{100};
  bool operator==(const BitmapOptions&) const = default;

  bool HasGeometry() const { return placement != 0 || offset != 0 || zoom != 100; }
};

inline CRect BitmapRect(const CRect& rect, const CRect& block, const CRect& video,
                        const CRect& screen, const BitmapOptions& options, DVDOverlaySource source)
{
  if (source != DVDOverlaySource::SUBTITLE || !options.HasGeometry() ||
      block.IsEmpty() || screen.IsEmpty())
    return rect;
  float zoom = std::clamp(options.zoom, 10, 200) / 100.0f;
  zoom = std::min({zoom, screen.Width() / block.Width(), screen.Height() / block.Height()});
  const float center = (block.x1 + block.x2) * 0.5f;
  const float height = block.Height() * zoom;
  float top = block.y2 - height;
  const float margin = 8.0f * screen.Height() / 1080.0f;
  switch (options.placement)
  {
    case 1: top = video.y2 - margin - height; break;
    case 2: top = video.y1 + margin; break;
    case 3: top = screen.y2 - margin - height; break;
    case 4: top = screen.y1 + margin; break;
    default: break;
  }
  top -= std::clamp(options.offset, -60, 60) * (screen.Height() * 18.75f / 1080.0f);
  top = std::clamp(top, screen.y1, screen.y2 - height);
  const float left = std::clamp(center - block.Width() * zoom * 0.5f,
                                screen.x1, screen.x2 - block.Width() * zoom);
  return CRect(left + (rect.x1 - block.x1) * zoom, top + (rect.y1 - block.y1) * zoom,
               left + (rect.x2 - block.x1) * zoom, top + (rect.y2 - block.y1) * zoom);
}

inline float BitmapDecodePQ(float value)
{
  constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
  const float p = std::pow(std::clamp(value, 0.0f, 1.0f), 1.0f / m2);
  return std::pow(std::max(p - c1, 0.0f) / std::max(c2 - c3 * p, 1e-6f), 1.0f / m1);
}

inline float BitmapEncodePQ(float value)
{
  if (value <= 0.0f) return 0.0f;
  constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
  const float p = std::pow(value, m1);
  return std::clamp(std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2), 0.0f, 1.0f);
}

// Input and output are straight-alpha packed 0xAARRGGBB. Call before premultiplication.
inline void AdjustBitmapPalette(std::vector<uint32_t>& palette, bool pq, const BitmapOptions& options,
                                 DVDOverlaySource source)
{
  if (source != DVDOverlaySource::SUBTITLE)
    return;
  const int brightness = pq ? options.hdrBrightness : options.sdrBrightness;
  const int saturation = pq ? options.hdrSaturation : options.sdrSaturation;
  if (brightness == 100 && saturation == 100)
    return; // Strict byte identity: no PQ round-trip, quantization, clamp or dither.
  const float gain = std::clamp(brightness, 0, 200) / 100.0f;
  const float sat = std::clamp(saturation, 0, 200) / 100.0f;
  for (uint32_t& pixel : palette)
  {
    float rgb[3];
    for (int i = 0; i < 3; ++i)
    {
      const float code = ((pixel >> (16 - 8 * i)) & 255) / 255.0f;
      rgb[i] = pq ? BitmapDecodePQ(code) : std::pow(code, 1.0f / 0.45f);
    }
    const float luma = pq ? rgb[0] * 0.2627f + rgb[1] * 0.6780f + rgb[2] * 0.0593f
                          : rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
    uint32_t result = pixel & 0xff000000u;
    for (int i = 0; i < 3; ++i)
    {
      const float linear = std::max(0.0f, luma + (rgb[i] - luma) * sat) * gain;
      const float code = pq ? BitmapEncodePQ(linear) : std::pow(std::min(linear, 1.0f), 0.45f);
      result |= static_cast<uint32_t>(code * 255.0f + 0.5f) << (16 - 8 * i);
    }
    pixel = result;
  }
}
} // namespace KODI::SUBTITLES
