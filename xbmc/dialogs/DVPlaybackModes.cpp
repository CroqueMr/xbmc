/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "DVPlaybackModes.h"
#ifdef HAVE_DVBRIDGE
#include "FileItem.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "GUIDialogNumeric.h"
#include "guilib/GUIKeyboardFactory.h"
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
#include "utils/DVBridgeDiagnosticLog.h"
#include "utils/log.h"
#include "threads/IRunnable.h"
#include <cstdio>

namespace
{
struct Choice { int output,method,label; };
constexpr Choice dvChoices[]={{0,0,40812},{4,0,40913},
    {5,0,40914},
    {2,0,40814},{1,0,40822},{1,1,40823},{1,2,40922}};
}

CDVPlaybackModes& CDVPlaybackModes::GetInstance()
{
  static CDVPlaybackModes instance;
  return instance;
}

void CDVPlaybackModes::OnSettingsLoaded()
{
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  m_expertBlackPreset=settings->GetInt("dvbridge.expertblack");
}

void CDVPlaybackModes::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if(!setting)return;
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  if(setting->GetId()=="dvbridge.expertblackcustom")
    settings->SetBool("dvbridge.expertblackcustomset",true);
  else if(setting->GetId()=="dvbridge.expertblack")
  {
    const int mode=settings->GetInt("dvbridge.expertblack");
    if(mode==3 && !settings->GetBool("dvbridge.expertblackcustomset"))
    {
      settings->SetNumber("dvbridge.expertblackcustom",
          m_expertBlackPreset==1 ? 0.005 : m_expertBlackPreset==2 ? 0.05 : 0);
      settings->SetBool("dvbridge.expertblackcustomset",true);
    }
    if(mode!=3)m_expertBlackPreset=mode;
  }
}

bool CDVPlaybackModes::MenuEnabled()
{
  return DVBRIDGE::PlaybackMenuEnabled();
}

StreamHdrType CDVPlaybackModes::SourceType()
{
  const auto player=CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  VideoStreamInfo source;
  if (player->IsPlayingVideo()) player->GetVideoStreamInfo(CURRENT_STREAM,source);
  if (!source.valid || (!source.stereoMode.empty() && source.stereoMode!="mono"))
    return StreamHdrType::HDR_TYPE_NONE;
  return source.hdrType;
}

bool CDVPlaybackModes::ConfigureProfile()
{
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!settings || !dialog) return false;
  const auto preferences=DVBRIDGE::FrameOutputSettings();
  const bool expert=preferences.output==DVBRIDGE::OutputMode::HDR10 && preferences.method==1;
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  std::string peak=StringUtils::Format("{:g}",expert?preferences.expertPeak:preferences.peak);
  if (expert ? !CGUIKeyboardFactory::ShowAndGetInput(peak,strings.Get(41030),false) :
               !CGUIDialogNumeric::ShowAndGetNumber(peak,strings.Get(40829))) return false;
  double value{};
  try
  {
    size_t length{};
    value=std::stod(peak,&length);
    if (length!=peak.size() || !std::isfinite(value) || value<(expert?50:0.000001) || value>10000) return false;
  }
  catch (const std::exception&) { return false; }
  dialog->Reset(); dialog->SetHeading(40836);
  for(int id : {40838,40839,40840}) dialog->Add(strings.Get(id));
  const int currentGamut=expert?preferences.expertGamut:preferences.gamut;
  dialog->SetSelected(currentGamut>0 ? currentGamut-1 : 1);
  dialog->Open();
  if (!dialog->IsConfirmed()) return false;
  const int gamut=dialog->GetSelectedItem()+1;
  auto profile=preferences;
  if(expert){profile.expertPeak=value;profile.expertGamut=gamut;}
  else{profile.peak=value;profile.gamut=gamut;}
  if (!(expert?profile.ExpertProfileValid():profile.ProfileValid())) return false;
  {
    std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
    const char* peakKey=expert?"dvbridge.expertpeak":"dvbridge.tvpeak";
    const char* gamutKey=expert?"dvbridge.expertgamut":"dvbridge.tvgamut";
    if (!settings->SetNumber(peakKey,value) || !settings->SetInt(gamutKey,gamut))
    {
      settings->SetNumber(peakKey,expert?preferences.expertPeak:preferences.peak);
      settings->SetInt(gamutKey,currentGamut);
      return false;
    }
  }
  settings->Save();
  return true;
}

