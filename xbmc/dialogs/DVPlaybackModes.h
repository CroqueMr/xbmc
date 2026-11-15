/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef HAVE_DVBRIDGE
#include "settings/lib/ISettingCallback.h"
#include <string>
#include "settings/lib/ISettingsHandler.h"
enum class StreamHdrType;

class CDVPlaybackModes : public ISettingCallback, public ISettingsHandler
{
public:
  static CDVPlaybackModes& GetInstance();
  static bool MenuEnabled();
  static StreamHdrType SourceType();
  static bool ShowMenu(bool settingsMenu = false);
  static bool ConfigureProfile();
  static std::string DefaultPlaybackLabel();
  static std::string LldvPreferenceLabel();
  static std::string SDRGamutLabel();
  void OnSettingAction(const std::shared_ptr<const CSetting>& setting) override;
  void OnSettingsLoaded() override;
  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;
private:
  int m_expertBlackPreset{};
};
#endif
