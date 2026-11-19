/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SettingConditions.h"

#include "GUIPassword.h"
#include "LockMode.h"
#include "ServiceBroker.h"
#include "addons/AddonManager.h"
#include "addons/Skin.h"
#include "addons/addoninfo/AddonType.h"
#include "application/AppParams.h"
#include "cores/AudioEngine/Engines/ActiveAE/ActiveAESettings.h"
#if defined(HAS_WEB_SERVER)
#include "network/WebServer.h"
#endif
#include "peripherals/Peripherals.h"
#include "profiles/ProfileManager.h"
#include "settings/SettingAddon.h"
#include "settings/SettingsComponent.h"
#include "utils/FontUtils.h"
#include "utils/StringUtils.h"
#include "windowing/WinSystem.h"
#ifdef HAVE_DVBRIDGE
#include "cores/DataCacheCore.h"
#include "dialogs/DVPlaybackModes.h"
#include "cores/VideoPlayer/Interface/StreamInfo.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSelection.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "windowing/gbm/WinSystemGbmGLESContext.h"
#endif

namespace
{
bool AddonHasSettings(const std::string& condition,
                      const std::string& value,
                      const SettingConstPtr& setting)
{
  if (!setting)
    return false;

  std::shared_ptr<const CSettingAddon> settingAddon = std::dynamic_pointer_cast<const CSettingAddon>(setting);
  if (!settingAddon)
    return false;

  ADDON::AddonPtr addon;
  if (!CServiceBroker::GetAddonMgr().GetAddon(settingAddon->GetValue(), addon,
                                              settingAddon->GetAddonType(),
                                              ADDON::OnlyEnabled::CHOICE_YES) ||
      !addon)
    return false;

  if (addon->Type() == ADDON::AddonType::SKIN)
    return ((ADDON::CSkinInfo*)addon.get())->HasSkinFile("SkinSettings.xml");

  return addon->CanHaveAddonOrInstanceSettings();
}

bool CheckMasterLock(const std::string& /*condition*/,
                     const std::string& value,
                     const SettingConstPtr& /*setting*/)
{
  return g_passwordManager.IsMasterLockUnlocked(StringUtils::EqualsNoCase(value, "true"));
}

bool HasPeripherals(const std::string& /*condition*/,
                    const std::string& /*value*/,
                    const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetPeripherals().GetNumberOfPeripherals() > 0;
}

bool HasPeripheralLibraries(const std::string& /*condition*/,
                            const std::string& /*value*/,
                            const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetAddonMgr().HasInstalledAddons(ADDON::AddonType::PERIPHERALDLL);
}

bool HasRumbleFeature(const std::string& /*condition*/,
                      const std::string& /*value*/,
                      const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetPeripherals().SupportsFeature(PERIPHERALS::FEATURE_RUMBLE);
}

bool HasRumbleController(const std::string& /*condition*/,
                         const std::string& /*value*/,
                         const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetPeripherals().HasPeripheralWithFeature(PERIPHERALS::FEATURE_RUMBLE);
}

bool HasPowerOffFeature(const std::string& /*condition*/,
                        const std::string& /*value*/,
                        const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetPeripherals().SupportsFeature(PERIPHERALS::FEATURE_POWER_OFF);
}

bool HasSystemSdrPeakLuminance(const std::string& /*condition*/,
                               const std::string& /*value*/,
                               const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->HasSystemSdrPeakLuminance();
}

bool HasMultipleMonitors(const std::string& /*condition*/,
                         const std::string& /*value*/,
                         const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->GetPhysicalOutputCount() > 1;
}

bool SupportsVideoSuperResolution(const std::string& /*condition*/,
                                  const std::string& /*value*/,
                                  const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->SupportsVideoSuperResolution();
}

bool SupportsDolbyVision(const std::string& /*condition*/,
                         const std::string& /*value*/,
                         const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities().SupportsDolbyVision();
}

bool SupportsScreenMove(const std::string& /*condition*/,
                        const std::string& /*value*/,
                        const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->SupportsScreenMove();
}

bool IsHDRDisplay(const std::string& /*condition*/,
                  const std::string& /*value*/,
                  const SettingConstPtr& /*setting*/)
{
  return CServiceBroker::GetWinSystem()->IsHDRDisplay();
}

bool IsMasterUser(const std::string& /*condition*/,
                  const std::string& /*value*/,
                  const SettingConstPtr& /*setting*/)
{
  return g_passwordManager.bMasterUser;
}

bool HasSubtitlesFontExtensions(const std::string& /*condition*/,
                                const std::string& /*value*/,
                                const SettingConstPtr& setting)
{
  const auto settingStr = std::dynamic_pointer_cast<const CSettingString>(setting);
  if (!settingStr)
    return false;

  return KODI::UTILS::FONT::IsSupportedFontExtension(settingStr->GetValue());
}

bool ProfileCanWriteDatabase(const std::string& /*condition*/,
                             const std::string& /*value*/,
                             const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().canWriteDatabases();
}

bool ProfileCanWriteSources(const std::string& /*condition*/,
                            const std::string& /*value*/,
                            const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().canWriteSources();
}

bool ProfileHasAddons(const std::string& /*condition*/,
                      const std::string& /*value*/,
                      const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().hasAddons();
}

bool ProfileHasDatabase(const std::string& /*condition*/,
                        const std::string& /*value*/,
                        const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().hasDatabases();
}

bool ProfileHasSources(const std::string& /*condition*/,
                       const std::string& /*value*/,
                       const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().hasSources();
}

bool ProfileHasAddonManagerLocked(const std::string& /*condition*/,
                                  const std::string& /*value*/,
                                  const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().addonmanagerLocked();
}

bool ProfileHasFilesLocked(const std::string& /*condition*/,
                           const std::string& /*value*/,
                           const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().filesLocked();
}

bool ProfileHasMusicLocked(const std::string& /*condition*/,
                           const std::string& /*value*/,
                           const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().musicLocked();
}

bool ProfileHasPicturesLocked(const std::string& /*condition*/,
                              const std::string& /*value*/,
                              const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().picturesLocked();
}

bool ProfileHasProgramsLocked(const std::string& /*condition*/,
                              const std::string& /*value*/,
                              const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().programsLocked();
}

bool ProfileHasSettingsLocked(const std::string& /*condition*/,
                              const std::string& value,
                              const SettingConstPtr& /*setting*/)
{
  SettingsLock slValue = SettingsLock::ALL;
  if (StringUtils::EqualsNoCase(value, "none"))
    slValue = SettingsLock::NONE;
  else if (StringUtils::EqualsNoCase(value, "standard"))
    slValue = SettingsLock::STANDARD;
  else if (StringUtils::EqualsNoCase(value, "advanced"))
    slValue = SettingsLock::ADVANCED;
  else if (StringUtils::EqualsNoCase(value, "expert"))
    slValue = SettingsLock::EXPERT;
  return slValue <= CSettingConditions::GetCurrentProfile().settingsLockLevel();
}

bool ProfileHasVideosLocked(const std::string& /*condition*/,
                            const std::string& /*value*/,
                            const SettingConstPtr& /*setting*/)
{
  return CSettingConditions::GetCurrentProfile().videoLocked();
}

bool ProfileLockMode(const std::string& /*condition*/,
                     const std::string& value,
                     const SettingConstPtr& /*setting*/)
{
  char* tmp = nullptr;
  const auto lockMode = static_cast<LockMode>(std::strtol(value.c_str(), &tmp, 0));
  if (tmp && *tmp != '\0')
    return false;

  return CSettingConditions::GetCurrentProfile().getLockMode() == lockMode;
}

bool GreaterThan(const std::string& /*condition*/,
                 const std::string& value,
                 const SettingConstPtr& setting)
{
  if (!setting)
    return false;

  std::shared_ptr<const CSettingInt> settingInt = std::dynamic_pointer_cast<const CSettingInt>(setting);
  if (!settingInt)
    return false;

  char* tmp = nullptr;

  int lhs = settingInt->GetValue();
  int rhs =
      StringUtils::IsInteger(value) ? static_cast<int>(std::strtol(value.c_str(), &tmp, 0)) : 0;

  return lhs > rhs;
}

bool GreaterThanOrEqual(const std::string& /*condition*/,
                        const std::string& value,
                        const SettingConstPtr& setting)
{
  if (!setting)
    return false;

  std::shared_ptr<const CSettingInt> settingInt = std::dynamic_pointer_cast<const CSettingInt>(setting);
  if (!settingInt)
    return false;

  char* tmp = nullptr;

  int lhs = settingInt->GetValue();
  int rhs =
      StringUtils::IsInteger(value) ? static_cast<int>(std::strtol(value.c_str(), &tmp, 0)) : 0;

  return lhs >= rhs;
}

bool LessThan(const std::string& /*condition*/,
              const std::string& value,
              const SettingConstPtr& setting)
{
  if (!setting)
    return false;

  std::shared_ptr<const CSettingInt> settingInt = std::dynamic_pointer_cast<const CSettingInt>(setting);
  if (!settingInt)
    return false;

  char* tmp = nullptr;

  int lhs = settingInt->GetValue();
  int rhs =
      StringUtils::IsInteger(value) ? static_cast<int>(std::strtol(value.c_str(), &tmp, 0)) : 0;

  return lhs < rhs;
}

bool LessThanOrEqual(const std::string& /*condition*/,
                     const std::string& value,
                     const SettingConstPtr& setting)
{
  if (!setting)
    return false;

  std::shared_ptr<const CSettingInt> settingInt = std::dynamic_pointer_cast<const CSettingInt>(setting);
  if (!settingInt)
    return false;

  char* tmp = nullptr;

  int lhs = settingInt->GetValue();
  int rhs =
      StringUtils::IsInteger(value) ? static_cast<int>(std::strtol(value.c_str(), &tmp, 0)) : 0;

  return lhs <= rhs;
}
}; // anonymous namespace

