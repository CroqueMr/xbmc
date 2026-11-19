/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbm.h"

#include "GBMDPMSSupport.h"
#include "OptionalsReg.h"
#include "ServiceBroker.h"
#include "VideoSyncGbm.h"

#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "drm/DRMAtomic.h"
#ifdef HAVE_DVBRIDGE
#include "drm/DVBridgeState.h"
#include "utils/DVBridgeDiagnosticLog.h"
#endif
#include "drm/DRMLegacy.h"
#include "drm/OffScreenModeSetting.h"
#include "messaging/ApplicationMessenger.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "rendering/RenderSystem.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "utils/DRMHelpers.h"
#include "utils/DisplayInfo.h"
#include "utils/HDRUtils.h"
#include "utils/Map.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include <algorithm>
#include <array>
#include <vector>
#include <mutex>
#include <cmath>
#include <string.h>

#ifndef HAVE_HDR_OUTPUT_METADATA
// HDR structs is copied from linux include/linux/hdmi.h
struct hdr_metadata_infoframe
{
  uint8_t eotf;
  uint8_t metadata_type;
  struct
  {
    uint16_t x, y;
  } display_primaries[3];
  struct
  {
    uint16_t x, y;
  } white_point;
  uint16_t max_display_mastering_luminance;
  uint16_t min_display_mastering_luminance;
  uint16_t max_cll;
  uint16_t max_fall;
};
struct hdr_output_metadata
{
  uint32_t metadata_type;
  union
  {
    struct hdr_metadata_infoframe hdmi_metadata_type1;
  };
};
#endif

using namespace KODI::WINDOWING::GBM;

using namespace std::chrono_literals;

namespace
{

constexpr const char* BROADCAST_RGB = "Broadcast RGB";
constexpr const char* BROADCAST_RGB_AUTOMATIC = "Automatic";
constexpr const char* BROADCAST_RGB_FULL = "Full";
constexpr const char* BROADCAST_RGB_LIMITED = "Limited 16:235";

constexpr const char* COLOR_FORMAT = "color format";

constexpr const char* MAX_BPC = "max bpc";
// indexed by the "Maximum bit depth" setting, 0 is Auto
constexpr std::array<uint64_t, 4> MAX_BPC_VALUES{0, 8, 10, 12};

// HDMI 2.0 carries at most 600 MHz of TMDS characters: the pixel clock times bits per component
// / 8 for RGB and YCbCr 4:4:4, half of that for 4:2:0, and as is for 4:2:2 (24 bit container).
// 4K 50/60 Hz (594 MHz) fits as 8 bit RGB, but needs 742 MHz as 10 bit.
constexpr uint64_t HDMI_MAX_TMDS_KHZ = 600000;
// YCbCr 4:2:0 over TMDS carries twice the pixel clock, so an HDMI 2.0 source also lists 4:2:0
// modes up to 1.2 GHz (e.g. 4K120 at 1188 MHz). Only a mode above that can be FRL for sure.
constexpr uint64_t HDMI_FRL_PROOF_CLOCK_KHZ = 2 * HDMI_MAX_TMDS_KHZ;
// Max_FRL_Rate code 3 (6 Gbit/s x 4 lanes) is the lowest that carries 4K 60 Hz 10 bit RGB
constexpr int HDMI_FRL_MIN_RATE_RGB_10BIT = 3;
// the kernel's enum names of "color format", indexed by ColorFormat
constexpr std::array<const char*, 5> COLOR_FORMAT_NAMES{"AUTO", "RGB", "YUV 4:4:4", "YUV 4:2:2",
                                                        "YUV 4:2:0"};

// These map to the definitions in the linux kernel
// drivers/gpu/drm/drm_connector.c

constexpr auto ColorimetryMap = make_map<KODI::UTILS::Colorimetry, std::string_view>({
    {KODI::UTILS::Colorimetry::DEFAULT, "Default"},
    {KODI::UTILS::Colorimetry::SMPTE_170M_YCC, "SMPTE_170M_YCC"},
    {KODI::UTILS::Colorimetry::BT709_YCC, "BT709_YCC"},
    {KODI::UTILS::Colorimetry::XVYCC_601, "XVYCC_601"},
    {KODI::UTILS::Colorimetry::XVYCC_709, "XVYCC_709"},
    {KODI::UTILS::Colorimetry::SYCC_601, "SYCC_601"},
    {KODI::UTILS::Colorimetry::OPYCC_601, "opYCC_601"},
    {KODI::UTILS::Colorimetry::OPRGB, "opRGB"},
    {KODI::UTILS::Colorimetry::BT2020_CYCC, "BT2020_CYCC"},
    {KODI::UTILS::Colorimetry::BT2020_RGB, "BT2020_RGB"},
    {KODI::UTILS::Colorimetry::BT2020_YCC, "BT2020_YCC"},
    {KODI::UTILS::Colorimetry::DCI_P3_RGB_D65, "DCI-P3_RGB_D65"},
    {KODI::UTILS::Colorimetry::DCI_P3_RGB_THEATER, "DCI-P3_RGB_Theater"},
    {KODI::UTILS::Colorimetry::RGB_WIDE_FIXED, "RGB_WIDE_FIXED"},
    {KODI::UTILS::Colorimetry::RGB_WIDE_FLOAT, "RGB_WIDE_FLOAT"},
    {KODI::UTILS::Colorimetry::BT601_YCC, "BT601_YCC"},
    {KODI::UTILS::Colorimetry::ST2113_RGB, "Default"},
    {KODI::UTILS::Colorimetry::ICTCP, "Default"},
});
} // namespace

CWinSystemGbm::CWinSystemGbm()
  : m_DRM(nullptr),
    m_GBM(std::make_unique<CGBMUtils>()),
    m_libinput(std::make_unique<CLibInputHandler>())
{
  m_dpms = std::make_shared<CGBMDPMSSupport>();
  m_libinput->Start();
  CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager()->RegisterCallback(
      this, {SETTING_VIDEOSCREEN_COLORRANGE, SETTING_VIDEOSCREEN_COLORFORMAT,
             SETTING_VIDEOSCREEN_MAXBPC});
}

CWinSystemGbm::~CWinSystemGbm()
{
  CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager()->UnregisterCallback(this);
}

const std::string CWinSystemGbm::GetName()
{
  const auto* gui = m_DRM->GetGuiPlane();
  const auto* video = m_DRM->GetVideoPlane();

  std::string name = "gbm (";
  if (gui)
    name += DRMHELPERS::FourCCToString(gui->GetFormat());
  if (gui && video)
    name += " / ";
  if (video)
    name += DRMHELPERS::FourCCToString(video->GetFormat());
  name += ")";
  return name;
}

bool CWinSystemGbm::InitWindowSystem()
{
  const char* x11 = getenv("DISPLAY");
  const char* wayland = getenv("WAYLAND_DISPLAY");
  if (x11 || wayland)
  {
    CLog::Log(LOGDEBUG, "CWinSystemGbm::{} - not allowed to run GBM under a window manager",
              __FUNCTION__);
    return false;
  }

  m_DRM = std::make_shared<CDRMAtomic>();

  if (!m_DRM->InitDrm())
  {
    CLog::Log(LOGERROR, "CWinSystemGbm::{} - failed to initialize Atomic DRM", __FUNCTION__);
    m_DRM.reset();

    // Legacy DRM is only for drivers with no atomic modesetting support. An
    // atomic-capable driver that failed to init (e.g. no connector ready yet
    // at cold boot) must never be demoted to legacy; go headless instead.
    if (!CDRMAtomic::SupportsAtomicModesetting())
    {
      m_DRM = std::make_shared<CDRMLegacy>();
      if (!m_DRM->InitDrm())
      {
        CLog::Log(LOGERROR, "CWinSystemGbm::{} - failed to initialize Legacy DRM", __FUNCTION__);
        m_DRM.reset();
      }
    }

    if (!m_DRM)
    {
      m_DRM = std::make_shared<COffScreenModeSetting>();
      if (!m_DRM->InitDrm())
      {
        CLog::Log(LOGERROR, "CWinSystemGbm::{} - failed to initialize off screen DRM",
                  __FUNCTION__);
        m_DRM.reset();
        return false;
      }
    }
  }

  CDRMConnector* connector = m_DRM->GetConnector();
  if (connector)
  {
    std::vector<uint8_t> edid = connector->GetEDID();
#ifdef HAVE_DVBRIDGE
    std::lock_guard lock(m_lldvEdidMutex);
    m_lldvEdid = edid;
    m_lldvDisplay = DVBRIDGE::ReadLldvDisplay(edid.data(), edid.size());
    m_lldvEdidConnector = *connector->GetConnectorId();
    ++m_lldvEdidGeneration;
    m_lldvRejectedTimings = {};
#endif
    if (!edid.empty())
    {
      m_info = UTILS::CDisplayInfo::Create(edid);
      if (m_info) m_displayEdid = edid;
    }
  }

  if (!m_GBM->CreateDevice(m_DRM->GetFileDescriptor()))
  {
    m_GBM.reset();
    m_DRM.reset();
    return false;
  }

  SetColorimetry(nullptr);
  SetHDR(nullptr);

  auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return false;

  auto settings = settingsComponent->GetSettings();
  if (!settings)
    return false;

  MigrateLimitedRangeSetting();
  if (auto rangeSetting = settings->GetSetting(SETTING_VIDEOSCREEN_COLORRANGE))
  {
    m_colorRange = settings->GetInt(SETTING_VIDEOSCREEN_COLORRANGE);
    m_shaderColorRange = m_colorRange.load();
    rangeSetting->SetVisible(true);
    if (auto legacySetting = settings->GetSetting(CSettings::SETTING_VIDEOSCREEN_LIMITEDRANGE))
      legacySetting->SetVisible(false);
  }

  if (auto formatSetting = settings->GetSetting(SETTING_VIDEOSCREEN_COLORFORMAT))
  {
    m_colorFormat = settings->GetInt(SETTING_VIDEOSCREEN_COLORFORMAT);

    const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
    formatSetting->SetVisible(std::dynamic_pointer_cast<CDRMAtomic>(m_DRM) && connector &&
                              connector->SupportsProperty(COLOR_FORMAT));

    if (connector && connector->GetType().rfind("HDMI", 0) == 0)
    {
      CLog::LogF(LOGINFO, "HDMI sink max FRL rate code {}, a mode above {} kHz is listed: {}",
                 connector->GetMaxFrlRate(), HDMI_FRL_PROOF_CLOCK_KHZ,
                 connector->HasModeAboveClock(HDMI_FRL_PROOF_CLOCK_KHZ));
    }
  }

  if (auto bpcSetting = settings->GetSetting(SETTING_VIDEOSCREEN_MAXBPC))
  {
    m_maxBpc = settings->GetInt(SETTING_VIDEOSCREEN_MAXBPC);

    const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
    const bool supported = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM) && connector &&
                           connector->SupportsProperty(MAX_BPC);

    // only once, after a cycle the property holds what Kodi wrote
    if (supported && !m_defaultMaxBpc)
      m_defaultMaxBpc = connector->GetPropertyValue(MAX_BPC);

    bpcSetting->SetVisible(supported);
  }


  auto setting = settings->GetSetting("videoscreen.limitguisize");
  if (setting)
    setting->SetVisible(true);

  SetHDR(nullptr);

  CLog::Log(LOGDEBUG, "CWinSystemGbm::{} - initialized DRM", __FUNCTION__);
  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemGbm::DestroyWindowSystem()
{
  CLog::Log(LOGDEBUG, "CWinSystemGbm::{} - deinitialized DRM", __FUNCTION__);

  m_libinput.reset();

  return true;
}

