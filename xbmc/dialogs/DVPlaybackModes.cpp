/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "DVPlaybackModes.h"
#ifdef HAVE_DVBRIDGE
#include "FileItem.h"
#include "ServiceBroker.h"
#include "GUIDialogNumeric.h"
#include "GUIDialogKaiToast.h"
#include "GUIDialogSelect.h"
#include "GUIDialogBusy.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/SettingConditions.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingDefinitions.h"
#include "utils/StringUtils.h"
#include "utils/JSONVariantParser.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "threads/IRunnable.h"
#include <cstdio>

#ifdef CB1_DEBUG_BUILD
namespace
{
bool DriverProfileCommand(const char* command, CVariant& status)
{
  const std::string invocation = std::string("/usr/bin/cb1-driver-profile ") + command;
  FILE* pipe = popen(invocation.c_str(), "r");
  if (!pipe) return false;
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), pipe) && output.size() < 16384)
    output += buffer;
  const int result = pclose(pipe);
  return result == 0 && output.size() < 16384 && CJSONVariantParser::Parse(output, status);
}

bool SelectDriverProfile(bool standard, CVariant& status)
{
  struct Selection : IRunnable
  {
    Selection(bool selected, CVariant& output) : standard(selected), status(output) {}
    bool standard;
    CVariant& status;
    bool success{false};
    void Run() override
    {
      success = DriverProfileCommand(standard ? "select standard" : "select modified", status);
    }
  } selection(standard, status);
  return CGUIDialogBusy::Wait(&selection, 100, false) && selection.success;
}
}
#endif

CDVPlaybackModes& CDVPlaybackModes::GetInstance()
{
  static CDVPlaybackModes instance;
  return instance;
}

bool CDVPlaybackModes::MenuEnabled()
{
  return DVBRIDGE::PlaybackMenuEnabled();
}

bool CDVPlaybackModes::ConfigureProfile()
{
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!settings || !dialog) return false;
  const auto preferences=DVBRIDGE::FrameOutputSettings();
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  std::string peak=StringUtils::Format("{:g}",preferences.peak);
  if (!CGUIDialogNumeric::ShowAndGetNumber(peak,strings.Get(40829))) return false;
  double value{};
  try
  {
    size_t length{};
    value=std::stod(peak,&length);
    if (length!=peak.size() || !std::isfinite(value) || value<=0 || value>10000) return false;
  }
  catch (const std::exception&) { return false; }
  dialog->Reset(); dialog->SetHeading(40836);
  for(int id : {40838,40839,40840}) dialog->Add(strings.Get(id));
  dialog->SetSelected(preferences.gamut>0 ? preferences.gamut-1 : 1);
  dialog->Open();
  if (!dialog->IsConfirmed()) return false;
  const int gamut=dialog->GetSelectedItem()+1;
  auto profile=preferences;
  profile.peak=value; profile.gamut=gamut;
  if (!profile.ProfileValid()) return false;
  {
    std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
    if (!settings->SetNumber("dvbridge.tvpeak",value) ||
        !settings->SetInt("dvbridge.tvgamut",gamut))
    {
      settings->SetNumber("dvbridge.tvpeak",preferences.peak);
      settings->SetInt("dvbridge.tvgamut",preferences.gamut);
      return false;
    }
  }
  settings->Save();
  return true;
}

bool CDVPlaybackModes::ShowMenu()
{
  if (!MenuEnabled()) return false;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog) return false;
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  struct Choice { int output,method,label; };
  constexpr Choice dvChoices[]={{0,0,40812},{4,0,40913},
#ifdef CB1_ENABLE_MASTER_IPT
      {6,0,40916},
#endif
      {5,0,40914},
      {2,0,40814},{1,0,40822},{1,1,40823}};
  const auto source=DVBRIDGE::StreamTelemetry::Read().transaction.capabilities;
  if (source.stereoSource || !source.sourceKnown || (!source.isDV && !source.isHDR10)) return false;
  const bool hdr10Source=source.isHDR10 && !source.isDV;
#ifdef CB1_HAS_HDR10_AI
  constexpr Choice hdr10Choices[]={{1,0,40867},{2,0,40868},{0,0,40866}};
#else
  constexpr Choice hdr10Choices[]={{0,0,40866}};