const CProfileManager* CSettingConditions::m_profileManager = nullptr;
CSettingConditions::SimpleConditions CSettingConditions::m_simpleConditions;
CSettingConditions::ComplexConditions CSettingConditions::m_complexConditions;

void CSettingConditions::Initialize()
{
  if (!m_simpleConditions.empty())
    return;

  // add simple conditions
  m_simpleConditions.emplace("true");
#ifdef HAS_UPNP
  m_simpleConditions.emplace("has_upnp");
#endif
#ifdef HAS_AIRPLAY
  m_simpleConditions.emplace("has_airplay");
#endif
#ifdef HAVE_X11
  m_simpleConditions.emplace("have_x11");
#endif
#ifdef HAVE_WAYLAND
  m_simpleConditions.emplace("have_wayland");
#endif
#ifdef HAVE_GBM
  m_simpleConditions.emplace("have_gbm");
#endif
#ifdef HAVE_DVBRIDGE
  m_simpleConditions.emplace("have_dvbridge");
#endif
#ifdef CB1_HAS_HDR10_AI
  m_simpleConditions.emplace("have_cb1_hdr10_ai");
#endif
#ifdef HAS_GL
  m_simpleConditions.emplace("has_gl");
#endif
#ifdef HAS_GLES
  m_simpleConditions.emplace("has_gles");
#endif
#if HAS_GLES >= 2
  m_simpleConditions.emplace("has_glesv2");
#endif
#ifdef HAS_TIME_SERVER
  m_simpleConditions.emplace("has_time_server");
#endif
#ifdef HAS_WEB_SERVER
  m_simpleConditions.emplace("has_web_server");
#endif
#ifdef HAS_FILESYSTEM_SMB
  m_simpleConditions.emplace("has_filesystem_smb");
#endif
#ifdef HAS_FILESYSTEM_NFS
  m_simpleConditions.emplace("has_filesystem_nfs");
#endif
#ifdef HAS_ZEROCONF
  m_simpleConditions.emplace("has_zeroconf");
#endif
#ifdef HAVE_LIBVA
  m_simpleConditions.emplace("have_libva");
#endif
#ifdef HAVE_LIBVDPAU
  m_simpleConditions.emplace("have_libvdpau");
#endif
#ifdef TARGET_ANDROID
  m_simpleConditions.emplace("has_mediacodec");
#endif
#ifdef TARGET_DARWIN
  m_simpleConditions.emplace("HasVTB");
#endif
#ifdef TARGET_DARWIN_OSX
  m_simpleConditions.emplace("have_osx");
#endif
#ifdef TARGET_DARWIN_IOS
  m_simpleConditions.emplace("have_ios");
#endif
#ifdef TARGET_DARWIN_TVOS
  m_simpleConditions.emplace("have_tvos");
#endif
#if defined(TARGET_WINDOWS)
  m_simpleConditions.emplace("has_dx");
  m_simpleConditions.emplace("hasdxva2");
#endif
#if defined(TARGET_WEBOS)
  m_simpleConditions.emplace("have_webos");
#endif

#ifdef HAVE_LCMS2
  m_simpleConditions.emplace("have_lcms2");
#endif

#ifdef TARGET_ANDROID
  m_simpleConditions.emplace("isstandalone");
#else
  if (CServiceBroker::GetAppParams()->IsStandAlone())
    m_simpleConditions.emplace("isstandalone");
#endif

  m_simpleConditions.emplace("has_ae_quality_levels");

#ifdef HAS_WEB_SERVER
  if (CWebServer::WebServerSupportsSSL())
    m_simpleConditions.emplace("webserver_has_ssl");
#endif

#ifdef HAVE_LIBBLURAY
  m_simpleConditions.emplace("have_libbluray");
#endif

#ifdef HAS_CDDA_RIPPER
  m_simpleConditions.emplace("has_cdda_ripper");
#endif

#ifdef HAS_OPTICAL_DRIVE
  m_simpleConditions.emplace("has_optical_drive");
#endif

  // add complex conditions

  m_complexConditions.try_emplace("addonhassettings", AddonHasSettings);
  m_complexConditions.try_emplace("checkmasterlock", CheckMasterLock);
  m_complexConditions.try_emplace("hasperipherals", HasPeripherals);
  m_complexConditions.try_emplace("hasperipherallibraries", HasPeripheralLibraries);
  m_complexConditions.try_emplace("hasrumblefeature", HasRumbleFeature);
  m_complexConditions.try_emplace("hasrumblecontroller", HasRumbleController);
  m_complexConditions.try_emplace("haspowerofffeature", HasPowerOffFeature);
  m_complexConditions.try_emplace("hassystemsdrpeakluminance", HasSystemSdrPeakLuminance);
  m_complexConditions.try_emplace("hasmultiplemonitors", HasMultipleMonitors);
  m_complexConditions.try_emplace("supportsscreenmove", SupportsScreenMove);
  m_complexConditions.try_emplace("supportsvideosuperresolution", SupportsVideoSuperResolution);
  m_complexConditions.try_emplace("supportsdolbyvision", SupportsDolbyVision);
  m_complexConditions.try_emplace("ishdrdisplay", IsHDRDisplay);
  m_complexConditions.try_emplace("ismasteruser", IsMasterUser);
  m_complexConditions.try_emplace("hassubtitlesfontextensions", HasSubtitlesFontExtensions);
  m_complexConditions.try_emplace("profilecanwritedatabase", ProfileCanWriteDatabase);
  m_complexConditions.try_emplace("profilecanwritesources", ProfileCanWriteSources);
  m_complexConditions.try_emplace("profilehasaddons", ProfileHasAddons);
  m_complexConditions.try_emplace("profilehasdatabase", ProfileHasDatabase);
  m_complexConditions.try_emplace("profilehassources", ProfileHasSources);
  m_complexConditions.try_emplace("profilehasaddonmanagerlocked", ProfileHasAddonManagerLocked);
  m_complexConditions.try_emplace("profilehasfileslocked", ProfileHasFilesLocked);
  m_complexConditions.try_emplace("profilehasmusiclocked", ProfileHasMusicLocked);
  m_complexConditions.try_emplace("profilehaspictureslocked", ProfileHasPicturesLocked);
  m_complexConditions.try_emplace("profilehasprogramslocked", ProfileHasProgramsLocked);
  m_complexConditions.try_emplace("profilehassettingslocked", ProfileHasSettingsLocked);
  m_complexConditions.try_emplace("profilehasvideoslocked", ProfileHasVideosLocked);
  m_complexConditions.try_emplace("profilelockmode", ProfileLockMode);
  m_complexConditions.try_emplace("aesettingvisible",
                                  ActiveAE::CActiveAESettings::IsSettingVisible);
  m_complexConditions.try_emplace("gt", GreaterThan);
  m_complexConditions.try_emplace("gte", GreaterThanOrEqual);
  m_complexConditions.try_emplace("lt", LessThan);
  m_complexConditions.try_emplace("lte", LessThanOrEqual);
}