namespace
{
// Blu-ray PSR23 formats, independent of the player profile or current arrangement.
// Keep this display backend independent of the optional libbluray headers.
uint32_t Bluray3DDisplayCapabilities(const std::vector<RESOLUTION_INFO>& modes)
{
  bool p1080Ntsc = false;
  bool p720Ntsc = false;
  bool p720Pal = false;
  for (const auto& mode : modes)
  {
    if ((mode.dwFlags & D3DPRESENTFLAG_INTERLACED) ||
        !(mode.dwFlags & (D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB)))
      continue;

    const int height = mode.iBlanking > 0
                           ? (mode.iScreenHeight - mode.iBlanking) / 2
                           : mode.iScreenHeight;
    if (mode.iScreenWidth == 1920 && height == 1080)
      p1080Ntsc |= std::abs(mode.fRefreshRate - 24000.0f / 1001.0f) < 0.005f;
    if (mode.iScreenWidth == 1280 && height == 720)
    {
      p720Ntsc |= std::abs(mode.fRefreshRate - 60000.0f / 1001.0f) < 0.005f;
      p720Pal |= std::abs(mode.fRefreshRate - 50.0f) < 0.005f;
    }
  }

  // Bit 0 requires both NTSC formats, bit 1 is 720p50. Physical width is unknown;
  // neither glasses-free nor interlaced capability can be inferred from these modes.
  return (p1080Ntsc && p720Ntsc ? 0x01u : 0u) | (p720Pal ? 0x02u : 0u);
}
} // namespace

void CWinSystemGbm::UpdateResolutions()
{
  RESOLUTION_INFO current = m_DRM->GetCurrentMode();

  auto resolutions = m_DRM->GetModes();
  // Publish from the complete local enumeration, before rebuilding the GUI catalogue.
  // An empty/mono enumeration must also clear a previously advertised 3D capability.
  m_bluray3DDisplayCapabilities.store(Bluray3DDisplayCapabilities(resolutions));
  if (resolutions.empty())
  {
    CDisplaySettings::GetInstance().ClearCustomResolutions();

    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(current);
    CDisplaySettings::GetInstance().AddResolutionInfo(current);
    CDisplaySettings::GetInstance().SetDesktopResolutionInfo(current);

    CLog::Log(LOGINFO, "Found resolution {}x{} with {}x{}{} @ {:f} Hz{}", current.iWidth,
              current.iHeight, current.iScreenWidth, current.iScreenHeight,
              current.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", current.fRefreshRate,
              current.StereoLayoutTag());
  }
  else
  {
    CDisplaySettings::GetInstance().ClearCustomResolutions();

    for (auto& res : resolutions)
    {
      CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
      CDisplaySettings::GetInstance().AddResolutionInfo(res);

      if (current.iScreenWidth == res.iScreenWidth && current.iScreenHeight == res.iScreenHeight &&
          current.iWidth == res.iWidth && current.iHeight == res.iHeight &&
          current.fRefreshRate == res.fRefreshRate && current.dwFlags == res.dwFlags)
      {
        CDisplaySettings::GetInstance().SetDesktopResolutionInfo(res);
      }

      // The layout matters as much as the geometry here: without it a half 3D mode prints
      // exactly like the 2D mode of the same timing, and the log dedupe swallows it as a
      // duplicate message.
      CLog::Log(LOGINFO, "Found resolution {}x{} with {}x{}{} @ {:f} Hz{}", res.iWidth, res.iHeight,
                res.iScreenWidth, res.iScreenHeight,
                res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", res.fRefreshRate,
                res.StereoLayoutTag());
    }
  }

  CDisplaySettings::GetInstance().ApplyCalibrations();

  // Ensure that scrubbed calibrations are always saved, otherwise would depend on calibration GUI
  CDisplaySettings::GetInstance().UpdateCalibrations();
}

void CWinSystemGbm::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting) return;
  if (setting->GetId() == SETTING_VIDEOSCREEN_COLORRANGE)
  {
    m_colorRange = std::static_pointer_cast<const CSettingInt>(setting)->GetValue();
    if (!m_migratingColorRange)
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_CALLBACK, -1, -1, &m_colorRangeCallback);
  }
  else if (setting->GetId() == SETTING_VIDEOSCREEN_COLORFORMAT)
  {
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_CALLBACK, -1, -1, &m_colorFormatCallback);
  }
  else if (setting->GetId() == SETTING_VIDEOSCREEN_MAXBPC)
  {
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_CALLBACK, -1, -1, &m_maxBpcCallback);
  }
}

void CWinSystemGbm::ApplyMaxBpcSetting(void* userptr)
{
  auto winSystem = static_cast<CWinSystemGbm*>(userptr);

  winSystem->m_maxBpc = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      SETTING_VIDEOSCREEN_MAXBPC);
  winSystem->m_maxBpcFallback = false;
  winSystem->m_bpcProbe = false;
  winSystem->ApplyConnectorProperties();
}

uint64_t CWinSystemGbm::GetEffectiveMaxBpc(std::optional<uint64_t> restored) const
{
  const int setting = m_maxBpc;
  if (!m_maxBpcFallback && setting > 0 && setting < static_cast<int>(MAX_BPC_VALUES.size()))
    return MAX_BPC_VALUES[setting];

  if (restored)
    return *restored;
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  return connector ? connector->GetPropertyValue(MAX_BPC).value_or(0) : 0;
}

void CWinSystemGbm::ApplyColorFormatSetting(void* userptr)
{
  auto winSystem = static_cast<CWinSystemGbm*>(userptr);

  winSystem->m_colorFormat = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      SETTING_VIDEOSCREEN_COLORFORMAT);
  // a new choice gets a fresh try, even for a mode where the previous one was refused
  winSystem->m_colorFormatSkip = 0;

  // a format that the connector does not offer would only act as Auto, so show that
  if (!winSystem->IsColorFormatOffered(winSystem->GetColorFormat()))
  {
    CLog::LogF(LOGINFO, "the connector does not offer the chosen colour format, using Auto");
    CServiceBroker::GetSettingsComponent()->GetSettings()->SetInt(
        SETTING_VIDEOSCREEN_COLORFORMAT, static_cast<int>(ColorFormat::AUTO));
  }

  winSystem->ApplyConnectorProperties();
}

bool CWinSystemGbm::IsColorFormatOffered(ColorFormat format) const
{
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;

  // nothing to judge it by, leave the choice alone
  if (!std::dynamic_pointer_cast<CDRMAtomic>(m_DRM) || !connector ||
      !connector->SupportsProperty(COLOR_FORMAT))
    return true;

  const auto listed = [&](ColorFormat candidate)
  {
    const char* name = COLOR_FORMAT_NAMES[static_cast<size_t>(candidate)];
    return connector->GetPropertyEnumValue(COLOR_FORMAT, name).has_value();
  };

  // Adaptive only ever asks for the subsampled formats
  if (format == ColorFormat::ADAPTIVE)
    return listed(ColorFormat::YUV422) || listed(ColorFormat::YUV420);

  return format == ColorFormat::AUTO || listed(format);
}

ColorFormat CWinSystemGbm::GetColorFormat() const
{
  const int value = m_colorFormat;
  if (value < static_cast<int>(ColorFormat::AUTO) ||
      value > static_cast<int>(ColorFormat::ADAPTIVE))
    return ColorFormat::AUTO;

  return static_cast<ColorFormat>(value);
}

ColorFormat CWinSystemGbm::GetRequestedColorFormat() const
{
  if (!std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    return ColorFormat::AUTO;

  const auto connector = m_DRM->GetConnector();
  if (!connector || !connector->SupportsProperty(COLOR_FORMAT))
    return ColorFormat::AUTO;

  std::vector<ColorFormat> candidates;
  const ColorFormat format = GetColorFormat();
  if (format == ColorFormat::ADAPTIVE)
  {
    if (ContentNeedsSubsampling())
    {
      // 4:2:2 keeps the full vertical resolution of the chroma, but only fits up to 600 MHz
      if (m_DRM->GetModeClock() <= HDMI_MAX_TMDS_KHZ)
        candidates.push_back(ColorFormat::YUV422);
      candidates.push_back(ColorFormat::YUV420);
    }
  }
  else if (format != ColorFormat::AUTO)
  {
    candidates.push_back(format);
  }

  // Only formats that the driver lists (Intel has no YCbCr 4:2:2 on HDMI), or a colorspace tag
  // would describe the wrong encoding.
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](ColorFormat candidate)
                                  { return !IsColorFormatOffered(candidate); }),
                   candidates.end());

  // Auto is the last resort, after the formats refused for this mode
  candidates.push_back(ColorFormat::AUTO);

  const size_t skip = static_cast<size_t>(std::max(m_colorFormatSkip, 0));
  return candidates[std::min(skip, candidates.size() - 1)];
}

