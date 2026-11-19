/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include <atomic>

namespace DVBRIDGE
{
enum class OutputMode : int { Standard = 0, HDR10 = 1, Disabled = 2 };

// Playback preparation latches the preferences. An open settings dialog must not
// change the output encoding of an already running playback session.
inline std::atomic<OutputMode> playbackMode{OutputMode::Standard};

inline void SelectOutputSettings()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  const int mode = settings ? settings->GetInt("dvbridge.outputmode") : 0;
  playbackMode.store(mode >= 0 && mode <= 2 ? static_cast<OutputMode>(mode) : OutputMode::Standard);
}

inline bool EnhancedInfoEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.enhancedinfo");
}
} // namespace DVBRIDGE
