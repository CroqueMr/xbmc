/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "DVPlaybackModes.h"
#ifdef HAVE_DVBRIDGE
#include "FileItem.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
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
#include "messaging/ApplicationMessenger.h"
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
#include "utils/DVBridgeLldv.h"
#ifdef HAVE_GBM
#include "windowing/gbm/WinSystemGbm.h"
#endif
#include "utils/log.h"
#include "threads/IRunnable.h"
#include <algorithm>
#include <cstdio>

namespace
{
struct Choice { int output,method,label; };
constexpr Choice dvChoices[]={{0,0,40812},{4,0,40913},
    {5,0,40914},
    {2,0,40814},{1,0,40822},{1,1,40823},{1,2,40922},{6,0,41130},{7,0,41221}};
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
  auto preferences=DVBRIDGE::FrameOutputSettings();
  {
    std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
    DVBRIDGE::ReadDefaultOutputSelectors().Apply(preferences);
  }
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

std::string CDVPlaybackModes::DefaultPlaybackLabel()
{
  std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
  const auto defaults=DVBRIDGE::ReadDefaultOutputSelectors();
  for (const auto& choice : dvChoices)
    if (int(defaults.output)==choice.output &&
        (choice.output!=1 || defaults.method==choice.method))
      return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(choice.label);
  return {};
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
  const bool hlgSource=nativeSource==StreamHdrType::HDR_TYPE_HLG;
  if (!hdr10Source && !hlgSource && nativeSource!=StreamHdrType::HDR_TYPE_DOLBYVISION) return false;
  constexpr Choice hlgChoices[]={{1,0,40867},{0,0,41311},{2,0,41130}};
  const auto source=DVBRIDGE::StreamTelemetry::Read().transaction.capabilities;
#ifdef CB1_HAS_HDR10_AI
  constexpr Choice hdr10Choices[]={{1,0,40867},{2,0,40868},{0,0,41310},{4,0,41130}};
  constexpr Choice hdr10PlusChoices[]={{3,0,40970},{5,0,41014},{0,0,40866},{4,0,41130}};
#else
  constexpr Choice hdr10Choices[]={{0,0,41310}};
  constexpr Choice hdr10PlusChoices[]={{5,0,41014},{0,0,40866}};
#endif
  const Choice* choices=hlgSource ? hlgChoices : hdr10PlusSource ? hdr10PlusChoices : hdr10Source ? hdr10Choices : dvChoices;
  const int count=hlgSource ? std::size(hlgChoices) : hdr10PlusSource ? std::size(hdr10PlusChoices) : hdr10Source ? std::size(hdr10Choices) : std::size(dvChoices);
  const char* sourceSetting=hlgSource ? "dvbridge.hlgsource" : hdr10PlusSource ? "dvbridge.hdr10plussource" : "dvbridge.hdr10source";
  const auto choiceSetting=std::make_shared<CSettingInt>(settingsMenu ? "dvbridge.playback" : hlgSource ? "dvbridge.hlgplaybackchoice" : hdr10PlusSource ? "dvbridge.hdr10plusplaybackchoice" :
      hdr10Source ? "dvbridge.hdr10playbackchoice" : "dvbridge.playbackchoice");
  {
    auto preferences=DVBRIDGE::FrameOutputSettings(hdr10PlusSource);
    if (settingsMenu)
    {
      std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
      DVBRIDGE::ReadDefaultOutputSelectors().Apply(preferences);
    }
    std::vector<IntegerSettingOption> options;
    int current=-1;
    for(int i=0;i<count;++i)
    {
      options.emplace_back(strings.Get(choices[i].label),i);
      const auto& choice=choices[i];
      if (hlgSource ? (int(preferences.hlgSource)==choice.output) :
          hdr10PlusSource ? (int(preferences.hdr10PlusSource)==choice.output) :
          hdr10Source ? (int(preferences.hdr10Source)==choice.output) :
          (int(preferences.output)==choice.output &&
          (choice.output!=1 || preferences.method==choice.method))) current=i;
    }
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    DVBRIDGE::DiagnosticLog(LOGINFO,
        "Playback mode menu: native_source={} source_known={} dv={} hdr10={} hdr10plus={} processing_known={} requested={} hdr10source={}",
        int(nativeSource), source.sourceKnown, source.isDV, source.isHDR10, source.isHDR10Plus, source.processingKnown,
        int(preferences.output), int(preferences.hdr10Source));
    dialog->Reset();
    dialog->SetHeading(strings.Get(settingsMenu ? 40811 : 40876));
    bool cb1Active = false;
    if (!settingsMenu)
    {
      const auto video = CServiceBroker::GetDataCacheCore().GetVideoProcessingInfo();
      cb1Active = video.output.flags != 0;
      dialog->SetProperty("CB1.SwitchInput", hlgSource ? "HLG" : hdr10PlusSource ? "HDR10+" : hdr10Source ? "HDR10" : "Dolby Vision");
      const char* output = nullptr;
      if (video.outputType)
      {
        switch (*video.outputType)
        {
          case VideoOutputType::DolbyVision:
            output = video.output.lldv ? strings.Get(41221).c_str() :
                (video.output.flags & CDVPlaybackInfo::HDMI) ? "Dolby Vision · TV-Led" : "Dolby Vision";
            break;
          case VideoOutputType::HDR10: output = "HDR10"; break;
          case VideoOutputType::HDR10Plus: output = "HDR10+"; break;
          case VideoOutputType::HLG: output = "HLG"; break;
          case VideoOutputType::SDR: output = "SDR"; break;
        }
      }
      dialog->SetProperty("CB1.SwitchOutput", output ? output : strings.Get(40877));
    }
    bool recommendMaster = false;
#ifdef HAVE_GBM
    if (!settingsMenu && nativeSource == StreamHdrType::HDR_TYPE_DOLBYVISION)
    {
      const auto* window =
          dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbm*>(CServiceBroker::GetWinSystem());
      recommendMaster = window && window->GetLldvConnectorStatus().sink.status == DV_EDID_STANDARD &&
          std::ranges::any_of(options, [choices](const auto& option) {
            return choices[option.value].output ==
                       static_cast<int>(DVBRIDGE::OutputMode::MasterReference) &&
                   std::ranges::none_of(option.properties, [](const auto& property) {
                     return property.first == "disabled" && property.second.asBoolean();
                   });
          });
    }
#endif
    dialog->SetUseDetails(true);
    for(const auto& option : options)
    {
      DVBRIDGE::DiagnosticLog(LOGINFO, "Playback mode option: value={} label={} reason={}",
          option.value, option.label, option.label2);
      const auto& choice = choices[option.value];
      const bool gray = settingsMenu && std::ranges::any_of(option.properties, [](const auto& property) {
        return property.first == "disabled" && property.second.asBoolean();
      });
      CFileItem item(settingsMenu ? (gray ? "[COLOR grey]" + option.label + "[/COLOR]" : option.label)
                                  : strings.Get(choice.label));
      item.SetLabel2(option.label2);
      for(const auto& [name,value] : option.properties) item.SetProperty(name,value);
      const bool disabled = item.GetProperty("disabled").asBoolean();
      const bool active = !settingsMenu && option.value == current;
      item.SetProperty("CB1.TextColor", disabled ? "FFBEC7CB" :
                                      active && cb1Active ? "FF65E18D" : "FFF2F5F6");
      if (!settingsMenu)
      {
        item.SetProperty("CB1.SwitchStatus", disabled ? strings.Get(40851) :
                                            active ? strings.Get(40878) : std::string{});
        if (recommendMaster && !disabled && option.label2.empty() &&
            choice.output == static_cast<int>(DVBRIDGE::OutputMode::PlayerLed))
          item.SetLabel2(strings.Get(40881));
      }
      dialog->Add(item);
    }
    dialog->SetSelected(current);
    if (!settingsMenu) dialog->SetProperty("CB1.PlaybackModes",true);
    dialog->Open();
    dialog->SetProperty("CB1.PlaybackModes", CVariant{});
    dialog->SetProperty("CB1.SwitchInput", CVariant{});
    dialog->SetProperty("CB1.SwitchOutput", CVariant{});
    if (!dialog->IsConfirmed()) return true;
    const int selected=dialog->GetSelectedItem();
    if (selected<0 || selected>=count) return true;
    // Admission may change during the dialog's nested event loop.
    if (!settingsMenu && SourceType()!=nativeSource) return true;
    CSettingConditions::DVOutputOptions(choiceSetting,options);
    for(const auto& [name,value] : options[selected].properties)
      if (name=="disabled" && value.asBoolean()) return true;
    const auto& choice=choices[selected];
    bool reprime = false;
    {
      std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
      if (settingsMenu)
      {
        const int priorOutput=settings->GetInt("dvbridge.outputmode");
        const int priorHDR10=settings->GetInt(sourceSetting);
        const int priorMethod=settings->GetInt("dvbridge.hdr10method");
        if (!((hdr10Source || hlgSource) ? settings->SetInt(sourceSetting,choice.output) :
            (settings->SetInt("dvbridge.outputmode",choice.output) &&
             settings->SetInt("dvbridge.hdr10method",choice.method))))
        {
          settings->SetInt("dvbridge.outputmode",priorOutput);
          settings->SetInt(sourceSetting,priorHDR10);
          settings->SetInt("dvbridge.hdr10method",priorMethod);
          return true;
        }
      }
      else
      {
        if (!DVBRIDGE::activeOutputSelectors)
          DVBRIDGE::activeOutputSelectors=DVBRIDGE::ReadDefaultOutputSelectors();
        auto& active=*DVBRIDGE::activeOutputSelectors;
        if (hlgSource)
        {
          const auto next = static_cast<DVBRIDGE::HLGSourceMode>(choice.output);
          reprime = next == DVBRIDGE::HLGSourceMode::ReferenceDV && active.hlgSource != next;
          active.hlgSource = next;
        }
        else if (hdr10PlusSource) active.hdr10PlusSource=static_cast<DVBRIDGE::HDR10SourceMode>(choice.output);
        else if (hdr10Source) active.hdr10Source=static_cast<DVBRIDGE::HDR10SourceMode>(choice.output);
        else {active.output=static_cast<DVBRIDGE::OutputMode>(choice.output);active.method=choice.method;}
      }
#ifdef CB1_HAS_HDR10_AI
      if (!settingsMenu && (hdr10Source || hlgSource) && choice.output != 0) ++DVBRIDGE::hdr10Reselection;
#endif
    }
    if (settingsMenu) settings->Save();
    if (reprime)
    {
      const auto player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
      if (player->CanSeek() && !player->IsInMenu() && player->GetPlaySpeed() == 1.0f)
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_SEEK_TIME, player->GetTime());
    }
    return true;
  }
}

