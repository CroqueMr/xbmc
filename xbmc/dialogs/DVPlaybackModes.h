/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef HAVE_DVBRIDGE
#include "settings/lib/ISettingCallback.h"
enum class StreamHdrType;

class CDVPlaybackModes : public ISettingCallback
{
public:
  static CDVPlaybackModes& GetInstance();
  static bool MenuEnabled();
  static StreamHdrType SourceType();
  static bool ShowMenu();
  static bool ConfigureProfile();
  void OnSettingAction(const std::shared_ptr<const CSetting>& setting) override;
};
#endif
