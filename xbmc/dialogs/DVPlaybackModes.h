/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef HAVE_DVBRIDGE
#include "settings/lib/ISettingCallback.h"
#include "settings/lib/ISettingsHandler.h"

class CDVPlaybackModes : public ISettingCallback, public ISettingsHandler
{
public:
  static CDVPlaybackModes& GetInstance();
  static bool MenuEnabled();
  static bool ShowMenu();
  static bool ConfigureProfile();
  static bool DriverProfilesAvailable();
  bool OnSettingChanging(const std::shared_ptr<const CSetting>& setting) override;
  void OnSettingsLoaded() override;
  void OnSettingAction(const std::shared_ptr<const CSetting>& setting) override;
private:
  bool m_syncingProfile{false};
  bool m_driverProfilesAvailable{false};
  bool m_standardProfile{false};
};
#endif