namespace
{
int LldvFormatLabel(DVBRIDGE::LldvFormat format, bool forced)
{
  return forced ? 41217 : 41202 + static_cast<int>(format);
}

DVBRIDGE::LldvConnectorStatus ReadLldvConnector()
{
#ifdef HAVE_GBM
  const auto* gbm = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (gbm)
    return gbm->GetLldvConnectorStatus();
#endif
  return {};
}

int SDRBT2020Unavailable()
{
#ifdef HAVE_GBM
  const auto* gbm = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (gbm)
  {
    switch (gbm->GetSDRBT2020Support())
    {
      case KODI::WINDOWING::GBM::CWinSystemGbm::SDRBT2020Support::Available: return 0;
      case KODI::WINDOWING::GBM::CWinSystemGbm::SDRBT2020Support::Display: return 41274;
      case KODI::WINDOWING::GBM::CWinSystemGbm::SDRBT2020Support::Driver: break;
    }
  }
#endif
  return 41275;
}

void ShowSDRGamutMenu()
{
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog)
    return;

  const int reason = SDRBT2020Unavailable();
  dialog->Reset();
  dialog->SetHeading(strings.Get(41270));
  dialog->SetUseDetails(true);
  dialog->Add(CFileItem(strings.Get(41272)));
  const auto label = strings.Get(41273);
  CFileItem wide(reason ? "[COLOR grey]" + label + "[/COLOR]" : label);
  wide.SetProperty("disabled", reason != 0);
  if (reason)
    wide.SetLabel2(strings.Get(reason));
  dialog->Add(wide);
  dialog->SetSelected(reason ? 0 : settings->GetInt("dvbridge.sdrgamutvalue"));
  dialog->Open();
  const int selected = dialog->GetSelectedItem();
  if (dialog->IsConfirmed() && (selected == 0 || (selected == 1 && !SDRBT2020Unavailable())) &&
      settings->SetInt("dvbridge.sdrgamutvalue", selected))
    settings->Save();
}

