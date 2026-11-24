/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <algorithm>
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
constexpr std::pair<float, float> SDROutputRange(bool limited, int bits)
{
  if (!limited)
    return {1.0f, 0.0f};
  const int depth = std::clamp(bits, 8, 16);
  const float unit = static_cast<float>(1u << (depth - 8)) / ((1u << depth) - 1);
  return {219.0f * unit, 16.0f * unit};
}
} // namespace KODI::WINDOWING::GBM