#ifdef HAVE_DVBRIDGE
void CSettingConditions::DVOutputOptions(const std::shared_ptr<const CSetting>& setting,
                                         std::vector<IntegerSettingOption>& list, bool inPlayback)
{
  // Availability never substitutes or saves a different requested preference.
  const bool menu=setting->GetId()=="dvbridge.playbackchoice";
  const bool playback=setting->GetId()=="dvbridge.playback";
  const bool hlgMenu=setting->GetId()=="dvbridge.hlgplaybackchoice";
  const bool hlgSource=hlgMenu || setting->GetId()=="dvbridge.hlgsource";
  const bool hdr10PlusMenu=setting->GetId()=="dvbridge.hdr10plusplaybackchoice";
  const bool hdr10Menu=hdr10PlusMenu || setting->GetId()=="dvbridge.hdr10playbackchoice";
  auto preferences = DVBRIDGE::FrameOutputSettings();
  if (!hdr10Menu && !hlgMenu && !menu)
  {
    std::lock_guard lock(DVBRIDGE::OutputSettingsMutex());
    DVBRIDGE::ReadDefaultOutputSelectors().Apply(preferences);
  }
  const auto data=DVBRIDGE::StreamTelemetry::Read();
  auto caps=data.transaction.capabilities;
  auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());
  const auto available=window ? window->GetDVOutputCapabilities(nullptr, true) : DVBRIDGE::OutputCapabilities{};
  // Global preferences use hardware capabilities, not the current or previous film.
  if ((!menu && !hdr10Menu && !hlgMenu) ||
      (!caps.sourceKnown && !data.transaction.identity.Valid()))
    caps=available;
  // A failed frame or disabled import does not remove reusable output capabilities.
  caps.nativeDV=available.nativeDV;
  caps.hdr10=available.hdr10;
  caps.hdr10MaxBpc=available.hdr10MaxBpc;
  caps.hdr10plus=available.hdr10plus;
  caps.playerLed = available.playerLed;
  caps.lldvUnavailableLabel = available.lldvUnavailableLabel;
  if (window && menu && inPlayback)
  {
    const auto lldv = window->GetDVOutputCapabilities(nullptr, false);
    caps.playerLed = lldv.playerLed;
    caps.lldvUnavailableLabel = lldv.lldvUnavailableLabel;
    if ((caps.playerLed || caps.lldvUnavailableLabel == DVBRIDGE::LLDV_CHOOSE_FORMAT_LABEL) &&
        !window->LldvSupportsPlaybackTiming(CServiceBroker::GetDataCacheCore().GetVideoFps(), true))
    {
      caps.playerLed = false;
      caps.lldvUnavailableLabel = 41212;
    }
  }
  if (caps.isDV && preferences.output==DVBRIDGE::OutputMode::Disabled)
  {
    caps.processingKnown=available.processingKnown;
    caps.processing=available.processing;
  }
  const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
  const bool method=setting->GetId()=="dvbridge.hdr10method";
  const bool hdr10PlusSource=hdr10PlusMenu || setting->GetId()=="dvbridge.hdr10plussource";
  const bool hdr10Source=hdr10Menu || hdr10PlusSource || setting->GetId()=="dvbridge.hdr10source";
  const bool nativeHDR10Plus=hdr10PlusMenu;
  // A preference can be deferred while paused; only the renderer admits a frame.
  const bool pendingHDR10Plus=nativeHDR10Plus && !caps.sourceKnown && !data.transaction.identity.Valid();
  const auto& processing=(hdr10Source || hlgSource) && !caps.sourceKnown ? caps.hdr10Processing : caps.processing;
  constexpr int menuModes[]{5,1,
      4,0,2,3,6,7,8};
