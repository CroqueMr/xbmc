/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <algorithm>
#include <optional>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace DVBRIDGE
{
inline bool ContinuousFastRenderingPicture(double currentUs, double previousUs)
{
  // Kodi picture timestamps use microseconds; seeks and repeats are excluded.
  return std::isfinite(currentUs) && std::isfinite(previousUs) &&
      currentUs > previousUs && currentUs - previousUs < 250000;
}

inline std::optional<int> LegacyFastRendering(std::string_view value)
{
  std::string text(value);
  std::transform(text.begin(), text.end(), text.begin(),
      [](unsigned char c) { return std::tolower(c); });
  if (text == "false") return 0;
  if (text == "true") return 1;
  return std::nullopt;
}

inline std::optional<int64_t> PresentationHeadroom(uint64_t previousNs, uint64_t readyNs,
    uint64_t submittedNs, uint64_t presentedNs, uint64_t periodNs)
{
  if (!previousNs || !readyNs || !submittedNs || periodNs < 1000000 || periodNs > 100000000 ||
      presentedNs <= previousNs || readyNs > presentedNs || submittedNs > presentedNs ||
      presentedNs - previousNs > 4 * periodNs)
    return std::nullopt;
  // A delayed actual presentation must not move the missed deadline forward.
  return static_cast<int64_t>(previousNs) + static_cast<int64_t>(periodNs) -
      static_cast<int64_t>(std::max(readyNs, submittedNs));
}

}