bool CWinSystemGbm::IsHdmiFrlLink() const
{
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector)
    return false;

  // no property tells: the sink has to do FRL, and a mode above the proof clock shows that the
  // source does too
  return connector->GetMaxFrlRate() >= HDMI_FRL_MIN_RATE_RGB_10BIT &&
         connector->HasModeAboveClock(HDMI_FRL_PROOF_CLOCK_KHZ);
}

bool CWinSystemGbm::ContentNeedsSubsampling() const
{
  if (m_contentBits <= 8 || !m_DRM)
    return false;

  // DisplayPort has the bandwidth or compresses, leave it to Auto
  const auto connector = m_DRM->GetConnector();
  if (!connector || connector->GetType().rfind("HDMI", 0) != 0)
    return false;

  if (IsHdmiFrlLink())
    return false;

  // RGB fits at 8 bit
  if (const uint64_t bpc = GetEffectiveMaxBpc(); bpc != 0 && bpc < 10)
    return false;

  // 10 bit RGB needs 10/8 of the pixel clock, YCbCr 4:2:0 half of that. Subsampling only helps
  // in between: below RGB fits, above not even 4:2:0 does, or the link is HDMI 2.1 and carries RGB.
  const uint64_t clock = m_DRM->GetModeClock();

  return clock * 5 / 4 > HDMI_MAX_TMDS_KHZ && clock * 5 / 8 <= HDMI_MAX_TMDS_KHZ;
}

bool CWinSystemGbm::IsYuvRequested() const
{
  if (!ConnectorPreferencesAllowed())
  {
    const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
    return connector && IsYuvFormat(*connector, RestoredConnectorProperty(COLOR_FORMAT));
  }
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector) return false;
  return IsYuvFormat(*connector, m_formatRequest.queued ?
      std::optional<uint64_t>(m_formatRequest.target) : connector->GetPropertyValue(COLOR_FORMAT));
}

bool CWinSystemGbm::IsYuvFormatCommitted(const CDRMConnector& connector) const
{
  return IsYuvFormat(connector, connector.GetPropertyValue(COLOR_FORMAT));
}

bool CWinSystemGbm::IsYuvFormat(const CDRMConnector& connector,
                               std::optional<uint64_t> current) const
{
  if (!current)
    return false;

  for (const ColorFormat format : {ColorFormat::YUV444, ColorFormat::YUV422, ColorFormat::YUV420})
  {
    const auto value = connector.GetPropertyEnumValue(
        COLOR_FORMAT, COLOR_FORMAT_NAMES[static_cast<size_t>(format)]);
    if (value && *value == *current)
      return true;
  }

  return false;
}

void CWinSystemGbm::ApplyColorRangeSetting(void* userptr)
{
  static_cast<CWinSystemGbm*>(userptr)->ApplyConnectorProperties();
}

ColorRange CWinSystemGbm::GetColorRange() const
{
  const int value = m_colorRange;
  if (value < static_cast<int>(ColorRange::AUTO) || value > static_cast<int>(ColorRange::LIMITED))
    return ColorRange::AUTO;

  return static_cast<ColorRange>(value);
}

void CWinSystemGbm::MigrateLimitedRangeSetting()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  if (!settings->GetBool(CSettings::SETTING_VIDEOSCREEN_LIMITEDRANGE))
    return;

  const auto rangeSetting = settings->GetSetting(SETTING_VIDEOSCREEN_COLORRANGE);
  if (!rangeSetting)
    return;

  // cleared either way, or choosing Auto later would be undone by the next start
  if (rangeSetting->IsDefault())
  {
    CLog::LogF(LOGINFO, "migrating \"use limited colour range\" to the colour range setting");
    m_migratingColorRange = true;
    settings->SetInt(SETTING_VIDEOSCREEN_COLORRANGE, static_cast<int>(ColorRange::LIMITED));
    m_migratingColorRange = false;
  }

  settings->SetBool(CSettings::SETTING_VIDEOSCREEN_LIMITEDRANGE, false);
}

void CWinSystemGbm::DeferConnectorPreferences()
{
  m_connectorPreferencesDeferred = true;
  m_rangeRequest.queued = m_formatRequest.queued = m_bpcRequest.queued = false;
}

void CWinSystemGbm::ApplyConnectorProperties(bool retry)
{
  if (!ConnectorPreferencesAllowed())
  {
    DeferConnectorPreferences();
    return;
  }
  m_connectorPreferencesDeferred = false;
  m_shaderColorRange = m_colorRange.load();
  if (!retry)
    m_connectorRetries = 0;

  ApplyBroadcastRange();
  ApplyMaxBpc();
  ApplyColorFormat();
  if (auto gui = CServiceBroker::GetGUI()) gui->GetWindowManager().MarkDirty();
}

void CWinSystemGbm::ApplyBroadcastRange()
{
  const char* targetName = BROADCAST_RGB_AUTOMATIC;
  switch (GetColorRange())
  {
    case ColorRange::FULL:
      targetName = BROADCAST_RGB_FULL;
      break;
    case ColorRange::LIMITED:
      targetName = BROADCAST_RGB_LIMITED;
      break;
    case ColorRange::AUTO:
      break;
  }

  QueueConnectorProperty(m_rangeRequest, BROADCAST_RGB, targetName);
}

bool CWinSystemGbm::QueueConnectorProperty(PropertyRequest& request,
                                           const char* property,
                                           const char* valueName)
{
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector)
    return false;

  const auto target = connector->GetPropertyEnumValue(property, valueName);
  if (!target)
    return false;

  return QueueConnectorProperty(request, property, *target, valueName);
}

bool CWinSystemGbm::QueueConnectorProperty(PropertyRequest& request,
                                           const char* property,
                                           uint64_t target,
                                           std::string_view label)
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return false;

  auto connector = drm->GetConnector();
  if (!connector || !connector->SupportsProperty(property))
    return false;

  if (connector->IsPropertyImmutable(property).value_or(false))
    return false;

  // always queue it, to replace a pending value that is not committed yet
  if (!drm->AddProperty(connector, property, target))
    return false;

  request.property = property;
  request.queued = true;
  request.target = target;

  CLog::LogF(LOGDEBUG, "queued connector \"{}\" = \"{}\"", property, label);

  const auto current = connector->GetPropertyValue(property);
  if (!current || *current != target)
  {
    drm->RequestModeset();
    if (std::strcmp(property, BROADCAST_RGB) == 0 || std::strcmp(property, COLOR_FORMAT) == 0)
      drm->SetColorRangeTransition();
  }

  return true;
}

void CWinSystemGbm::ApplyMaxBpc()
{
  const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector || !connector->SupportsProperty(MAX_BPC))
    return;

  uint64_t value = 0;
  const int setting = m_maxBpc;
  const auto limits = connector->GetRangePropertyLimits(MAX_BPC);
  if (!m_maxBpcFallback && setting > 0 && setting < static_cast<int>(MAX_BPC_VALUES.size()))
    value = MAX_BPC_VALUES[setting];
  else if (m_defaultMaxBpc)
    value = *m_defaultMaxBpc; // Auto, or back to it after a refusal
  else if (limits)
    value = limits->second; // fallback when the initial value was unavailable
  else
    return; // never written, so there is nothing to put back

  // the driver's limits, e.g. Intel stops at 12
  if (limits)
    value = std::clamp(value, limits->first, limits->second);

  QueueConnectorProperty(m_bpcRequest, MAX_BPC, value, std::to_string(value));
}

void CWinSystemGbm::ApplyColorFormat()
{
  if (!ConnectorPreferencesAllowed())
  {
    DeferConnectorPreferences();
    return;
  }
  const ColorFormat format = GetRequestedColorFormat();

  QueueConnectorProperty(m_formatRequest, COLOR_FORMAT,
                         COLOR_FORMAT_NAMES[static_cast<size_t>(format)]);

  if (IsYuvRequested() != m_scanoutYuv)
    ApplyColorspace();
}

bool CWinSystemGbm::IsRequestRejected(PropertyRequest& request)
{
  if (!request.queued)
    return false;

  request.queued = false;

  const CDRMConnector* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector)
    return false;

  // a refused commit is replaced by the previous request, which may hold the same value (e.g. when
  // switching modes), so the value alone does not tell
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  return (drm && !drm->LastPageAccepted()) || m_DRM->LastCommitFellBack() ||
         connector->GetPropertyValue(request.property) != request.target;
}

void CWinSystemGbm::RequeueConnectorProperties()
{
  ApplyConnectorProperties(true);

  ApplyColorspace();
  if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
      drm && m_hdrBlob.IsValid() && drm->GetConnector() &&
      drm->GetConnector()->SupportsProperty("HDR_OUTPUT_METADATA"))
    drm->AddProperty(drm->GetConnector(), "HDR_OUTPUT_METADATA", m_hdrBlob.Get());

  // The rejected commit took the modeset request along, and queueing only requests one when the
  // cached value differs from the target, which it may not (same depth, other refresh rate).
  if (m_DRM)
    m_DRM->RequestModeset();
}