void ShowLldvFormatMenu()
{
  using namespace DVBRIDGE;
  const auto status = ReadLldvConnector();
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog)
    return;

  const bool usableTiming = status.currentTiming || status.availableTiming;
  const auto preferred =
      ChooseLldvFormat(status.sink, status.driverFormats, usableTiming, status.forced42210);
  const auto requiresForcing = [&status](LldvFormat format) {
    return LldvRequiresForcing(format, status.driverFormats, status.forced42210);
  };
  const auto saved = settings->GetInt("dvbridge.lldvformatvalue");
  dialog->Reset();
  dialog->SetHeading(strings.Get(41200));
  // Estuary fits all formats in its simple list and displays the notes inline.
  const bool estuary = settings->GetString(CSettings::SETTING_LOOKANDFEEL_SKIN) == "skin.estuary";
  dialog->SetUseDetails(!estuary);
  dialog->EnableButton(!estuary, 41320);

  const int summary = LldvUnavailableLabel(status, 0, true);
  std::string summaryText = strings.Get(summary ? 41322 : 41321);
  if (summary)
    summaryText += '\n' + strings.Get(summary);
  dialog->SetProperty("CB1.LldvStatus", summaryText);

  std::string details, labels, values;
  const auto addDetail = [&](int id, const std::string& value) {
    const auto label = strings.Get(id);
    if (!details.empty())
    {
      details += '\n';
      labels += '\n';
      values += '\n';
    }
    details += label + ": " + value;
    labels += label;
    values += value;
  };
  const auto& display = status.display;
  if (display.sink.length)
  {
    addDetail(41323, StringUtils::Format("v{}", display.sink.version));
    addDetail(41324, strings.Get(display.sink.status == DV_EDID_LLDV_ONLY ? 41291 :
                                dvbridge_lldv_422(display.sink) ? 41292 : 41293));
  }
  else
    addDetail(41323, strings.Get(40851));
  if (display.status == LldvEdidStatus::Incomplete)
    addDetail(41330, StringUtils::Format("{} / {} bytes", display.received, display.expected));
  if (display.peak > 0)
  {
    addDetail(41325, StringUtils::Format("{:.0f} nits", display.peak));
    addDetail(41326, StringUtils::Format("{:g} nits", display.black));
    for (unsigned i = 0; i < 3; ++i)
      addDetail(41327 + i, StringUtils::Format("{:.4f}, {:.4f}",
                                             display.reported[i * 2], display.reported[i * 2 + 1]));
  }
  dialog->SetProperty("CB1.LldvDetails", details);
  dialog->SetProperty("CB1.LldvDetailLabels", labels);
  dialog->SetProperty("CB1.LldvDetailValues", values);
  for (int i = 0; i <= static_cast<int>(LldvFormat::Y444_12); ++i)
  {
    const auto format = static_cast<LldvFormat>(i);
    const int reason = LldvUnavailableLabel(status, i, true);
    const bool disabled = format != LldvFormat::Auto && reason != 0;
    const bool forced = requiresForcing(format);
    const auto label = strings.Get(LldvFormatLabel(format, !estuary && forced));
    CFileItem item(disabled ? "[COLOR grey]" + label + "[/COLOR]" : label);
    item.SetProperty("disabled", disabled);
    if (reason)
      item.SetLabel2(strings.Get(reason));
    else if (format != LldvFormat::Auto && format == status.rejected)
      item.SetLabel2(strings.Get(41298));
    else if (i == 0)
      item.SetLabel2(preferred == LldvFormat::Auto ? strings.Get(41213)
          : strings.Get(LldvFormatLabel(preferred, !estuary && requiresForcing(preferred))));
    else if (format == status.active)
      item.SetLabel2(strings.Get(status.activeForced && !requiresForcing(format) ? 41218 : 41214));
    else
      item.SetLabel2(strings.Get(format == LldvFormat::Y422_10 ? 41216 : 41215));
    if (estuary)
    {
      if (disabled)
        item.SetProperty("CB1.LldvFocusNote", strings.Get(reason == 41210 ? 41332 : reason));
      else if (format != LldvFormat::Auto && format == status.rejected)
        item.SetProperty("CB1.LldvFocusNote", item.GetLabel2());
      else if (i == 0 || forced)
        item.SetProperty("CB1.LldvNote", i == 0 ? item.GetLabel2() : strings.Get(41331));
    }
    dialog->Add(item);
  }
  dialog->SetSelected(saved);
  dialog->Open();
  for (const char* key : {"CB1.LldvStatus", "CB1.LldvDetails", "CB1.LldvDetailLabels",
                          "CB1.LldvDetailValues"})
    dialog->SetProperty(key, CVariant{});
  const auto current = ReadLldvConnector();
  const int choice = dialog->GetSelectedItem();
  if (dialog->IsConfirmed() && choice >= 0 && choice <= static_cast<int>(LldvFormat::Y444_12) &&
      (choice == static_cast<int>(LldvFormat::Auto) ||
       (current.connectorId == status.connectorId && current.edidGeneration == status.edidGeneration &&
        !LldvUnavailableLabel(current, choice, true))) &&
      settings->SetInt("dvbridge.lldvformatvalue", choice))
    settings->Save();
}
} // namespace

std::string CDVPlaybackModes::LldvPreferenceLabel()
{
  using namespace DVBRIDGE;
  const auto status = ReadLldvConnector();
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int saved = settings->GetInt("dvbridge.lldvformatvalue");
  const auto format = LldvFormatPreference(saved);
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  if (format != LldvFormat::Auto && LldvUnavailableLabel(status, saved, true))
    return strings.Get(40851);
  return strings.Get(LldvFormatLabel(format,
      LldvRequiresForcing(format, status.driverFormats, status.forced42210)));
}

std::string CDVPlaybackModes::SDRGamutLabel()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(
      settings->GetInt("dvbridge.sdrgamutvalue")==1 && !SDRBT2020Unavailable() ? 41273 : 41272);
}

void CDVPlaybackModes::OnSettingAction(const std::shared_ptr<const CSetting>& setting)
{
  if (setting && setting->GetId()=="dvbridge.configureprofile") ConfigureProfile();
  else if (setting && setting->GetId()=="dvbridge.playback") ShowMenu(true);
  else if (setting && setting->GetId()=="dvbridge.lldvformat") ShowLldvFormatMenu();
  else if (setting && setting->GetId()=="dvbridge.sdrgamut") ShowSDRGamutMenu();
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
#endif
