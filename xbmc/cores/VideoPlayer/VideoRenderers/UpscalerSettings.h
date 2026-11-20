/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cores/VideoSettings.h"

#include <string>

namespace KODI::VIDEO::UPSCALER
{
inline constexpr auto AT_LEAST_1080P = "videoplayer.upscaler1080pandabove";
inline constexpr auto BELOW_1080P = "videoplayer.upscalerbelow1080p";
inline constexpr int KODI_DEFAULT = -1;

constexpr int Select(int method, int kodi)
{
  return method == -1 ? kodi : method;
}

constexpr bool Enlarges(float width, float height, float targetWidth, float targetHeight)
{
  return width > 0 && height > 0 && targetWidth > 0 && targetHeight > 0 &&
         (targetWidth - width >= 0.5f || targetHeight - height >= 0.5f);
}

constexpr bool IsAtLeast1080p(unsigned int width, unsigned int height)
{
  return width >= 1920 || height >= 1080;
}

int Get(unsigned int sourceWidth, unsigned int sourceHeight, int kodi);
constexpr bool UsesFSR1(int method)
{
  return method == VS_SCALINGMETHOD_FSR1 || method == VS_SCALINGMETHOD_FSR1_RCAS;
}

constexpr bool UsesShaderHooks(int method)
{
  return method == VS_SCALINGMETHOD_FSRCNNX || method == VS_SCALINGMETHOD_ANIME4K;
}

constexpr int Label(int method)
{
  switch (method)
  {
    case -1: return 41301; // Kodi default
    case VS_SCALINGMETHOD_NEAREST: return 16301;  // Nearest neighbour
    case VS_SCALINGMETHOD_LINEAR: return 16302;  // Bilinear
    case VS_SCALINGMETHOD_CUBIC_B_SPLINE: return 16303;  // Bicubic B-Spline
    case VS_SCALINGMETHOD_CUBIC_MITCHELL: return 16314;  // Bicubic Mitchell
    case VS_SCALINGMETHOD_CUBIC_CATMULL: return 16321;  // Bicubic Catmull-Rom
    case VS_SCALINGMETHOD_CUBIC_0_075: return 16326;  // Bicubic (0, 0.75)
    case VS_SCALINGMETHOD_CUBIC_0_1: return 16330;  // Bicubic (0, 1)
    case VS_SCALINGMETHOD_LANCZOS2: return 16304;  // Lanczos2
    case VS_SCALINGMETHOD_LANCZOS3_FAST: return 16315;  // Lanczos3 Fast
    case VS_SCALINGMETHOD_LANCZOS3: return 16305;  // Lanczos3
    case VS_SCALINGMETHOD_AUTO: return 16316; // Auto
    case VS_SCALINGMETHOD_SPLINE36_FAST: return 16323; // Spline36 Fast
    case VS_SCALINGMETHOD_SPLINE36: return 16322; // Spline36
    case VS_SCALINGMETHOD_FSR1: return 41306;
    case VS_SCALINGMETHOD_FSR1_RCAS: return 41309;
    case VS_SCALINGMETHOD_FSRCNNX: return 41307;
    case VS_SCALINGMETHOD_ANIME4K: return 41308;
    default: return 0;
  }
}
} // namespace KODI::VIDEO::UPSCALER