bool CDVPlaybackModes::ShowMenu(bool settingsMenu)
{
  if (!settingsMenu && !MenuEnabled()) return false;
  auto* dialog=CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog) return false;
  const auto settings=CServiceBroker::GetSettingsComponent()->GetSettings();
  const auto& strings=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const auto nativeSource=settingsMenu ? StreamHdrType::HDR_TYPE_DOLBYVISION : SourceType();
  const bool hdr10PlusSource=nativeSource==StreamHdrType::HDR_TYPE_HDR10PLUS;
  const bool hdr10Source=nativeSource==StreamHdrType::HDR_TYPE_HDR10 ||
                         nativeSource==StreamHdrType::HDR_TYPE_HDR10PLUS;
  if (!hdr10Source && nativeSource!=StreamHdrType::HDR_TYPE_DOLBYVISION) return false;
  const auto source=DVBRIDGE::StreamTelemetry::Read().transaction.capabilities;
#ifdef CB1_HAS_HDR10_AI
  constexpr Choice hdr10Choices[]={{1,0,40867},{2,0,40868},{0,0,40866}};
  constexpr Choice hdr10PlusChoices[]={{3,0,40970},{0,0,41014}};
#else
  constexpr Choice hdr10Choices[]={{0,0,40866}};
  constexpr Choice hdr10PlusChoices[]={{0,0,41014}};
#endif
  const Choice* choices=hdr10PlusSource ? hdr10PlusChoices : hdr10Source ? hdr10Choices : dvChoices;
  const int count=hdr10PlusSource ? std::size(hdr10PlusChoices) : hdr10Source ? std::size(hdr10Choices) : std::size(dvChoices);
  const char* sourceSetting=hdr10PlusSource ? "dvbridge.hdr10plussource" : "dvbridge.hdr10source";
  const auto choiceSetting=std::make_shared<CSettingInt>(settingsMenu ? "dvbridge.playback" : hdr10PlusSource ? "dvbridge.hdr10plusplaybackchoice" :
      hdr10Source ? "dvbridge.hdr10playbackchoice" : "dvbridge.playbackchoice");
  {
    const auto preferences=DVBRIDGE::FrameOutputSettings(hdr10PlusSource);
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
    DVBRIDGE::DiagnosticLog(LOGINFO,
        "Playback mode menu: native_source={} source_known={} dv={} hdr10={} hdr10plus={} processing_known={} requested={} hdr10source={}",
        int(nativeSource), source.sourceKnown, source.isDV, source.isHDR10, source.isHDR10Plus, source.processingKnown,
        int(preferences.output), int(preferences.hdr10Source));
    dialog->Reset();
    dialog->SetHeading(settingsMenu ? strings.Get(40811) :
        StringUtils::Format(strings.Get(40876), hdr10PlusSource ? "HDR10+" : hdr10Source ? "HDR10" : "Dolby Vision"));
    dialog->SetUseDetails(!settingsMenu);
    for(const auto& option : options)
    {
      DVBRIDGE::DiagnosticLog(LOGINFO, "Playback mode option: value={} label={} reason={}",
          option.value, option.label, option.label2);
      CFileItem item(option.label);
      item.SetLabel2(option.label2);
      for(const auto& [name,value] : option.properties) item.SetProperty(name,value);
      item.SetProperty("CB1.TextColor", item.GetProperty("disabled").asBoolean() ?
                                      "FFA0A0A0" : "FFF0F0F0");
      dialog->Add(item);
    }
    dialog->SetSelected(current);
    if (!settingsMenu) dialog->SetProperty("CB1.PlaybackModes",true);
    dialog->Open();
    dialog->SetProperty("CB1.PlaybackModes", CVariant{});
    if (!dialog->IsConfirmed()) return true;
    const int selected=dialog->GetSelectedItem();
    if (selected<0 || selected>=count) return true;
    // Admission may change during the dialog's nested event loop.
    if (!settingsMenu && SourceType()!=nativeSource) return true;
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    for(const auto& [name,value] : options[selected].properties)
      if (name=="disabled" && value.asBoolean()) return true;
    const auto& choice=choices[selected];
    {
      std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
      const int priorOutput=settings->GetInt("dvbridge.outputmode");
      const int priorHDR10=settings->GetInt(sourceSetting);
      const int priorMethod=settings->GetInt("dvbridge.hdr10method");
      if (!(hdr10Source ? settings->SetInt(sourceSetting,choice.output) :
          (settings->SetInt("dvbridge.outputmode",choice.output) &&
           settings->SetInt("dvbridge.hdr10method",choice.method))))
      {
        settings->SetInt("dvbridge.outputmode",priorOutput);
        settings->SetInt(sourceSetting,priorHDR10);
        settings->SetInt("dvbridge.hdr10method",priorMethod);
        return true;
      }
#ifdef CB1_HAS_HDR10_AI
      if (hdr10Source && choice.output != 0) ++DVBRIDGE::hdr10Reselection;
#endif
    }
    settings->Save();
    return true;
  }
}

void CDVPlaybackModes::OnSettingAction(const std::shared_ptr<const CSetting>& setting)
{
  if (setting && setting->GetId()=="dvbridge.configureprofile") ConfigureProfile();
  else if (setting && setting->GetId()=="dvbridge.playback") ShowMenu(true);
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