void CWinSystemGbm::RetryConnectorProperties()
{
  const bool bpcPending = m_bpcRequest.queued;
  const bool rangeRejected = IsRequestRejected(m_rangeRequest);
  const bool formatRejected = IsRequestRejected(m_formatRequest);
  const bool bpcRejected = IsRequestRejected(m_bpcRequest);

  if (!rangeRejected && !formatRejected && !bpcRejected)
  {
    // the requests went through, a later rejection gets its own retry
    m_connectorRetries = 0;
    if (m_bpcProbe && bpcPending)
    {
      // the default depth went through, so the chosen one was refused
      m_bpcProbe = false;
      CLog::LogF(LOGWARNING,
                 "connector \"{}\" could not be committed with the chosen value, using the default",
                 MAX_BPC);
    }
    return;
  }

  if (auto gui = CServiceBroker::GetGUI()) gui->GetWindowManager().MarkDirty();

  // the default depth did not help, so it was not the cause: take the choice back
  bool probeFailed = false;
  if (m_bpcProbe)
  {
    m_bpcProbe = false;
    m_maxBpcFallback = false;
    probeFailed = true;
  }

  // an explicit format that the display or mode refuses is not worth a retry, likely the cause
  const bool explicitFormat = formatRejected && GetRequestedColorFormat() != ColorFormat::AUTO;
  const bool customBpc = bpcRejected && m_maxBpc > 0 && !m_maxBpcFallback;

  // format and depth are both suspects, try the default depth first so the format is kept
  if (explicitFormat && customBpc && !probeFailed)
  {
    m_bpcProbe = true;
    m_maxBpcFallback = true;
    m_maxBpcFailedMode = m_currentModeId;
    m_connectorRetries = 0;
    RequeueConnectorProperties();
    return;
  }

  // a rejected commit drops the queued properties and the modeset request, so queue them again
  if (!explicitFormat && m_connectorRetries++ < 1)
  {
    RequeueConnectorProperties();
    return;
  }

  if (explicitFormat)
  {
    const ColorFormat refused = GetRequestedColorFormat();
    m_colorFormatFailedMode = m_currentModeId;
    ++m_colorFormatSkip;

    CLog::LogF(LOGWARNING, "connector \"{}\" = \"{}\" could not be committed, using \"{}\"",
               COLOR_FORMAT, COLOR_FORMAT_NAMES[static_cast<size_t>(refused)],
               COLOR_FORMAT_NAMES[static_cast<size_t>(GetRequestedColorFormat())]);

    m_connectorRetries = 0;
    RequeueConnectorProperties();
    return;
  }

  // no format left to blame, the chosen depth may be the cause
  if (bpcRejected && m_maxBpc > 0 && !m_maxBpcFallback)
  {
    CLog::LogF(LOGWARNING, "connector \"{}\" = {} could not be committed, using the default",
               MAX_BPC, m_bpcRequest.target);

    m_maxBpcFallback = true;
    m_maxBpcFailedMode = m_currentModeId;
    m_connectorRetries = 0;
    RequeueConnectorProperties();
    return;
  }

  if (rangeRejected)
    CLog::LogF(LOGWARNING, "connector \"{}\" could not be committed", m_rangeRequest.property);
  if (formatRejected)
    CLog::LogF(LOGWARNING, "connector \"{}\" could not be committed", m_formatRequest.property);
  if (bpcRejected)
    CLog::LogF(LOGWARNING, "connector \"{}\" could not be committed", m_bpcRequest.property);
}

void CWinSystemGbm::UpdateDriverLimitedRange()
{
  bool limited = false;

  if (const CDRMConnector* connector = m_DRM ? m_DRM->GetConnector() : nullptr)
  {
    const auto current = connector->GetPropertyValue(BROADCAST_RGB);
    const auto limitedValue = connector->GetPropertyEnumValue(BROADCAST_RGB, BROADCAST_RGB_LIMITED);
    limited = current && limitedValue && *current == *limitedValue;

    // the driver's RGB to YCbCr conversion is limited range
    if (!limited)
      limited = IsYuvFormatCommitted(*connector);
  }

  m_driverLimitedRange = limited;
}

bool CWinSystemGbm::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  return true;
}

bool CWinSystemGbm::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  // Notify other subsystems that we will change resolution
  OnLostDevice();

  if (!m_DRM->SetMode(res))
  {
    CLog::Log(LOGERROR, "CWinSystemGbm::{} - failed to set DRM mode", __FUNCTION__);
    return false;
  }

  m_currentModeId = res.strId;
  // a refusal of a depth or format only holds for the mode it happened in
  if ((m_maxBpcFallback || m_bpcProbe) && m_maxBpcFailedMode != m_currentModeId)
  {
    m_maxBpcFallback = false;
    m_bpcProbe = false;
  }
  if (m_colorFormatSkip != 0 && m_colorFormatFailedMode != m_currentModeId)
    m_colorFormatSkip = 0;

  // the properties go out with the modeset, and are retried if it is rejected
  ApplyConnectorProperties();

  struct gbm_bo* bo = nullptr;

  if (!std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
  {
    bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get();
    if (!bo)
    {
      CLog::Log(LOGERROR, "CWinSystemGbm::{} - failed to lock front buffer", __FUNCTION__);
      return false;
    }
  }

  auto result = m_DRM->SetVideoMode(res, bo);

  // Complete Kodi's queued native modeset before the next EGL buffer swap.
  // This existing GBM synchronization is required for ordinary HDR as well;
  // it does not set DV signaling or override Kodi's HDR/SDR policy.
  if (result && std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    FlipPage(true, false, false);

  auto delay =
      std::chrono::milliseconds(CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                                    "videoscreen.delayrefreshchange") *
                                100);
  if (delay > 0ms)
    m_dispResetTimer.Set(delay);

  return result;
}

#ifdef HAVE_DVBRIDGE
bool CWinSystemGbm::RefreshDisplayInfoForVideo()
{
  auto* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  const auto id = connector && connector->IsConnected() ? *connector->GetConnectorId() : 0;
  auto edid = id ? connector->GetEDID(true) : std::vector<uint8_t>{};
  bool lldvChanged;
  {
    std::lock_guard lock(m_lldvEdidMutex);
    lldvChanged = id != m_lldvEdidConnector || edid != m_lldvEdid;
    if (lldvChanged)
    {
      m_lldvEdid = edid;
      m_lldvDisplay = DVBRIDGE::ReadLldvDisplay(edid.data(), edid.size());
      m_lldvEdidConnector = id;
      ++m_lldvEdidGeneration;
      m_lldvRejectedTimings = {};
    }
  }
  if (edid == m_displayEdid)
    return lldvChanged;
  // An absent or invalid EDID withdraws the previous sink's capabilities.
  auto information = edid.empty() ? nullptr : UTILS::CDisplayInfo::Create(edid);
  if (!information && !m_info)
    return lldvChanged;
  m_displayEdid = information ? std::move(edid) : std::vector<uint8_t>{};
  m_info = std::move(information);
  DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge display refresh: connector={} changed=true",id);
  return true;
}

namespace
{
double DVOutputTimingRate(const drmModeModeInfo* mode)
{
  if (!mode || !mode->hdisplay || (mode->hdisplay & 1) || !mode->vdisplay ||
      mode->hdisplay > 3840 || mode->vdisplay > 2160 ||
      uint64_t(mode->hdisplay) * mode->vdisplay < 5 * 3072u ||
      (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)))
    return 0.0;
  const double rate = mode->htotal && mode->vtotal
                          ? mode->clock * 1000.0 / mode->htotal / mode->vtotal
                          : 0.0;
  return CResolutionUtils::IsCB1RefreshRate(rate) ? rate : 0.0;
}
} // namespace

bool CWinSystemGbm::SupportsDVOutputMode(bool requireCurrentMode) const
{
  if (!m_DRM) return false;
  if (DVOutputTimingRate(m_DRM->GetSelectedMode())) return true;
  if (requireCurrentMode) return false;
  auto* connector = m_DRM->GetConnector();
  if (!connector || !connector->IsConnected()) return false;
  for (int i = 0; i < connector->GetModesCount(); ++i)
    if (DVOutputTimingRate(connector->GetModeForIndex(i))) return true;
  return false;
}

std::vector<CWinSystemGbm::DVOutputTiming> CWinSystemGbm::GetDVOutputTimings() const
{
  std::vector<DVOutputTiming> timings;
  auto* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector || !connector->IsConnected())
    return timings;

  for (int index = 0; index < connector->GetModesCount(); ++index)
  {
    const auto* mode = connector->GetModeForIndex(index);
    const double refresh = DVOutputTimingRate(mode);
    if (refresh > 0.0 && !(mode->flags & DRM_MODE_FLAG_3D_MASK) && mode->vscan <= 1)
      timings.push_back({index, mode->hdisplay, mode->vdisplay, refresh});
  }
  return timings;
}

bool CWinSystemGbm::SupportsNativeDVOutput(bool requireCurrentMode) const
{
  if (!m_info || !m_info->SupportsStandardDolbyVision() ||
      !std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
    return false;
  auto* connector = m_DRM->GetConnector();
  if (!connector || !connector->IsConnected() || !SupportsDVOutputMode(requireCurrentMode))
    return false;
  const auto backend = CDVBridgeState::GetBackend(*connector);
  return backend == CDVBridgeState::Backend::KERNEL ||
      (backend == CDVBridgeState::Backend::NVIDIA && m_info->SupportsRGBQuantizationSelection());
}

bool CWinSystemGbm::CompleteDVBridgePage(int timeoutMs)
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || !drm->HasPendingCB1Page()) return true;
  if (!drm->CompleteCB1Page(timeoutMs)) return false;
  m_GBM->GetDevice().GetSurface().CompleteTransactionalBuffer(true);
  return true;
}

bool CWinSystemGbm::FlipDVBridgePage(CDVBridgeState& state, bool restore, bool async,
                                    bool retainSignal, struct gbm_bo* bo)
{
  if (!CompleteDVBridgePage()) return false;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || !m_GBM || !state.Captured())
    return false;
  auto& surface = m_GBM->GetDevice().GetSurface();
  if (!bo) bo = surface.LockTransactionalBuffer();
  bool valid = bo != nullptr;
  if (valid && !restore)
  {
    const auto mode = drm->GetSelectedMode();
    const auto format = gbm_bo_get_format(bo);
    valid = mode && SupportsDVOutputMode() &&
            gbm_bo_get_width(bo) == mode->hdisplay && gbm_bo_get_height(bo) == mode->vdisplay &&
            (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) &&
            !(mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN));
    if (valid)
    {
      const double rate = mode->htotal && mode->vtotal
                              ? mode->clock * 1000.0 / mode->htotal / mode->vtotal : 0.0;
      valid = CResolutionUtils::IsCB1RefreshRate(rate);
    }
  }
  if (valid)
    valid = state.Stage(*drm, restore);
  if (valid) valid = StageTransportPreferences();
  if (valid && restore && retainSignal)
    valid = RequeueOutputProperties();
  bool accepted = false;
  bool restarted = false;
  // Hades RC2: restart an active NVIDIA head when only the transport depth changes.
  // A queued new mode already performs that link transition in its own commit.
  if (valid && restore && state.CapturedBackend() == CDVBridgeState::Backend::NVIDIA &&
      drm->IsActive() && drm->IsSelectedModeCommitted())
  {
    drm->SetActive(false);
    const bool off = drm->FlipPageTransactional(bo, true);
    drm->SetActive(true);
    if (off)
    {
      restarted = true;
      const bool staged = state.Stage(*drm, true) && StageTransportPreferences() &&
          (!retainSignal || RequeueOutputProperties());
      accepted = drm->FlipPageTransactional(staged ? bo : nullptr, true);
    }
    else
      valid = state.Stage(*drm, true) && StageTransportPreferences() && (!retainSignal || RequeueOutputProperties());
  }
  if (!restarted)
    accepted = drm->FlipPageTransactional(valid ? bo : nullptr, restore, async);
  if (!drm->HasPendingCB1Page()) surface.CompleteTransactionalBuffer(accepted);
  if (accepted && restore)
  {
    surface.EndTransactionalMode();
    state.Restored();
  }
  return accepted;
}
#endif

