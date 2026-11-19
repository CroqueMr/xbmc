/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "DVMetadataBuffer.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#ifdef HAVE_DVBRIDGE
#include "dvbridge_policy.h"
#include "cb1_cmv40.h"
#endif

namespace DVBRIDGE
{
enum class OutputMode : int { Standard = 0, HDR10 = 1, Disabled = 2, EnhancedDV = 3, MasterReference = 4, AdaptiveB = 5, SDR = 6, PlayerLed = 7 };
enum class HDR10SourceMode : int { Native = 0, ReferenceDV = 1, EnhancedDV = 2, HDR10PlusDV = 3, SDR = 4 };
enum class HLGSourceMode : int { Native = 0, ReferenceDV = 1 };

// Decoder ingress sees the last complete frame-boundary preference snapshot.
inline std::atomic<OutputMode> playbackMode{OutputMode::Standard};
inline std::atomic<HDR10SourceMode> hdr10PlaybackMode{HDR10SourceMode::Native};
inline std::atomic<HDR10SourceMode> hdr10PlusPlaybackMode{HDR10SourceMode::Native};
inline std::atomic<HDR10SourceMode> hlgPlaybackMode{HDR10SourceMode::Native};

#ifdef HAVE_DVBRIDGE
inline int PolicyCode(const dvbridge_policy& policy)
{
  if (policy.mode==DVBRIDGE_MODE_PLAYER_LED) return 9;
  if (policy.mode==DVBRIDGE_MODE_SDR) return 8;
  if (policy.mode==DVBRIDGE_MODE_HDR10PLUS) return 7;
  return policy.mode==DVBRIDGE_MODE_ENHANCED_DV && policy.adaptive!=DVBRIDGE_ADAPTIVE_A ? 6 : int(policy.mode);
}
#endif

struct OutputPreferences
{
  OutputMode output{OutputMode::Standard};
  HDR10SourceMode hdr10Source{HDR10SourceMode::Native};
  HDR10SourceMode hdr10PlusSource{HDR10SourceMode::Native};
  HLGSourceMode hlgSource{HLGSourceMode::Native};
  int sdrPreset{0};
  double sdrPeak{100}, sdrGamma{2.4}, sdrBlack{0};
  int method{0}, panel{0}, gamut{0};
  double peak{0};
  double expertPeak{}, expertBlack{};
  int expertGamut{};
  int lldvFormat{};
  bool optimizedDV{true};
  bool masterIPT{false};
  bool disableL2Trims{false};
  int cmv40Mode{}, cmv40Trigger{}, cmv40Headroom{20};
  int felUpscaling{0};
  bool processingOptimizations{false};
  uint64_t revision{1};

  bool ProfileValid() const
  {
    return std::isfinite(peak) && peak > 0 && peak <= 10000 &&
        panel >= 0 && panel <= 2 && gamut >= 1 && gamut <= 3;
  }

  bool ExpertProfileValid() const
  {
    return std::isfinite(expertPeak) && expertPeak>=50 && expertPeak<=10000 &&
        std::isfinite(expertBlack) && expertBlack>=0 && expertBlack<=0.5 &&
        expertGamut>=1 && expertGamut<=3;
  }

