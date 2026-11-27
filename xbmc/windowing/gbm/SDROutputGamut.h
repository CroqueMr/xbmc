/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace KODI::WINDOWING::GBM
{
enum class SDROutputGamut
{
  Auto = 0,
  BT2020 = 1,
  BT709 = 2,
};

constexpr bool WantsSDRBT2020(SDROutputGamut preference, bool nativeBT2020)
{
  return preference == SDROutputGamut::BT2020 ||
         (preference == SDROutputGamut::Auto && nativeBT2020);
}
inline constexpr const char* SDR_BT2020_DEEP_COLOR = "CB1_SDR_BT2020_DEEP_COLOR";

// Auto can raise a low ceiling only when the driver can validate the actual
// HDMI stream. Legacy drivers keep their existing explicit-depth behavior.
constexpr std::optional<uint64_t> SelectSDRBT2020MaxBpc(
    uint64_t ceiling, bool automatic, bool strict,
    std::optional<std::pair<uint64_t, uint64_t>> limits)
{
  if (!strict)
    return ceiling != 0 && ceiling < 10 ? std::nullopt : std::optional{ceiling};
  if (!ceiling || !limits || limits->second < 10)
    return {};
  if (automatic)
    ceiling = std::max<uint64_t>(10, ceiling);
  if (ceiling < 10 || (!automatic && ceiling < limits->first))
    return {};
  return std::clamp(ceiling, limits->first, limits->second);
}

constexpr std::pair<float, float> SDROutputRange(bool limited, int bits)
{
  if (!limited)
    return {1.0f, 0.0f};
  const int depth = std::clamp(bits, 8, 16);
  const float unit = static_cast<float>(1u << (depth - 8)) / ((1u << depth) - 1);
  return {219.0f * unit, 16.0f * unit};
}
} // namespace KODI::WINDOWING::GBM