void CWinSystemGbm::FlipPage(bool rendered, bool videoLayer, bool async)
{
  FlipPage(rendered, videoLayer, async, {});
}

bool CWinSystemGbm::FlipPage(bool rendered, bool videoLayer, bool async,
                             const std::shared_ptr<gbm_bo>& carrier)
{
  if (m_videoLayerBridge && !videoLayer)
  {
    // disable video plane when video layer no longer is active
    m_videoLayerBridge->Disable();
  }

  if (!ConnectorPreferencesAllowed())
    DeferConnectorPreferences();

  auto atomic = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  bool wideTransition = false;
  if (!StageNativeSDRSignal(rendered, wideTransition))
  {
    if (auto gui = CServiceBroker::GetGUI()) gui->GetWindowManager().MarkDirty();
    return false;
  }
  const bool transactional = rendered && atomic && (carrier || atomic->HasColorRangeTransition());
  struct gbm_bo* bo = nullptr;

  if (rendered)
  {
    auto& surface = m_GBM->GetDevice().GetSurface();
    bo = carrier ? surface.AdoptTransactionalBuffer(carrier.get(), carrier)
                 : transactional ? surface.LockTransactionalBuffer() : surface.LockFrontBuffer().Get();
    if (transactional && !bo)
    {
      if (auto gui = CServiceBroker::GetGUI()) gui->GetWindowManager().MarkDirty();
      return false;
    }
  }

  // A refused colour request keeps the old scanout. Release its GBM buffers
  // only after a blocking replacement has completed, as in CB1 restoration.
  m_DRM->FlipPage(bo, rendered, videoLayer, transactional ? false : async);
  if (transactional)
  {
    auto& surface = m_GBM->GetDevice().GetSurface();
    surface.CompleteTransactionalBuffer(atomic->LastPageAccepted());
    if (atomic->LastPageAccepted()) surface.EndTransactionalMode();
  }

  if (rendered && atomic)
  {
    if (atomic->LastPageAccepted())
      m_nativeSDRSubmitted = m_nativeSDRActive;
    else if (wideTransition)
    {
      // Retain the previous framebuffer and render the next frame into BT.709.
      m_queuedColorspace.reset();
      m_nativeSDRRejected = true;
      if (auto gui = CServiceBroker::GetGUI()) gui->GetWindowManager().MarkDirty();
    }
  }

  if (ConnectorPreferencesAllowed())
  {
    UpdateDriverLimitedRange();
    RetryConnectorProperties();
  }

  // !videoLayer alone cannot gate teardown: FlipPage also runs with videoLayer
  // false during a mode switch or renderer swap. use_count reaches 1 only when
  // CRendererDRMPRIME drops its bridge reference, which happens at video stop.
  if (m_videoLayerBridge && !videoLayer && m_videoLayerBridge.use_count() == 1)
  {
    // delete video layer bridge when video layer no longer is active
    m_videoLayerBridge.reset();

    //! @todo unify D2P and single-plane teardown behind one winsystem plane API
    m_DRM->ReleaseVideoPlane();
    auto* gui = m_DRM->GetGuiPlane();
    m_DRM->FindGuiPlane(gui->GetFormat(), gui->GetModifier());
  }
  if (atomic)
    return atomic->LastPageAccepted();
  return false;
}

bool CWinSystemGbm::OrdinaryShaderLimitedColor()
{
  UpdateDriverLimitedRange();
  return !m_driverLimitedRange &&
      m_shaderColorRange == static_cast<int>(ColorRange::LIMITED);
}

bool CWinSystemGbm::UseLimitedColor()
{
  // CB1 consumes the user's range in its own renderer and owns the wire conversion.
  if (!ConnectorPreferencesAllowed())
  {
    if (const auto limited = TransportLimitedColor()) return *limited;
    const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
    const auto restored = RestoredConnectorProperty(BROADCAST_RGB);
    const auto limited = connector ?
        connector->GetPropertyEnumValue(BROADCAST_RGB, BROADCAST_RGB_LIMITED) : std::nullopt;
    if ((restored && limited && *restored == *limited) || IsYuvRequested())
      return false;
    return GetColorRange() == ColorRange::LIMITED;
  }
  if (m_connectorPreferencesDeferred)
  {
    ApplyConnectorProperties();
    ApplyColorspace();
  }
  UpdateDriverLimitedRange();
  bool driverLimited = m_driverLimitedRange;
  if (const auto connector = m_DRM ? m_DRM->GetConnector() : nullptr;
      connector && (m_rangeRequest.queued || m_formatRequest.queued))
  {
    const auto range = m_rangeRequest.queued ? std::optional<uint64_t>(m_rangeRequest.target) :
        connector->GetPropertyValue(BROADCAST_RGB);
    const auto limited = connector->GetPropertyEnumValue(BROADCAST_RGB, BROADCAST_RGB_LIMITED);
    const auto format = m_formatRequest.queued ? std::optional<uint64_t>(m_formatRequest.target) :
        connector->GetPropertyValue(COLOR_FORMAT);
    driverLimited = (range && limited && *range == *limited) || IsYuvFormat(*connector, format);
  }
  return !driverLimited && m_shaderColorRange == static_cast<int>(ColorRange::LIMITED);
}

bool CWinSystemGbm::Hide()
{
  bool ret = m_DRM->SetActive(false);
  FlipPage(false, false, false);
  return ret;
}

bool CWinSystemGbm::Show(bool raise)
{
  bool ret = m_DRM->SetActive(true);
  FlipPage(false, false, false);
  return ret;
}

void CWinSystemGbm::Register(IDispResource* resource)
{
  std::unique_lock lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemGbm::Unregister(IDispResource* resource)
{
  std::unique_lock lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
  {
    m_resources.erase(i);
  }
}

void CWinSystemGbm::OnLostDevice()
{
  if (m_dispReset) return;
  CLog::Log(LOGDEBUG, "{} - notify display change event", __FUNCTION__);
  m_dispReset = true;

  std::unique_lock lock(m_resourceSection);
  std::vector<IDispResource*> resources = m_resources;

  for (auto resource : resources)
  {
    if (find(m_resources.begin(), m_resources.end(), resource) == m_resources.end())
      continue;

    resource->OnLostDisplay();
  }
}

std::unique_ptr<CVideoSync> CWinSystemGbm::GetVideoSync(CVideoReferenceClock* clock)
{
  return std::make_unique<CVideoSyncGbm>(clock);
}

std::vector<std::string> CWinSystemGbm::GetConnectedOutputs()
{
  std::vector<std::string> outputs;
  outputs.emplace_back(OUTPUT_NAME_DEFAULT);
  for (const auto& name : m_DRM->GetConnectedConnectorNames())
  {
    outputs.emplace_back(name);
  }
  return outputs;
}

bool CWinSystemGbm::SetVideoOutput(const VideoPicture* videoPicture)
{
  // Base: flip the gui/video plane role for the single shared output
  // plane on GBM. videoPicture set means a video is starting and the
  // output plane should be tracked as the video plane; nullptr means
  // playback ended and the plane reverts to the gui role.
  //
  // Format and modifier come from whichever plane is currently active.
  // EGL backends override this and rebuild the GBM/EGL output surface
  // at the target bit depth before chaining here, so by the time we
  // run the active plane's cached format reflects the post-rebuild
  // surface (e.g. AR30 for 10-bit content).
  CDRMPlane* current = m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
  if (!current)
    return false;

  return videoPicture ? m_DRM->FindVideoPlane(current->GetFormat(), current->GetModifier())
                      : m_DRM->FindGuiPlane(current->GetFormat(), current->GetModifier());
}

void CWinSystemGbm::SetColorimetry(const VideoPicture* videoPicture, bool nativeSDRShader)
{
  const bool nativeSDR = nativeSDRShader && videoPicture &&
                         videoPicture->hdrType == StreamHdrType::HDR_TYPE_NONE &&
                         videoPicture->color_transfer != AVCOL_TRC_SMPTE2084 &&
                         videoPicture->color_transfer != AVCOL_TRC_ARIB_STD_B67;
  m_nativeSDRSource = nativeSDR && videoPicture->color_primaries == AVCOL_PRI_BT2020;
  m_nativeSDRRejected = false;
  if (!m_nativeSDRSource)
    m_nativeSDRActive = false;
  if (!m_nativeSDRSource && m_nativeSDRCompositor)
  {
    SetGuiCompositing(0);
    m_nativeSDRCompositor = false;
  }

  m_queuedColorspace.reset();
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return;

  // Adaptive follows the video: >8 bit content may need YCbCr, so evaluate when it starts or stops
  m_contentBits = videoPicture ? videoPicture->colorBits : 0;
  if (GetColorFormat() == ColorFormat::ADAPTIVE)
    ApplyColorFormat();

  auto connector = drm->GetConnector();
  if (!connector || !connector->SupportsProperty("Colorspace"))
    return;

  KODI::UTILS::Colorimetry colorimetry = KODI::UTILS::Colorimetry::DEFAULT;

  if (videoPicture)
  {
    colorimetry = KODI::UTILS::GetColorimetry(*videoPicture);
    if (nativeSDR)
      colorimetry = KODI::UTILS::Colorimetry::BT709_YCC;
  }

  m_colorimetry = colorimetry;

  ApplyColorspace();
}

std::optional<uint64_t> CWinSystemGbm::NativeSDRBT2020Colorspace() const
{
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector || !connector->IsConnected() || !connector->GetType().starts_with("HDMI") ||
      !connector->SupportsProperty("Colorspace") ||
      connector->IsPropertyImmutable("Colorspace").value_or(false) || !m_info)
    return {};

  const auto target = IsYuvRequested() ? KODI::UTILS::Colorimetry::BT2020_YCC :
                                        KODI::UTILS::Colorimetry::BT2020_RGB;
  if (!m_info->SupportsColorimetry(target))
    return {};
  return connector->GetPropertyEnumValue("Colorspace", ColorimetryMap.at(target));
}

