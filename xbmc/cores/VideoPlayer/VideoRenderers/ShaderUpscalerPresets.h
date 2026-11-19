/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "UpscalerSettings.h"
#include "filesystem/File.h"

extern "C"
{
#include <libplacebo/shaders/custom.h>
}

#include <array>
#include <iterator>
#include <span>
#include <vector>

namespace KODI::VIDEO::UPSCALER
{
inline void DestroyHooks(std::vector<const pl_hook*>& hooks)
{
  for (auto* hook : hooks)
    pl_mpv_user_shader_destroy(&hook);
  hooks.clear();
}

inline bool LoadHooks(pl_gpu gpu, int method, std::vector<const pl_hook*>& hooks, bool master = false)
{
  constexpr std::array fsr{"FSRCNNX_x2_8-0-4-1.glsl"};
  constexpr std::array anime{"Anime4K_Clamp_Highlights.glsl", "Anime4K_Restore_CNN_M.glsl",
      "Anime4K_Upscale_CNN_x2_M.glsl", "Anime4K_AutoDownscalePre_x2.glsl",
      "Anime4K_AutoDownscalePre_x4.glsl", "Anime4K_Upscale_CNN_x2_S.glsl"};
  const auto files = method == VS_SCALINGMETHOD_FSRCNNX ? std::span<const char* const>{fsr}
                                                       : std::span<const char* const>{anime};
  for (const std::string_view name : files)
  {
    XFILE::CFileStream file;
    if (!file.Open("special://xbmc/system/shaders/cb1/" + std::string{name}))
      return false;
    std::string source{std::istreambuf_iterator<char>(file), {}};
    if (master && (method == VS_SCALINGMETHOD_FSRCNNX || name == anime[2]))
    {
      // The final fit already established that this is an enlargement. The model works at
      // 2x internally; libplacebo resamples its result to the requested rectangle.
      constexpr std::string_view when = "//!WHEN";
      for (auto pos = source.find(when); pos != std::string::npos;
           pos = source.find(when, pos + when.size()))
        source.replace(pos + when.size(), source.find('\n', pos) - pos - when.size(), " 1");
    }
    if (master && name == anime[0])
    {
      constexpr std::string_view bt601 = "0.299, 0.587, 0.114";
      constexpr std::string_view bt2020 = "0.2627, 0.6780, 0.0593";
      for (auto pos = source.find(bt601); pos != std::string::npos;
           pos = source.find(bt601, pos))
        source.replace(pos, bt601.size(), bt2020);
    }
    const pl_hook* hook = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
    if (!hook)
      return false;
    hooks.push_back(hook);
  }
  return true;
}
} // namespace KODI::VIDEO::UPSCALER
