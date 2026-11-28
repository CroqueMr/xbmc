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
#include "utils/log.h"
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
#include <string_view>

namespace
{
bool ShowLldvFormatMenu(bool forCircuit = false, bool inPlayback = false);
struct Choice { int output,method,label; };
constexpr Choice dvChoices[]={{4,0,40913},{0,0,40812},
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
    return StreamHdrType::HDR_TYPE_COUNT;
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
  const auto source=DVBRIDGE::StreamTelemetry::Read().transaction.capabilities;
  const bool sdrSource=!settingsMenu && nativeSource==StreamHdrType::HDR_TYPE_NONE &&
      source.sourceKnown && source.isSDR && !source.stereoSource;
  if (!sdrSource && !hdr10Source && !hlgSource && nativeSource!=StreamHdrType::HDR_TYPE_DOLBYVISION) return false;
  constexpr Choice hlgChoices[]={{1,0,40867},{0,0,41311},{2,0,41130}};
  constexpr Choice sdrChoices[]={{0,0,41511},{1,0,40822},{2,0,40867}};
#ifdef CB1_HAS_HDR10_AI
  constexpr Choice hdr10Choices[]={{1,0,40867},{2,0,40868},{0,0,41310},{4,0,41130}};
  constexpr Choice hdr10PlusChoices[]={{3,0,40970},{5,0,41014},{0,0,40866},{4,0,41130}};
#else
  constexpr Choice hdr10Choices[]={{0,0,41310}};
  constexpr Choice hdr10PlusChoices[]={{5,0,41014},{0,0,40866}};
#endif
  const Choice* choices=sdrSource ? sdrChoices : hlgSource ? hlgChoices : hdr10PlusSource ? hdr10PlusChoices : hdr10Source ? hdr10Choices : dvChoices;
  const int count=sdrSource ? std::size(sdrChoices) : hlgSource ? std::size(hlgChoices) : hdr10PlusSource ? std::size(hdr10PlusChoices) : hdr10Source ? std::size(hdr10Choices) : std::size(dvChoices);
  const char* sourceSetting=sdrSource ? "dvbridge.sdrsource" : hlgSource ? "dvbridge.hlgsource" : hdr10PlusSource ? "dvbridge.hdr10plussource" : "dvbridge.hdr10source";
  const auto choiceSetting=std::make_shared<CSettingInt>(settingsMenu ? "dvbridge.playback" : sdrSource ? "dvbridge.sdrplaybackchoice" : hlgSource ? "dvbridge.hlgplaybackchoice" : hdr10PlusSource ? "dvbridge.hdr10plusplaybackchoice" :
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
    const auto video = CServiceBroker::GetDataCacheCore().GetVideoProcessingInfo();
    const bool activeFallback = !settingsMenu &&
        nativeSource == StreamHdrType::HDR_TYPE_DOLBYVISION &&
        preferences.hdr10TransportFallback && video.output.flags &&
        video.outputType == VideoOutputType::HDR10;
    const auto currentOutput = activeFallback ? DVBRIDGE::OutputMode::HDR10 : preferences.output;
    const int currentMethod = activeFallback ? 0 : preferences.method;
    for(int i=0;i<count;++i)
    {
      options.emplace_back(strings.Get(choices[i].label),i);
      const auto& choice=choices[i];
      if (sdrSource ? ((video.output.flags && video.output.sdrExpanded ?
          (video.outputType==VideoOutputType::DolbyVision ? 2 : 1) : 0)==choice.output) :
          hlgSource ? (int(preferences.hlgSource)==choice.output) :
          hdr10PlusSource ? (int(preferences.hdr10PlusSource)==choice.output) :
          hdr10Source ? (int(preferences.hdr10Source)==choice.output) :
          (int(currentOutput)==choice.output &&
          (choice.output!=1 || currentMethod==choice.method))) current=i;
    }
    CSettingConditions::DVOutputOptions(choiceSetting, options, !settingsMenu);
    DVBRIDGE::DiagnosticLog(LOGINFO,
        "Playback mode menu: native_source={} source_known={} dv={} hdr10={} hdr10plus={} processing_known={} requested={} hdr10source={}",
        int(nativeSource), source.sourceKnown, source.isDV, source.isHDR10, source.isHDR10Plus, source.processingKnown,
        int(preferences.output), int(preferences.hdr10Source));
    dialog->Reset();
    dialog->SetHeading(strings.Get(settingsMenu ? 40811 : 40876));
    bool cb1Active = false;
    if (!settingsMenu)
    {
      cb1Active = video.output.flags != 0;
      dialog->SetProperty("CB1.SwitchInput", sdrSource ? "SDR" : hlgSource ? "HLG" : hdr10PlusSource ? "HDR10+" : hdr10Source ? "HDR10" : "Dolby Vision");
      const char* output = nullptr;
      if (video.outputType)
      {
        switch (*video.outputType)
        {
          case VideoOutputType::DolbyVision:
            output = video.output.lldv ? strings.Get(41344).c_str() :
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
    bool recommendLldv = false;
#ifdef HAVE_GBM
    if (!settingsMenu && nativeSource == StreamHdrType::HDR_TYPE_DOLBYVISION)
    {
      const auto* window =
          dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbm*>(CServiceBroker::GetWinSystem());
      const auto selectable = [&](DVBRIDGE::OutputMode mode) {
        return std::ranges::any_of(options, [&](const auto& option) {
          return choices[option.value].output == static_cast<int>(mode) &&
              std::ranges::none_of(option.properties, [](const auto& property) {
                return (property.first == "disabled" || property.first == "cb1ChooseLldv") &&
                    property.second.asBoolean();
              });
        });
      };
      if (window)
      {
        const auto status = window->GetLldvConnectorStatus().sink.status;
        recommendMaster = status == DV_EDID_STANDARD &&
            selectable(DVBRIDGE::OutputMode::MasterReference);
        recommendLldv = status == DV_EDID_LLDV_ONLY &&
            selectable(DVBRIDGE::OutputMode::PlayerLed);
      }
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
        else if (recommendLldv && disabled &&
                 choice.output == static_cast<int>(DVBRIDGE::OutputMode::MasterReference))
          item.SetLabel2(strings.Get(41506));
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
    auto& selectedOption = options[selected];
    const auto refreshChoice = [&] {
      selectedOption.properties.clear();
      CSettingConditions::DVOutputOptions(choiceSetting, options, !settingsMenu);
    };
    const auto has = [&](std::string_view name) {
      return std::ranges::any_of(selectedOption.properties, [name](const auto& property) {
        return property.first == name && property.second.asBoolean();
      });
    };
    refreshChoice();
    if (has("disabled")) return true;
    if (has("cb1ChooseLldv"))
    {
      if (!ShowLldvFormatMenu(true, !settingsMenu) || (!settingsMenu && SourceType()!=nativeSource)) return true;
      refreshChoice();
      if (has("disabled") || has("cb1ChooseLldv")) return true;
    }
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
        if (sdrSource) active.sdrSource=static_cast<DVBRIDGE::SDRSourceMode>(choice.output);
        else if (hlgSource)
        {
          const auto next = static_cast<DVBRIDGE::HLGSourceMode>(choice.output);
          reprime = next == DVBRIDGE::HLGSourceMode::ReferenceDV && active.hlgSource != next;
          active.hlgSource = next;
        }
        else if (hdr10PlusSource) active.hdr10PlusSource=static_cast<DVBRIDGE::HDR10SourceMode>(choice.output);
        else if (hdr10Source) active.hdr10Source=static_cast<DVBRIDGE::HDR10SourceMode>(choice.output);
        else
        {
          active.output = static_cast<DVBRIDGE::OutputMode>(choice.output);
          active.method = choice.method;
          active.hdr10TransportFallback = false;
          active.playerLedStartup = false;
        }
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
    using Support = KODI::WINDOWING::GBM::CWinSystemGbm::SDRBT2020Support;
    const auto native = gbm->GetSDRBT2020Support(true);
    const auto converted = gbm->GetSDRBT2020Support();
    if (native == Support::Available || converted == Support::Available)
      return 0;
    if (native == Support::Display || converted == Support::Display)
      return 41274;
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
  static constexpr int values[] = {0, 2, 1};
  dialog->Add(CFileItem(strings.Get(40984)));
  dialog->Add(CFileItem(strings.Get(41272)));
  const auto label = strings.Get(41273);
  CFileItem wide(reason ? "[COLOR grey]" + label + "[/COLOR]" : label);
  wide.SetProperty("disabled", reason != 0);
  if (reason)
    wide.SetLabel2(strings.Get(reason));
  dialog->Add(wide);
  const int value = settings->GetInt("dvbridge.sdrgamutvalue");
  dialog->SetSelected(value == 1 ? 2 : value == 2 ? 1 : 0);
  dialog->Open();
  const int selected = dialog->GetSelectedItem();
  if (dialog->IsConfirmed() && selected >= 0 && selected < static_cast<int>(std::size(values)) &&
      (values[selected] != 1 || !SDRBT2020Unavailable()) &&
      settings->SetInt("dvbridge.sdrgamutvalue", values[selected]))
    settings->Save();
}

bool ShowLldvFormatMenu(bool forCircuit, bool inPlayback)
{
  using namespace DVBRIDGE;
  const auto status = ReadLldvConnector();
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  auto* dialog = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogSelect>(WINDOW_DIALOG_SELECT);
  if (!dialog)
    return false;

  const bool allowModeSwitch = !inPlayback && status.active == LldvFormat::Auto;
  const auto preferred = SelectLldvTransport(status, 0, allowModeSwitch).format;
  const auto requiresForcing = [&status, allowModeSwitch](LldvFormat format) {
    return LldvRequiresForcing(format, status.driverFormats, status.forced42210) ||
        (format == LldvFormat::Y422_10 &&
         SelectLldvTransport(status, LLDV_FORCE_422_10, allowModeSwitch).forced);
  };
  const auto preferenceFor = [&status, &requiresForcing, allowModeSwitch](LldvFormat format) {
    if (requiresForcing(format))
      return LLDV_FORCE_422_10;
    if (format == LldvFormat::Y422_12 &&
        SelectLldvTransport(status, LLDV_PACKED_422_12, allowModeSwitch).experimentalPacked12)
      return LLDV_PACKED_422_12;
    return static_cast<int>(format);
  };
  const auto saved = settings->GetInt("dvbridge.lldvformatvalue");
  dialog->Reset();
  dialog->SetHeading(strings.Get(41200));
  // Estuary displays the format notes inline.
  const bool estuary = settings->GetString(CSettings::SETTING_LOOKANDFEEL_SKIN) == "skin.estuary";
  dialog->SetUseDetails(!estuary);
  dialog->EnableButton(!estuary, 41320);

  const int summary = LldvUnavailableLabel(status, saved, allowModeSwitch);
  const bool chooseFormat = summary && LldvCanChooseFormat(status, allowModeSwitch);
  std::string summaryText = strings.Get(summary ? 41322 : 41321);
  if (summary)
    summaryText += '\n' + strings.Get(chooseFormat ? LLDV_CHOOSE_FORMAT_LABEL : summary);
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
  if (const unsigned bits = dvbridge_lldv_444_bits(display.sink))
    addDetail(41341, StringUtils::Format("{}-bit", bits));
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
  const auto gamutNote = display.GamutAdjusted() ? strings.Get(41343) : std::string{};
  if (!gamutNote.empty())
    details += "\n\n" + gamutNote;
  dialog->SetProperty("CB1.LldvDetailNote", gamutNote);
  dialog->SetProperty("CB1.LldvDetails", details);
  dialog->SetProperty("CB1.LldvDetailLabels", labels);
  dialog->SetProperty("CB1.LldvDetailValues", values);
  for (int i = 0; i <= static_cast<int>(LldvFormat::Y444_12); ++i)
  {
    const auto format = static_cast<LldvFormat>(i);
    const bool forced = requiresForcing(format);
    const int preference = preferenceFor(format);
    const int reason = LldvUnavailableLabel(status, preference, allowModeSwitch);
    const bool disabled = reason != 0 &&
        (format != LldvFormat::Auto || forCircuit || status.active != LldvFormat::Auto);
    const auto label = strings.Get(LldvFormatLabel(format, !estuary && forced));
    CFileItem item(disabled ? "[COLOR grey]" + label + "[/COLOR]" : label);
    item.SetProperty("disabled", disabled);
    if (reason)
      item.SetLabel2(strings.Get(reason));
    else if (i == 0)
      item.SetLabel2(preferred == LldvFormat::Auto ? strings.Get(41213)
          : strings.Get(LldvFormatLabel(preferred, !estuary && requiresForcing(preferred))));
    else if (preference == LLDV_PACKED_422_12)
      item.SetLabel2(strings.Get(41342));
    else if (format == status.active)
      item.SetLabel2(strings.Get(status.activeForced && !requiresForcing(format) ? 41218 : 41214));
    else
      item.SetLabel2(strings.Get(format == LldvFormat::Y422_10 ? 41216 : 41215));
    if (estuary)
    {
      if (disabled)
        item.SetProperty("CB1.LldvFocusNote", strings.Get(reason == 41210 ? 41332 : reason));
      else if (i == 0 || preference == LLDV_PACKED_422_12)
        item.SetProperty("CB1.LldvNote", item.GetLabel2());
      else if (forced)
        item.SetProperty("CB1.LldvNote", strings.Get(41331));
    }
    dialog->Add(item);
  }
  const auto selected = LldvFormatPreference(saved);
  dialog->SetSelected(selected <= LldvFormat::Y444_12 ? static_cast<int>(selected) : 0);
  dialog->Open();
  for (const char* key : {"CB1.LldvStatus", "CB1.LldvDetails", "CB1.LldvDetailLabels",
                          "CB1.LldvDetailValues", "CB1.LldvDetailNote"})
    dialog->SetProperty(key, CVariant{});
  const auto current = ReadLldvConnector();
  const int choice = dialog->GetSelectedItem();
  if (!dialog->IsConfirmed() || choice < 0 || choice > static_cast<int>(LldvFormat::Y444_12))
    return false;
  const auto format = static_cast<LldvFormat>(choice);
  const int preference = preferenceFor(format);
  if ((current.connectorId != status.connectorId || current.edidGeneration != status.edidGeneration) ||
      ((format != LldvFormat::Auto || forCircuit || current.active != LldvFormat::Auto) &&
       LldvUnavailableLabel(current, preference, !inPlayback && current.active == LldvFormat::Auto)) ||
      !settings->SetInt("dvbridge.lldvformatvalue", preference))
    return false;
  settings->Save();
  return true;
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
  return strings.Get(LldvFormatLabel(format, SelectLldvTransport(status, saved, true).forced));
}

std::string CDVPlaybackModes::SDRGamutLabel()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int gamut = settings->GetInt("dvbridge.sdrgamutvalue");
  return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(
      gamut == 1 ? (!SDRBT2020Unavailable() ? 41273 : 41272) : gamut == 2 ? 41272 : 40984);
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
void DVBRIDGE::PlaybackNotification::EmitLldvRateFallback(double fps)
{
  const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const auto rate = LldvFrameRateLabel(fps);
  CLog::Log(LOGINFO, "CB1 fallback: LLDV unavailable at {} fps, HDR10 presentation accepted", rate);
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, strings.Get(40863),
      StringUtils::Format(strings.Get(41505), rate), 3000, false);
}
void DVBRIDGE::PlaybackNotification::EmitAI(int mode)
{
  if (mode!=1 && mode!=4) return;
  const int label=mode==1 ? 40867 : 40868;
  Emit(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(label));
}
#endif