bool CWinSystemGbm::BeginFrameGuiComposite(bool guiWillRender)
{
  const bool wasNative = m_nativeSDRCompositor;
  // Only the renderer that owns a single RGB scanout can compose both layers
  // into BT.2020. Keep the existing CMS and CB1 transport owners independent.
  const bool wide = m_nativeSDRSource && !m_nativeSDRRejected && ConnectorPreferencesAllowed() &&
                    m_DRM && m_DRM->GetVideoPlane() && !m_DRM->GetGuiPlane() &&
                    !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
                        "videoscreen.cmsenabled") &&
                    NativeSDRBT2020Colorspace().has_value();
  if (wide && (!m_nativeSDRCompositor || !IsHdrComposite()))
    m_nativeSDRCompositor = SetGuiCompositing(AVCOL_TRC_BT2020_10);
  else if (!wide && m_nativeSDRCompositor)
  {
    SetGuiCompositing(0);
    m_nativeSDRCompositor = false;
  }

  const bool compositing = BeginGuiComposite(guiWillRender);
  m_nativeSDRActive = wide && m_nativeSDRCompositor && compositing;
  if (wide && !m_nativeSDRActive)
  {
    SetGuiCompositing(0);
    m_nativeSDRCompositor = false;
    m_nativeSDRRejected = true;
  }
  if (wasNative != m_nativeSDRCompositor)
  {
    // BeginRender ran before negotiation. Match the GUI shaders to the FBO
    // so the first frame also applies display range only once.
    GetRenderSystem()->UpdateGUIShaderColorState();
  }
  return compositing;
}

bool CWinSystemGbm::StageNativeSDRSignal(bool rendered, bool& wideTransition)
{
  wideTransition = false;
  if (!m_nativeSDRSubmitted && (!rendered || (!m_nativeSDRActive && !m_nativeSDRRejected)))
    return true;

  const bool wide = rendered ? m_nativeSDRActive.load() : m_nativeSDRSubmitted;
  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector || !connector->SupportsProperty("Colorspace"))
  {
    m_nativeSDRSubmitted = false;
    return !wide;
  }

  const auto committed = connector->GetPropertyValue("Colorspace");
  if (wide)
  {
    const auto target = NativeSDRBT2020Colorspace();
    if (!target || !drm->AddProperty(connector, "Colorspace", *target))
    {
      m_queuedColorspace.reset();
      m_nativeSDRRejected = true;
      return false;
    }
    m_queuedColorspace = target;
  }
  else
  {
    // An earlier wide request may have replaced the ordinary BT.709 request
    // before it was accepted, even when no wide frame was ever submitted.
    if (!m_nativeSDRSubmitted && m_queuedColorspace && m_queuedColorspace == committed)
      return true;
    ApplyColorspace();
    if (!m_queuedColorspace)
      return false;
  }

  const bool tagChanged = m_queuedColorspace != committed;
  wideTransition = wide && (tagChanged || !drm->IsSelectedModeCommitted());
  // Never attach a framebuffer in the new gamut to the old atomic request.
  if (tagChanged || wideTransition)
    drm->SetColorRangeTransition();
  if (tagChanged)
    drm->RequestModeset();
  return true;
}

void CWinSystemGbm::ApplyColorspace()
{
  m_queuedColorspace.reset();
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return;

  auto connector = drm->GetConnector();
  if (!connector || !connector->SupportsProperty("Colorspace"))
    return;

  const KODI::UTILS::Colorimetry colorimetry = m_colorimetry;
  const bool yuv = IsYuvRequested();
  m_scanoutYuv = yuv;

  // The DRM Colorspace connector property controls what colorimetry tag the
  // driver transmits in the HDMI AVI InfoFrame (or DisplayPort VSC SDP). The
  // transmitted tag must match the pixel encoding actually on the wire.
  // Kodi scans out to an RGB primary plane (XR24/XR30/AB4H), which the driver sends as RGB or
  // converts when a YCbCr "color format" is requested. Signaling YCC while sending RGB, or the
  // opposite, causes most DP-to-HDMI bridges (incl. LSPCON) to misinterpret the signal.
  //
  // The CTA-861 standard (used by both HDMI and DisplayPort for colorimetry
  // signaling) defines explicit RGB variants only for wide-gamut spaces
  // (BT.2020_RGB, DCI-P3_RGB). BT.709 RGB and BT.601 RGB are signaled via
  // "Default", with the sink inferring primaries from the transmitted mode
  // (HD resolution -> BT.709, SD resolution -> BT.601).
  //
  // For YCbCr, wide gamut content is tagged BT2020_YCC and the rest BT709_YCC: the driver's
  // conversion does not depend on the resolution, so "Default" could read an SD mode as BT.601.
  KODI::UTILS::Colorimetry scanoutColorimetry;
  switch (colorimetry)
  {
    case KODI::UTILS::Colorimetry::BT2020_YCC:
    case KODI::UTILS::Colorimetry::BT2020_CYCC:
      scanoutColorimetry =
          yuv ? KODI::UTILS::Colorimetry::BT2020_YCC : KODI::UTILS::Colorimetry::BT2020_RGB;
      break;
    default:
      // BT709_YCC, SMPTE_170M_YCC and all other YCC variants map to Default for RGB, see above
      scanoutColorimetry =
          yuv ? KODI::UTILS::Colorimetry::BT709_YCC : KODI::UTILS::Colorimetry::DEFAULT;
      break;
  }

  std::optional<uint64_t> colorspace =
      connector->GetPropertyEnumValue("Colorspace", ColorimetryMap.at(scanoutColorimetry));
  if (!colorspace && yuv)
  {
    // no YCC variant listed: an untagged signal is better than a wrong one
    scanoutColorimetry = KODI::UTILS::Colorimetry::DEFAULT;
    colorspace =
        connector->GetPropertyEnumValue("Colorspace", ColorimetryMap.at(scanoutColorimetry));
  }

  if (colorspace)
  {
    CLog::LogF(LOGDEBUG, "setting connector colorspace to {} (source {}, {} output)",
               KODI::UTILS::ColorimetryToString(scanoutColorimetry),
               KODI::UTILS::ColorimetryToString(colorimetry), yuv ? "YCbCr" : "RGB");
    if (drm->AddProperty(connector, "Colorspace", *colorspace))
    {
      m_queuedColorspace = colorspace;
      drm->SetActive(true);
    }
  }
}

namespace
{
hdr_output_metadata MakeHDRMetadata(const VideoPicture& picture, KODI::UTILS::Eotf eotf)
{
  hdr_output_metadata hdr_metadata = {};

  hdr_metadata.metadata_type = KODI::UTILS::HDMI_STATIC_METADATA_TYPE1;
  hdr_metadata.hdmi_metadata_type1.eotf = static_cast<uint8_t>(eotf);
  hdr_metadata.hdmi_metadata_type1.metadata_type = KODI::UTILS::HDMI_STATIC_METADATA_TYPE1;

  if (hdr_metadata.hdmi_metadata_type1.eotf)
  {
    const AVMasteringDisplayMetadata* mdmd =
        KODI::UTILS::GetMasteringDisplayMetadata(picture);
    if (mdmd && mdmd->has_primaries)
    {
      // Convert to unsigned 16-bit values in units of 0.00002,
      // where 0x0000 represents zero and 0xC350 represents 1.0000
      for (int i = 0; i < 3; i++)
      {
        hdr_metadata.hdmi_metadata_type1.display_primaries[i].x =
            std::round(av_q2d(mdmd->display_primaries[i][0]) * 50000.0);
        hdr_metadata.hdmi_metadata_type1.display_primaries[i].y =
            std::round(av_q2d(mdmd->display_primaries[i][1]) * 50000.0);

        CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - display_primaries[{}].x: {}",
                  __FUNCTION__, i, hdr_metadata.hdmi_metadata_type1.display_primaries[i].x);
        CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - display_primaries[{}].y: {}",
                  __FUNCTION__, i, hdr_metadata.hdmi_metadata_type1.display_primaries[i].y);
      }
      hdr_metadata.hdmi_metadata_type1.white_point.x =
          std::round(av_q2d(mdmd->white_point[0]) * 50000.0);
      hdr_metadata.hdmi_metadata_type1.white_point.y =
          std::round(av_q2d(mdmd->white_point[1]) * 50000.0);

      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - white_point.x: {}", __FUNCTION__,
                hdr_metadata.hdmi_metadata_type1.white_point.x);
      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - white_point.y: {}", __FUNCTION__,
                hdr_metadata.hdmi_metadata_type1.white_point.y);
    }
    if (mdmd && mdmd->has_luminance)
    {
      // Convert to unsigned 16-bit value in units of 1 cd/m2,
      // where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2
      hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance =
          std::round(av_q2d(mdmd->max_luminance));

      // Convert to unsigned 16-bit value in units of 0.0001 cd/m2,
      // where 0x0001 represents 0.0001 cd/m2 and 0xFFFF represents 6.5535 cd/m2
      hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance =
          std::round(av_q2d(mdmd->min_luminance) * 10000.0);

      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - max_display_mastering_luminance: {}",
                __FUNCTION__, hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance);
      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - min_display_mastering_luminance: {}",
                __FUNCTION__, hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance);
    }

    const AVContentLightMetadata* clmd = KODI::UTILS::GetContentLightMetadata(picture);
    if (clmd)
    {
      hdr_metadata.hdmi_metadata_type1.max_cll = clmd->MaxCLL;
      hdr_metadata.hdmi_metadata_type1.max_fall = clmd->MaxFALL;

      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - max_cll: {}", __FUNCTION__,
                hdr_metadata.hdmi_metadata_type1.max_cll);
      CLog::Log(LOGDEBUG, LOGVIDEO, "CWinSystemGbm::{} - max_fall: {}", __FUNCTION__,
                hdr_metadata.hdmi_metadata_type1.max_fall);
    }

  }
  return hdr_metadata;
}
} // namespace