#ifdef CB1_HAS_HDR10_AI
  constexpr int hdr10MenuModes[]{1,4,0,7};
  constexpr int hdr10PlusMenuModes[]{1,0,0,7};
#else
  constexpr int hdr10MenuModes[]{0};
  constexpr int hdr10PlusMenuModes[]{0,0};
#endif
#ifdef CB1_HAS_HDR10_AI
  constexpr int nativeHDR10PlusMenuIndex = 1;
#else
  constexpr int nativeHDR10PlusMenuIndex = 0;
#endif
  const int nativeHDR10PlusChoice = hdr10PlusMenu ? nativeHDR10PlusMenuIndex :
      static_cast<int>(DVBRIDGE::HDR10SourceMode::HDR10PlusPassthrough);
  for(auto& option : list)
  {
    const int mode=hlgSource ? (option.value==2 ? DVBRIDGE_MODE_SDR :
        (hlgMenu ? option.value==0 : option.value==1) ? DVBRIDGE_MODE_STANDARD : DVBRIDGE_MODE_DISABLED) : hdr10PlusMenu ? (option.value>=0 && option.value<int(std::size(hdr10PlusMenuModes)) ? hdr10PlusMenuModes[option.value] : 0) :
        hdr10Menu ? (option.value>=0 && option.value<int(std::size(hdr10MenuModes)) ? hdr10MenuModes[option.value] : 0) :
        hdr10Source ? (option.value==1 || option.value==3 ? 1 : option.value==2 ? 4 : option.value==4 ? 7 : 0) :
        (menu || playback) ? (option.value>=0 && option.value<int(std::size(menuModes)) ? menuModes[option.value] : 0) :
        method ? (option.value==0 ? 2 : option.value==1 ? 3 : option.value==2 ? 6 : 0) :
        (option.value==0 ? 1 : option.value==1 ? 2 : option.value==3 || option.value==5 ? 4 : option.value==4 ? 5 : option.value==6 ? 7 : option.value==7 ? 8 : 0);
    auto reason=DVBRIDGE::OutputReason::None;
    const bool guided = hdr10PlusSource && (hdr10PlusMenu ? option.value==0 : option.value==3);
    if (hdr10PlusSource && option.value == nativeHDR10PlusChoice && !caps.hdr10plus)
      reason=DVBRIDGE::OutputReason::NoHDR10PlusTransport;
    else if (mode)
    {
      const bool dv=mode==1 || mode==4 || mode==5;
      if (guided && hdr10Menu && !(pendingHDR10Plus || (caps.sourceKnown && caps.isHDR10Plus)))
        reason=DVBRIDGE::OutputReason::SourceIneligible;
      else if (hdr10Source && (nativeHDR10Plus || caps.hdr10PlusSource) && !guided && mode!=7)
        reason=DVBRIDGE::OutputReason::SourceIneligible;
      else if ((mode==2 || mode==3) && caps.HDR10DepthLimited())
        reason=DVBRIDGE::OutputReason::HDR10LinkDepthTooLow;
      else if (mode==8 && !caps.playerLed) reason=DVBRIDGE::OutputReason::NoPlayerLedTransport;
      else if (mode!=7 && mode!=8 && !(dv ? caps.nativeDV : caps.hdr10))
        reason=dv ? DVBRIDGE::OutputReason::NoDVTransport : DVBRIDGE::OutputReason::NoHDR10Transport;
      else if (mode==6 && !caps.hdr10plus) reason=DVBRIDGE::OutputReason::NoHDR10PlusTransport;
      else if (!caps.processingKnown) reason=DVBRIDGE::OutputReason::ProcessingPending;
      else if (caps.sourceKnown && !(hlgSource ? (caps.isHLG && !caps.isDV) : hdr10Source ? (caps.isHDR10 && !caps.isDV) : caps.isDV))
        reason=DVBRIDGE::OutputReason::SourceIneligible;
      else if (!processing[mode].usable)
        reason=processing[mode].reason==DVBRIDGE::OutputReason::None ?
            DVBRIDGE::OutputReason::NoGPUProcessing : processing[mode].reason;
      else if ((mode==3 || mode==4 || mode==7) && !preferences.ProfileValid(static_cast<dvbridge_mode>(mode)))
        reason=mode==7 ? DVBRIDGE::OutputReason::SDRTargetInvalid : DVBRIDGE::OutputReason::TVProfileRequired;
    }
    int reasonId=CDVPlaybackInfo::ReasonLabel(reason);
    if (reason==DVBRIDGE::OutputReason::NoPlayerLedTransport && caps.lldvUnavailableLabel)
      reasonId=caps.lldvUnavailableLabel;
    if (reason==DVBRIDGE::OutputReason::TVProfileRequired)
    {
      const bool expert=mode==3;
      const double peak=expert?preferences.expertPeak:preferences.peak;
      const int gamut=expert?preferences.expertGamut:preferences.gamut;
      reasonId=!std::isfinite(peak) || peak<(expert?50:0.000001) || peak>10000 ? 40911 :
          gamut<1 || gamut>3 ? 40912 : 40845;
    }
    const bool profile=reason==DVBRIDGE::OutputReason::TVProfileRequired || reason==DVBRIDGE::OutputReason::SDRTargetInvalid;
    const bool chooseLldv = reasonId == DVBRIDGE::LLDV_CHOOSE_FORMAT_LABEL &&
        (!caps.sourceKnown || caps.isDV) && processing[DVBRIDGE_MODE_PLAYER_LED].usable;
    if (reasonId && !profile && !chooseLldv && reason!=DVBRIDGE::OutputReason::ProcessingPending)
      option.label+=" / "+localizer.Get(40851);
    option.label2=reasonId ? localizer.Get(reasonId) :
        guided ? localizer.Get(40971) : mode==3 ? localizer.Get(40864) : "";
    // Settings must allow selection before the required profile controls appear.
    const bool configure=profile && !menu && !hdr10Menu && !hdr10PlusMenu && !hlgMenu;
    option.properties.emplace_back("disabled",reasonId!=0 && !configure && !chooseLldv);
    if (chooseLldv)
      option.properties.emplace_back("cb1ChooseLldv", true);
  }
}
#endif

void CSettingConditions::Deinitialize()
{
  m_profileManager = nullptr;
}

const CProfile& CSettingConditions::GetCurrentProfile()
{
  if (!m_profileManager)
    m_profileManager = CServiceBroker::GetSettingsComponent()->GetProfileManager().get();

  if (m_profileManager)
    return m_profileManager->GetCurrentProfile();

  static CProfile emptyProfile;
  return emptyProfile;
}

bool CSettingConditions::Check(const std::string& condition,
                               const std::string& value /* = "" */,
                               const SettingConstPtr& setting /* = nullptr */)
{
  if (m_simpleConditions.contains(condition))
    return true;

  const auto itCondition = m_complexConditions.find(condition);
  if (itCondition != m_complexConditions.end())
    return itCondition->second(condition, value, setting);

  return Check(condition);
}
