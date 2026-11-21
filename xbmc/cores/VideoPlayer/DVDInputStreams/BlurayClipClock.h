/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

namespace KODI::BLURAY
{
inline int64_t MpegTimeDelta(int64_t timestamp, int64_t in)
{
  constexpr uint64_t wrap = UINT64_C(1) << 33;
  const uint64_t delta = (static_cast<uint64_t>(timestamp) - static_cast<uint64_t>(in)) & (wrap - 1);
  return delta < wrap / 2 ? static_cast<int64_t>(delta)
                          : static_cast<int64_t>(delta) - static_cast<int64_t>(wrap);
}

inline int64_t PresentationTime(int64_t timestamp, int64_t in, int64_t anchor)
{
  return anchor + MpegTimeDelta(timestamp, in);
}

inline bool IsSequentialClipRead(uint64_t previousEnd, uint64_t position, uint64_t itemStart)
{
  return itemStart != 0 && previousEnd == itemStart && position == itemStart;
}
} // namespace KODI::BLURAY