bool CWinSystemGbm::CanKeepHDR10Signal(const VideoPicture& picture) const
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings || !settings->GetBool(SETTING_WINSYSTEM_IS_HDR_DISPLAY))
    return false;

  const auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm || !drm->HasQuirk(QUIRK_NEEDSPRIMARY) || !drm->IsActive() ||
      !drm->IsSelectedModeCommitted() || drm->LastCommitFellBack() ||
      drm->IsSDRRestorePending() || drm->HasPendingRequest() || drm->HasPendingCB1Page() ||
      drm->HasColorRangeTransition() || drm->HDR10PlusActive() || !m_hdrBlob.IsValid() ||
      m_eotf != KODI::UTILS::Eotf::PQ || picture.color_transfer != AVCOL_TRC_SMPTE2084 ||
      KODI::UTILS::GetColorimetry(picture) != m_colorimetry || !m_queuedColorspace)
    return false;

  auto* connector = drm->GetConnector();
  const auto* crtc = drm->GetCrtc();
  if (!connector || !crtc)
    return false;

  // An HDR10+ source can retain the static PQ signal when the output cannot send HDR10+.
  if (picture.hdrType != StreamHdrType::HDR_TYPE_HDR10 &&
      (picture.hdrType != StreamHdrType::HDR_TYPE_HDR10PLUS ||
       (connector->SupportsProperty("HDR10PLUS_METADATA") &&
        (!m_info || m_info->HDR10PlusVersion() >= 0))))
    return false;

  return crtc->GetPropertyValue("ACTIVE") == 1 &&
      connector->GetPropertyValue("HDR_OUTPUT_METADATA") == m_hdrBlob.Get() &&
      connector->GetPropertyValue("Colorspace") == m_queuedColorspace;
}

bool CWinSystemGbm::PresentHDR10Handoff(const VideoPicture& picture)
{
  if (!CanKeepHDR10Signal(picture))
    return false;

  const auto drm = std::static_pointer_cast<CDRMAtomic>(m_DRM);
  const auto metadata = MakeHDRMetadata(picture, m_eotf);
  CDRMPropertyBlob candidate(drm->GetFileDescriptor(), &metadata, sizeof(metadata));
  if (!candidate.IsValid())
    return false;

  auto& surface = m_GBM->GetDevice().GetSurface();
  auto* buffer = surface.LockTransactionalBuffer();
  const bool accepted = buffer && drm->FlipPageWithHDR10Metadata(buffer, candidate.Get());
  surface.CompleteTransactionalBuffer(accepted);
  if (!accepted)
    return false;

  surface.EndTransactionalMode();
  m_hdrBlob = std::move(candidate);
  m_queuedHdrMetadata = m_hdrBlob.Get();
  CLog::Log(LOGDEBUG, "HDR10 handoff committed picture and metadata blob {} together", m_hdrBlob.Get());
  return true;
}

bool CWinSystemGbm::SetHDR(const VideoPicture* videoPicture)
{
  if (!videoPicture)
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM))
      drm->SetSDRRestorePending(true);
  m_queuedHdrMetadata = 0;
  m_eotf = KODI::UTILS::Eotf::TRADITIONAL_SDR;
  m_hdrBlob.Reset();
  auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return false;

  auto settings = settingsComponent->GetSettings();
  if (!settings)
    return false;

  if (!settings->GetBool(SETTING_WINSYSTEM_IS_HDR_DISPLAY))
    return false;

  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return false;

  auto connector = drm->GetConnector();
  if (!connector)
    return false;

  if (!videoPicture)
  {
    if (connector->SupportsProperty("HDR_OUTPUT_METADATA"))
    {
      CLog::LogF(LOGDEBUG, "clearing HDR_OUTPUT_METADATA");
      drm->AddProperty(connector, "HDR_OUTPUT_METADATA", 0);
      drm->SetActive(true);

    }

    return false;
  }

  // Only enable HDR for PQ (HDR10/HDR10+/DV) or HLG transfer functions
  if (videoPicture->color_transfer != AVCOL_TRC_SMPTE2084 &&
      videoPicture->color_transfer != AVCOL_TRC_ARIB_STD_B67)
  {
    return false;
  }

  KODI::UTILS::Eotf eotf = KODI::UTILS::GetEOTF(*videoPicture);

  if (connector->SupportsProperty("HDR_OUTPUT_METADATA") && m_info &&
      m_info->SupportsHDRStaticMetadataType1() && m_info->SupportsEOTF(eotf))
  {
    const auto hdr_metadata = MakeHDRMetadata(*videoPicture, eotf);
    if (hdr_metadata.hdmi_metadata_type1.eotf)
    {
      m_hdrBlob = CDRMPropertyBlob(drm->GetFileDescriptor(), &hdr_metadata, sizeof(hdr_metadata));
    }

    if (!m_hdrBlob.IsValid() ||
        !drm->AddProperty(connector, "HDR_OUTPUT_METADATA", m_hdrBlob.Get()))
    {
      m_hdrBlob.Reset();
      CLog::LogF(LOGWARNING, "HDR output rejected: metadata blob allocation or property staging failed");
      return false;
    }
    m_eotf = eotf;
    m_queuedHdrMetadata = m_hdrBlob.Get();
    drm->SetSDRRestorePending(false);
    drm->SetActive(true);
  }
  else
  {
    CLog::LogF(LOGWARNING, "HDR output unavailable: metadata_property={} display_metadata={} eotf_supported={}",
               connector->SupportsProperty("HDR_OUTPUT_METADATA"),
               m_info && m_info->SupportsHDRStaticMetadataType1(),
               m_info && m_info->SupportsEOTF(eotf));
  }

  return m_hdrBlob.IsValid();
}

bool CWinSystemGbm::RequeueOutputProperties()
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector) return false;
  bool valid = true;
  if (m_queuedColorspace)
    valid &= drm->AddProperty(connector, "Colorspace", *m_queuedColorspace);
  if (m_queuedHdrMetadata && connector->SupportsProperty("HDR_OUTPUT_METADATA"))
    valid &= drm->AddProperty(connector, "HDR_OUTPUT_METADATA", *m_queuedHdrMetadata);
  drm->AllowModeset();
  return valid;
}

#ifdef HAVE_DVBRIDGE
bool CWinSystemGbm::StageOrdinaryHDR10Recovery()
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  auto* connector = drm ? drm->GetConnector() : nullptr;
  if (!connector)
    return false;
  if (connector->SupportsProperty("HDR_OUTPUT_METADATA") &&
      !drm->AddProperty(connector, "HDR_OUTPUT_METADATA", 0))
    return false;
  if (connector->SupportsProperty("Colorspace"))
  {
    const auto value = connector->GetPropertyEnumValue("Colorspace", "Default");
    if (!value || !drm->AddProperty(connector, "Colorspace", *value))
      return false;
  }
  drm->SetActive(true);
  return true;
}

void CWinSystemGbm::CompleteOrdinaryHDR10Recovery()
{
  m_hdrBlob.Reset();
  m_eotf = KODI::UTILS::Eotf::TRADITIONAL_SDR;
  m_colorimetry = KODI::UTILS::Colorimetry::DEFAULT;
}
#endif

bool CWinSystemGbm::IsHDRDisplay()
{
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm)
    return false;

  auto connector = drm->GetConnector();
  if (!connector)
    return false;

  return connector->SupportsProperty("HDR_OUTPUT_METADATA") && m_info &&
         m_info->SupportsHDRStaticMetadataType1();
}

#ifdef HAVE_DVBRIDGE
namespace
{
// Five supported CTA timings, each with or without the optional 16:9 flag.
int LldvTimingIndex(const drmModeModeInfo* mode, bool cta93FractionalOnly)
{
  if (!mode || mode->hdisplay != 3840 || mode->vdisplay != 2160 || mode->vtotal != 2250 ||
      mode->vsync_start != 2168 || mode->vsync_end != 2178 || mode->hskew || mode->vscan ||
      (mode->clock != 297000 && mode->clock != 296703) ||
      (mode->flags & ~DRM_MODE_FLAG_PIC_AR_16_9) !=
          (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC))
    return -1;

  if (cta93FractionalOnly && (mode->htotal != 5500 || mode->clock != 296703))
    return -1;

  const int aspect = (mode->flags & DRM_MODE_FLAG_PIC_AR_16_9) ? 5 : 0;
  const int fractional = mode->clock == 296703 ? 1 : 0;
  if (mode->htotal == 5500 && mode->hsync_start == 5116 && mode->hsync_end == 5204)
    return aspect + fractional;
  if (mode->htotal == 5280 && mode->hsync_start == 4896 && mode->hsync_end == 4984 &&
      !fractional)
    return aspect + 2;
  if (mode->htotal == 4400 && mode->hsync_start == 4016 && mode->hsync_end == 4104)
    return aspect + 3 + fractional;
  return -1;
}
} // namespace