  bool SameValues(const OutputPreferences& other) const
  {
    return output == other.output && hdr10Source == other.hdr10Source &&
        hdr10PlusSource == other.hdr10PlusSource && hlgSource == other.hlgSource &&
        sdrPreset == other.sdrPreset &&
        std::bit_cast<uint64_t>(sdrPeak) == std::bit_cast<uint64_t>(other.sdrPeak) &&
        std::bit_cast<uint64_t>(sdrGamma) == std::bit_cast<uint64_t>(other.sdrGamma) &&
        std::bit_cast<uint64_t>(sdrBlack) == std::bit_cast<uint64_t>(other.sdrBlack) && method == other.method &&
        panel == other.panel && gamut == other.gamut && optimizedDV == other.optimizedDV &&
        masterIPT == other.masterIPT &&
        disableL2Trims == other.disableL2Trims &&
        cmv40Mode == other.cmv40Mode && cmv40Trigger == other.cmv40Trigger &&
        cmv40Headroom == other.cmv40Headroom &&
        felUpscaling == other.felUpscaling &&
        processingOptimizations == other.processingOptimizations &&
        ((output!=OutputMode::PlayerLed && (output!=OutputMode::HDR10 || method!=1)) ||
         (expertPeak == other.expertPeak && expertBlack == other.expertBlack && expertGamut == other.expertGamut)) &&
        (output!=OutputMode::PlayerLed || lldvFormat==other.lldvFormat) &&
        std::bit_cast<uint64_t>(peak) == std::bit_cast<uint64_t>(other.peak);
  }

#ifdef HAVE_DVBRIDGE
  bool ProfileValid(dvbridge_mode mode) const
  {
    if (mode==DVBRIDGE_MODE_SDR)
    {
      if (sdrPreset==0 || sdrPreset==1) return true;
      return sdrPreset==2 && std::isfinite(sdrPeak) && sdrPeak>=20 && sdrPeak<=500 &&
          std::isfinite(sdrGamma) && sdrGamma>=1.8 && sdrGamma<=2.8 &&
          std::isfinite(sdrBlack) && sdrBlack>=0 && sdrBlack<=5 && sdrBlack<sdrPeak;
    }
    return mode==DVBRIDGE_MODE_HDR10_EXPERT || mode==DVBRIDGE_MODE_PLAYER_LED ?
        ExpertProfileValid() : ProfileValid();
  }
  bool L2TrimsDisabled(dvbridge_mode mode, bool hdr10AI = false) const
  {
    return disableL2Trims && !hdr10AI &&
        (mode == DVBRIDGE_MODE_STANDARD || mode == DVBRIDGE_MODE_MASTER_REFERENCE ||
         mode == DVBRIDGE_MODE_PLAYER_LED);
  }
  cb1_cmv40_options CMv40Options(dvbridge_mode mode, bool hdr10AI = false) const
  {
    if (hdr10AI || (mode != DVBRIDGE_MODE_STANDARD && mode != DVBRIDGE_MODE_MASTER_REFERENCE &&
                    mode != DVBRIDGE_MODE_PLAYER_LED)) return {};
    return {static_cast<cb1_cmv40_mode>(cmv40Mode), static_cast<unsigned>(cmv40Trigger),
        static_cast<unsigned>(cmv40Headroom), mode==DVBRIDGE_MODE_PLAYER_LED ? expertPeak : peak};
  }
  dvbridge_policy Policy() const
  {
    dvbridge_policy result{};
    result.revision = revision;
    result.sdr={static_cast<dvbridge_sdr_preset>(sdrPreset),sdrPeak,sdrGamma,sdrBlack};
    switch (output)
    {
      case OutputMode::PlayerLed: result.mode = DVBRIDGE_MODE_PLAYER_LED; break;
      case OutputMode::SDR: result.mode = DVBRIDGE_MODE_SDR; break;
      case OutputMode::Disabled: result.mode = DVBRIDGE_MODE_DISABLED; break;
      case OutputMode::Standard: result.mode = DVBRIDGE_MODE_STANDARD; break;
      case OutputMode::EnhancedDV:
      case OutputMode::AdaptiveB:
        result.mode=DVBRIDGE_MODE_ENHANCED_DV;
        result.adaptive=DVBRIDGE_ADAPTIVE_B_FP16;
        break;
      case OutputMode::MasterReference:
        result.mode = DVBRIDGE_MODE_MASTER_REFERENCE; result.master_ipt = masterIPT; break;
      case OutputMode::HDR10:
        result.mode = method == 2 ? DVBRIDGE_MODE_HDR10PLUS :
            method == 1 ? DVBRIDGE_MODE_HDR10_EXPERT : DVBRIDGE_MODE_HDR10_BASIC;
        break;
    }
    // Basic/native paths never consume calibration. Keep invalid user values
    // in this snapshot, rather than silently completing the stored profile.
    if (result.mode == DVBRIDGE_MODE_HDR10_EXPERT || result.mode == DVBRIDGE_MODE_PLAYER_LED)
    {
      result.tv={expertPeak,DVBRIDGE_PANEL_OLED,static_cast<dvbridge_gamut>(expertGamut)};
      result.expert_black_nits=expertBlack;
    }
    else if (result.mode == DVBRIDGE_MODE_ENHANCED_DV)
      result.tv = {peak, static_cast<dvbridge_panel>(panel), static_cast<dvbridge_gamut>(gamut)};
    return result;
  }
  HDR10SourceMode HDR10Mode(bool hdr10Plus) const
  {
    return hdr10Plus ? hdr10PlusSource : hdr10Source;
  }
  dvbridge_policy HDR10Policy(bool hdr10Plus = false) const
  {
    auto result = Policy();
    result.master_ipt=false;
    result.expert_black_nits=0;
    const auto source = HDR10Mode(hdr10Plus);
    result.mode = source == HDR10SourceMode::ReferenceDV ||
        source == HDR10SourceMode::HDR10PlusDV ? DVBRIDGE_MODE_STANDARD :
        source == HDR10SourceMode::EnhancedDV ? DVBRIDGE_MODE_ENHANCED_DV :
        source == HDR10SourceMode::SDR ? DVBRIDGE_MODE_SDR : DVBRIDGE_MODE_DISABLED;
    // Same scene expansion as Adaptive DV, with predicted rather than authored guidance.
    result.adaptive = result.mode == DVBRIDGE_MODE_ENHANCED_DV ?
        DVBRIDGE_ADAPTIVE_B_FP16 : DVBRIDGE_ADAPTIVE_A;
    result.tv = result.mode == DVBRIDGE_MODE_ENHANCED_DV ?
        dvbridge_tv_profile{peak,static_cast<dvbridge_panel>(panel),static_cast<dvbridge_gamut>(gamut)} : dvbridge_tv_profile{};
    return result;
  }
#endif
};

inline std::mutex& OutputSettingsMutex()
{
  static std::mutex mutex;
  return mutex;
}

// Protected by OutputSettingsMutex. The settings store remains the sole default owner.
struct OutputSelectors
{
  OutputMode output{OutputMode::Standard};
  HDR10SourceMode hdr10Source{HDR10SourceMode::Native}, hdr10PlusSource{HDR10SourceMode::Native};
  HLGSourceMode hlgSource{HLGSourceMode::Native};
  int method{};
  void Apply(OutputPreferences& preferences) const
  {
    preferences.output=output;
    preferences.method=method;
    preferences.hdr10Source=hdr10Source;
    preferences.hdr10PlusSource=hdr10PlusSource;
    preferences.hlgSource=hlgSource;
  }
};
inline std::optional<OutputSelectors> activeOutputSelectors;

// Caller holds OutputSettingsMutex; no selector-setting callbacks take this lock.
inline OutputSelectors ReadDefaultOutputSelectors()
{
  OutputSelectors result;
  const auto component=CServiceBroker::GetSettingsComponent();
  const auto settings=component ? component->GetSettings() : nullptr;
  if (!settings) return result;
  const int output=settings->GetInt("dvbridge.outputmode");
  result.output=output==3 ? OutputMode::AdaptiveB :
      output>=0 && output<=static_cast<int>(OutputMode::PlayerLed) ? static_cast<OutputMode>(output) : OutputMode::Standard;
  const int method=settings->GetInt("dvbridge.hdr10method");
  result.method=method>=0 && method<=2 ? method : 0;
  const int hdr10=settings->GetInt("dvbridge.hdr10source");
  result.hdr10Source=(hdr10>=0 && hdr10<=2) || hdr10==4 ?
      static_cast<HDR10SourceMode>(hdr10) : HDR10SourceMode::Native;
  const int plus=settings->GetInt("dvbridge.hdr10plussource");
  result.hdr10PlusSource=plus==3 ? HDR10SourceMode::HDR10PlusDV :
      plus==4 ? HDR10SourceMode::SDR : HDR10SourceMode::Native;
  result.hlgSource=settings->GetInt("dvbridge.hlgsource")==1 ? HLGSourceMode::ReferenceDV : HLGSourceMode::Native;
  return result;
}

#ifdef CB1_HAS_HDR10_AI
// Protected by OutputSettingsMutex; confirmation retries without changing settings.
inline uint64_t hdr10Reselection{};
#endif

inline OutputPreferences FrameOutputSettings(bool hdr10Plus = false, bool hlg = false)
{
  static OutputPreferences previous;
#ifdef CB1_HAS_HDR10_AI
  static uint64_t previousReselection{};
#endif
  std::lock_guard<std::mutex> lock(OutputSettingsMutex());
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  OutputPreferences next;
  if (settings)
  {
    next.sdrPreset=settings->GetInt("dvbridge.sdrpreset");
    next.sdrPeak=settings->GetNumber("dvbridge.sdrpeak");
    next.sdrGamma=settings->GetNumber("dvbridge.sdrgamma");
    next.sdrBlack=settings->GetNumber("dvbridge.sdrblack");
    next.lldvFormat=settings->GetInt("dvbridge.lldvformatvalue");
    {
      next.expertPeak=settings->GetNumber("dvbridge.expertpeak");
      next.expertGamut=settings->GetInt("dvbridge.expertgamut");
      const int black=settings->GetInt("dvbridge.expertblack");
      next.expertBlack=black==0 ? 0 : black==1 ? 0.005 : black==2 ? 0.05 :
          black==3 ? settings->GetNumber("dvbridge.expertblackcustom") : -1;
    }
    next.peak = settings->GetNumber("dvbridge.tvpeak");
    next.panel = settings->GetInt("dvbridge.tvpanel");
    next.gamut = settings->GetInt("dvbridge.tvgamut");
    next.felUpscaling = settings->GetInt("dvbridge.felupscaling");
    next.masterIPT = settings->GetInt("dvbridge.masteroutput")==1;
    next.disableL2Trims = settings->GetBool("dvbridge.disablel2");
    next.cmv40Mode = settings->GetInt("dvbridge.cmv40");
    next.cmv40Trigger = settings->GetInt("dvbridge.cmv40trigger");
    next.cmv40Headroom = settings->GetInt("dvbridge.cmv40headroom");
  }
  if (next.cmv40Mode < 0 || next.cmv40Mode > 4) next.cmv40Mode = 0;
  if (next.cmv40Trigger != 1000 && next.cmv40Trigger != 2000 &&
      next.cmv40Trigger != 4000 && next.cmv40Trigger != 10000) next.cmv40Trigger = 0;
  if (next.cmv40Headroom < 0 || next.cmv40Headroom > 50) next.cmv40Headroom = 20;
  if (next.felUpscaling < 0 || next.felUpscaling > 2) next.felUpscaling = 0;
  // Only selectors belong to the file. Calibration and tuning stay live.
  (activeOutputSelectors ? *activeOutputSelectors : ReadDefaultOutputSelectors()).Apply(next);
  next.revision = previous.revision;
  bool changed = !next.SameValues(previous);
#ifdef CB1_HAS_HDR10_AI
  changed |= previousReselection != hdr10Reselection;
  previousReselection = hdr10Reselection;
#endif
  if (changed) next.revision = NextRevision();
  previous = next;
  playbackMode.store(next.output);
  hdr10PlaybackMode.store(next.hdr10Source);
  hdr10PlusPlaybackMode.store(next.hdr10PlusSource);
  hlgPlaybackMode.store(next.hlgSource==HLGSourceMode::ReferenceDV ? HDR10SourceMode::ReferenceDV : HDR10SourceMode::Native);
  // Resolve only the returned copy; alternating inputs must not churn revisions.
  if (hdr10Plus) next.hdr10Source = next.hdr10PlusSource;
  if (hlg) next.hdr10Source = next.hlgSource == HLGSourceMode::ReferenceDV ?
      HDR10SourceMode::ReferenceDV : HDR10SourceMode::Native;
  return next;
}

inline void SelectOutputSettings()
{
  {
    std::lock_guard lock(OutputSettingsMutex());
    activeOutputSelectors=ReadDefaultOutputSelectors();
  }
  FrameOutputSettings();
}

inline bool EnhancedInfoEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.enhancedinfo");
}
inline bool PlaybackMenuEnabled()
{
  return true;
}
inline bool AsyncPreparationEnabled()
{
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings && settings->GetBool("dvbridge.asyncpreparation");
}
inline bool FELHardwareUpscalingEnabled()
{
  return true;
}
} // namespace DVBRIDGE