#endif
  const Choice* choices=hdr10Source ? hdr10Choices : dvChoices;
  const int count=hdr10Source ? std::size(hdr10Choices) : std::size(dvChoices);
  const auto choiceSetting=std::make_shared<CSettingInt>(hdr10Source ?
      "dvbridge.hdr10playbackchoice" : "dvbridge.playbackchoice");
  {
    const auto preferences=DVBRIDGE::FrameOutputSettings();
    std::vector<IntegerSettingOption> options;
    int current=-1;
    for(int i=0;i<count;++i)
    {
      options.emplace_back(strings.Get(choices[i].label),i);
      const auto& choice=choices[i];
      if (hdr10Source ? (int(preferences.hdr10Source)==choice.output) :
          (int(preferences.output)==choice.output &&
          (choice.output!=1 || preferences.method==choice.method))) current=i;
    }
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    dialog->Reset();
    dialog->SetHeading(StringUtils::Format(strings.Get(40876), hdr10Source ? "HDR10" : "Dolby Vision"));
    dialog->SetUseDetails(true);
    for(const auto& option : options)
    {
      CFileItem item(strings.Get(choices[option.value].label));
      item.SetLabel2(option.label2);
      for(const auto& [name,value] : option.properties) item.SetProperty(name,value);
      dialog->Add(item);
    }
    dialog->SetSelected(current);
    dialog->SetProperty("CB1.PlaybackModes",true);
    dialog->Open();
    dialog->SetProperty("CB1.PlaybackModes", CVariant{});
    if (!dialog->IsConfirmed()) return true;
    const int selected=dialog->GetSelectedItem();
    if (selected<0 || selected>=count) return true;
    // Admission may change during the dialog's nested event loop.
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    for(const auto& [name,value] : options[selected].properties)
      if (name=="disabled" && value.asBoolean()) return true;
    const auto& choice=choices[selected];
    {
      std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
      const int priorOutput=settings->GetInt("dvbridge.outputmode");
      const int priorHDR10=settings->GetInt("dvbridge.hdr10source");
      const int priorMethod=settings->GetInt("dvbridge.hdr10method");
      if (!(hdr10Source ? settings->SetInt("dvbridge.hdr10source",choice.output) :
          (settings->SetInt("dvbridge.outputmode",choice.output) &&
           settings->SetInt("dvbridge.hdr10method",choice.method))))
      {
        settings->SetInt("dvbridge.outputmode",priorOutput);
        settings->SetInt("dvbridge.hdr10source",priorHDR10);
        settings->SetInt("dvbridge.hdr10method",priorMethod);
        return true;
      }
#ifdef CB1_HAS_HDR10_AI
      if (hdr10Source && choice.output != 0) ++DVBRIDGE::hdr10Reselection;
#endif
      if (!hdr10Source) DVBRIDGE::automaticFastRendering.store(false);
    }
    settings->Save();
    return true;
  }
}

void CDVPlaybackModes::OnSettingAction(const std::shared_ptr<const CSetting>& setting)
{
  if (setting && setting->GetId()=="dvbridge.configureprofile") ConfigureProfile();
}

bool CDVPlaybackModes::DriverProfilesAvailable()
{
  return GetInstance().m_driverProfilesAvailable;
}

void CDVPlaybackModes::OnSettingsLoaded()
{
#ifdef CB1_DEBUG_BUILD
  CVariant status;
  m_driverProfilesAvailable = DriverProfileCommand("status", status) && status["available"].asBoolean();
  CLog::Log(LOGINFO, "CB1 driver profile: active={} pending={} available={} reason={}",
      status["active"].asString(), status["pending"].asString(), m_driverProfilesAvailable,
      status["reason"].asString());
  if (m_driverProfilesAvailable)
    CLog::Log(LOGINFO, "CB1 Debug boot: build={} kernel={} kernel_sha256={} system_sha256={} modules_sha256={}",
        status["build"]["id"].asString(), status["kernel_release"].asString(),
        status["payloads"]["kernel"]["sha256"].asString(),
        status["payloads"]["system"]["sha256"].asString(),
        status["payloads"]["modules_sha256"].asString());
  if (m_driverProfilesAvailable)
  {
    m_standardProfile = status["pending"].asString() == "standard";
    m_syncingProfile = true;
    CServiceBroker::GetSettingsComponent()->GetSettings()->SetBool(
        "debug.cb1standarddrivers", m_standardProfile);
    m_syncingProfile = false;
  }
#endif
}

bool CDVPlaybackModes::OnSettingChanging(const std::shared_ptr<const CSetting>& setting)
{
#ifdef CB1_DEBUG_BUILD
  if (!setting || setting->GetId() != "debug.cb1standarddrivers" || m_syncingProfile)
    return true;
  const bool standard = std::static_pointer_cast<const CSettingBool>(setting)->GetValue();
  // Kodi repeats this callback with the previous value after a refused change.
  if (standard == m_standardProfile) return true;
  CVariant status;
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  if (!SelectDriverProfile(standard, status) ||
      !status["available"].asBoolean() ||
      status["pending"].asString() != (standard ? "standard" : "modified"))
  {
    CLog::Log(LOGERROR, "CB1 driver profile selection refused: {}", status["reason"].asString());
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
        strings.Get(40917), strings.Get(40920), 5000, false);
    return false;
  }
  m_standardProfile = standard;
  CLog::Log(LOGINFO, "CB1 driver profile selected: active={} pending={} reboot_required=true",
      status["active"].asString(), status["pending"].asString());
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
      strings.Get(40917), strings.Get(40919), 3000, false);
#endif
  return true;
}

void DVBRIDGE::PlaybackNotification::Emit(const std::string& text)
{
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  if (settings && settings->GetBool("dvbridge.startupmode"))
    CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
        CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(40863),text,3000,false);
}
void DVBRIDGE::PlaybackNotification::EmitAI(int mode)
{
  if (mode!=1 && mode!=4) return;
  const int label=mode==1 ? 40867 : 40868;
  Emit(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(label));
}
void DVBRIDGE::PlaybackNotification::HDR10Recovery()
{
  const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
      localizer.Get(40863),localizer.Get(40875),3000,false);
}
void DVBRIDGE::PlaybackNotification::EmitMasterRecovery()
{
  const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info,
      localizer.Get(40863),localizer.Get(40915),3000,false);
}
#endif