bool CWinSystemGbm::FlipPlayerLedPage(CDVBridgeState& state,
                                     const DVBRIDGE::LldvTransport& packed, bool async,
                                     gbm_bo* carrier, std::shared_ptr<void> lease)
{
  using namespace DVBRIDGE;
  if (!CompleteDVBridgePage()) return false;
  auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
  if (!drm) return false;
  const auto reject = [&]() { return drm->FlipPageTransactional(nullptr); };
  if (!m_GBM || drm->IsSDRRestorePending()) return reject();

  const auto status = GetLldvConnectorStatus();
  const int preference = packed.forced ? LLDV_FORCE_422_10 :
      packed.experimentalPacked12 ? LLDV_PACKED_422_12 : static_cast<int>(packed.format);
  if (!packed.connectorId || !(packed == SelectLldvTransport(status, preference)))
    return reject();

  const char* name = packed.rgbScanoutBits > 8 ? LldvNativeEnumName(packed.format) :
      packed.forced ? "YCbCr 4:2:2 10 bits forced" :
      packed.format == LldvFormat::Y422_10 ? "YCbCr 4:2:2 10 useful bits" :
      packed.experimentalPacked12 ? "YCbCr 4:2:2 12 bits packed" : "YCbCr 4:2:2 12 bits";
  auto* connector = drm->GetConnector();
  const auto value = connector ? connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", name) :
                                 std::nullopt;
  if (!value || (!state.Captured() && !state.CapturePlayerLed(*drm))) return reject();

  // The caller has swapped the packed carrier into the existing GBM surface.
  auto& surface = m_GBM->GetDevice().GetSurface();
  auto* bo = carrier ? surface.AdoptTransactionalBuffer(carrier, std::move(lease)) :
                       surface.LockTransactionalBuffer();
  if (!bo) return reject();
  const auto format = gbm_bo_get_format(bo);
  const bool validFormat = packed.rgbScanoutBits == 16 ? format == DRM_FORMAT_ABGR16161616 :
      packed.rgbScanoutBits == 10 ?
      (format == DRM_FORMAT_XRGB2101010 || format == DRM_FORMAT_ARGB2101010 ||
       format == DRM_FORMAT_XBGR2101010 || format == DRM_FORMAT_ABGR2101010) :
      (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888);
  const bool ready = validFormat &&
      gbm_bo_get_width(bo) == 3840 && gbm_bo_get_height(bo) == 2160 &&
      state.StagePlayerLed(*drm, *value, packed.rgbScanoutBits == 16 ? 12 : packed.rgbScanoutBits) && StageTransportPreferences();
  const bool stable = status.timingCommitted && status.active == packed.format &&
                      status.activeForced == packed.forced &&
                      status.activeRgbScanoutBits == packed.rgbScanoutBits &&
                      status.activeExperimentalPacked12 == packed.experimentalPacked12;
  if (ready)
  {
    drm->StageHDR10Plus(std::nullopt, 0);
    if (!stable)
      drm->AllowModeset();
  }
  const auto* mode = ready && !stable ? drm->GetSelectedMode() : nullptr;
  const int timing = LldvTimingIndex(mode, status.cta93FractionalOnly);
  const int clock = mode ? mode->clock : 0;
  const int htotal = mode ? mode->htotal : 0;
  bool formatRejected = false;
  const bool accepted = drm->FlipPageTransactional(ready ? bo : nullptr, false, async && stable,
      ready && !stable && timing >= 0 ? &formatRejected : nullptr);
  if (formatRejected)
  {
    std::lock_guard lock(m_lldvEdidMutex);
    if (packed.connectorId == m_lldvEdidConnector && packed.edidGeneration == m_lldvEdidGeneration)
      m_lldvRejectedTimings[timing] |= LldvTransportBit(packed);
  }
  if (!accepted && !stable)
    CLog::Log(LOGWARNING, "CB1 LLDV: DRM commit refused connector={} edid_generation={} format={} forced={} staged={} clock={} htotal={} format_rejected={}",
              packed.connectorId, packed.edidGeneration, name, packed.forced, ready, clock, htotal,
              formatRejected);
  if (!drm->HasPendingCB1Page()) surface.CompleteTransactionalBuffer(accepted);
  return accepted;
}

CWinSystemGbm::SDRBT2020Support CWinSystemGbm::GetSDRBT2020Support() const
{
  // CB1's existing SDR scanout stages RGB via CaptureColorPreferences.
  auto* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (!connector || !connector->GetType().starts_with("HDMI") ||
      !connector->GetPropertyEnumValue("Colorspace", "BT2020_RGB"))
    return SDRBT2020Support::Driver;
  if (!m_info || !m_info->SupportsColorimetry(KODI::UTILS::Colorimetry::BT2020_RGB))
    return SDRBT2020Support::Display;
  return SDRBT2020Support::Available;
}

DVBRIDGE::LldvConnectorStatus CWinSystemGbm::GetLldvConnectorStatus(bool snapshot) const
{
  DVBRIDGE::LldvConnectorStatus status;
  decltype(m_lldvRejectedTimings) rejectedTimings{};
  auto* connector = m_DRM ? m_DRM->GetConnector() : nullptr;
  if (connector && connector->IsConnected())
  {
    std::lock_guard lock(m_lldvEdidMutex);
    status.connectorId = *connector->GetConnectorId();
    status.edidGeneration = m_lldvEdidGeneration;
    if (status.connectorId == m_lldvEdidConnector)
    {
      status.display = m_lldvDisplay;
      rejectedTimings = m_lldvRejectedTimings;
    }
    status.sink = status.display.sink;
  }
  if (status.connectorId && connector->SupportsProperty("CB1_LLDV_FORMAT"))
  {
    using namespace DVBRIDGE;
    const auto native10 =
        connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", "YCbCr 4:2:2 10 useful bits");
    const auto output12 = connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", "YCbCr 4:2:2 12 bits");
    const auto packed12 =
        connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", "YCbCr 4:2:2 12 bits packed");
    const auto forced10 =
        connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", "YCbCr 4:2:2 10 bits forced");
    const bool exactFormats = connector->SupportsProperty("CB1_NATIVE_LLDV_FORMATS");
    const uint64_t nativeFormats = exactFormats ?
        connector->ReadPropertyValue("CB1_NATIVE_LLDV_FORMATS").value_or(0) : 0;
    const bool native422 = !connector->SupportsProperty("CB1_NATIVE_LLDV_422") ||
        connector->ReadPropertyValue("CB1_NATIVE_LLDV_422") == 1;
    const auto active = connector->ReadPropertyValue("CB1_LLDV_FORMAT");

    for (const auto format : LLDV_PREFERRED)
    {
      const auto value = connector->GetPropertyEnumValue("CB1_LLDV_FORMAT", LldvNativeEnumName(format));
      if (!value)
        continue;
      const bool tenBits = LldvUses10Bits(format);
      const bool capable = exactFormats ? *value < 64 && (nativeFormats & (uint64_t{1} << *value)) :
          format == LldvFormat::Y422_10 ? native422 :
          format == LldvFormat::Y422_12 || format == LldvFormat::Rgb10;
      if (capable && (tenBits ? HasLldvRgb10Scanout() : HasLldvRgb16Scanout()))
      {
        status.driverFormats |= LldvBit(format);
        if (format == LldvFormat::Y422_10) status.rgb10Scanout = true;
        if (format == LldvFormat::Y422_12) status.rgb16Scanout = true;
      }
      if (active == value &&
          ((format != LldvFormat::Y422_12 &&
            !connector->SupportsProperty("CB1_NATIVE_LLDV_ACTIVE")) ||
           connector->ReadPropertyValue("CB1_NATIVE_LLDV_ACTIVE") == (exactFormats ? *value : 1)))
      {
        status.active = format;
        status.activeRgbScanoutBits = tenBits ? 10 : 16;
      }
    }

    status.packed42212 = packed12.has_value();
    status.output42212 = output12.has_value();
    if (native10) status.driverFormats |= LldvBit(LldvFormat::Y422_10);
    if (output12 || status.packed42212) status.driverFormats |= LldvBit(LldvFormat::Y422_12);
    status.forced42210 = forced10.has_value();

    if (active && active == native10)
      status.active = LldvFormat::Y422_10;
    else if (active && (active == output12 || active == packed12))
    {
      status.active = LldvFormat::Y422_12;
      status.activeExperimentalPacked12 = active == packed12;
    }
    else if (active && active == forced10)
    {
      status.active = LldvFormat::Y422_10;
      status.activeForced = true;
    }
    status.cta93FractionalOnly =
        connector->GetPropertyValue("CB1_LLDV_CTA93_23976_ONLY").value_or(0);
    const int timing = LldvTimingIndex(m_DRM->GetSelectedMode(), status.cta93FractionalOnly);
    status.currentTiming = timing >= 0;
    if (status.currentTiming)
      status.currentRejected = rejectedTimings[timing];
    // A format is refused for mode-switch admission only if every eligible
    // timing refused it. The exact target is checked before playback starts.
    unsigned rejectedInEveryMode = status.currentTiming ? status.currentRejected : ~0U;
    for (int i = 0; i < connector->GetModesCount(); ++i)
    {
      const int available =
          LldvTimingIndex(connector->GetModeForIndex(i), status.cta93FractionalOnly);
      if (available < 0)
        continue;
      status.availableTiming = true;
      rejectedInEveryMode &= rejectedTimings[available];
      if (snapshot)
        status.modeRejections.emplace_back(i, rejectedTimings[available]);
      if (!snapshot && !rejectedInEveryMode)
        break;
    }
    if (status.availableTiming)
      status.availableRejected = rejectedInEveryMode;
    status.connectorId = *connector->GetConnectorId();
    const auto* atomic = dynamic_cast<const CDRMAtomic*>(m_DRM.get());
    status.timingCommitted = status.currentTiming && atomic && atomic->IsActive() &&
                             !atomic->HasPendingCB1Page() && atomic->IsSelectedModeCommitted();
  }
  return status;
}
#endif

CHDRCapabilities CWinSystemGbm::GetDisplayHDRCapabilities() const
{
  if (!m_info)
    return {};

  CHDRCapabilities caps;

  if (m_info->SupportsEOTF(UTILS::Eotf::PQ))
    caps.SetHDR10();

  if (m_info->SupportsEOTF(UTILS::Eotf::HLG))
    caps.SetHLG();

  if (m_info->SupportsDolbyVision())
    caps.SetDolbyVision();

  if (m_info->HDR10PlusVersion() >= 0)
    caps.SetHDR10Plus();

  return caps;
}
