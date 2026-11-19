/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DVOutputSettings.h"
#include "DVPlaybackInfo.h"
#ifdef HAVE_DVBRIDGE
#include <algorithm>
#include <array>

namespace DVBRIDGE
{
enum class OutputRoute { Vanilla, NativeDV, HDR10, SDR, PlayerLed };
struct OutputSelection
{
  OutputRoute route{OutputRoute::Vanilla};
  dvbridge_policy policy{};
  OutputReason reason{OutputReason::None};
  int lldvFormat{};
  uint64_t lldvEdidGeneration{};
  bool lldvRgb10Scanout{};
};

inline OutputSelection SelectOutput(const OutputPreferences& preferences,
                                    const OutputCapabilities& caps)
{
  OutputSelection result;
  if (caps.stereoSource)
    return {OutputRoute::Vanilla, {}, OutputReason::SourceIneligible};
  if (caps.sourceKnown && (caps.isHLG || caps.isHDR10) && !caps.isDV)
  {
    const auto source = caps.isHLG ? preferences.HLGMode() : preferences.HDR10Mode(caps.hdr10PlusSource);
    if (source == HDR10SourceMode::Native) return result;
    if (caps.hdr10PlusSource && source != HDR10SourceMode::HDR10PlusDV && source != HDR10SourceMode::SDR)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    if (source == HDR10SourceMode::HDR10PlusDV && !caps.isHDR10Plus)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    auto policy = preferences.HDR10PolicyFor(source);
    if (policy.mode == DVBRIDGE_MODE_DISABLED)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    if (policy.mode == DVBRIDGE_MODE_SDR)
    {
      if (!preferences.ProfileValid(policy.mode)) return {OutputRoute::Vanilla,{},OutputReason::SDRTargetInvalid};
      if (!caps.processingKnown || !caps.processing[policy.mode].usable)
        return {OutputRoute::Vanilla,{},!caps.processingKnown ? OutputReason::ProcessingPending : caps.processing[policy.mode].reason!=OutputReason::None ?
            caps.processing[policy.mode].reason : OutputReason::NoGPUProcessing};
      return {OutputRoute::SDR,policy,OutputReason::None};
    }
    if (policy.mode != DVBRIDGE_MODE_STANDARD && !preferences.ProfileValid(policy.mode))
      return {OutputRoute::Vanilla,{},OutputReason::TVProfileRequired};
    if (!caps.nativeDV)
      return {OutputRoute::Vanilla,{},OutputReason::NoDVTransport};
    if (!caps.processingKnown || !caps.processing[policy.mode].usable)
      return {OutputRoute::Vanilla,{},!caps.processingKnown ? OutputReason::ProcessingPending : caps.processing[policy.mode].reason!=OutputReason::None ?
          caps.processing[policy.mode].reason : OutputReason::NoGPUProcessing};
    return {OutputRoute::NativeDV,policy,OutputReason::None};
  }
  if (!caps.sourceKnown || !caps.isDV || preferences.output == OutputMode::Disabled)
    return result;
  auto policy = preferences.Policy();
  result.policy=policy;
  if (preferences.hdr10TransportFallback)
  {
    if (caps.HDR10DepthLimited())
      return {OutputRoute::Vanilla, policy, OutputReason::HDR10LinkDepthTooLow};
    if (!caps.hdr10)
      return {OutputRoute::Vanilla, policy, OutputReason::NoHDR10Transport};
    auto hdr10 = preferences;
    hdr10.output = OutputMode::HDR10;
    hdr10.method = 0;
    policy = hdr10.Policy();
    if (!caps.processingKnown)
      return {OutputRoute::Vanilla, policy, OutputReason::ProcessingPending};
    const auto& admission = caps.processing[policy.mode];
    if (!admission.usable)
      return {OutputRoute::Vanilla, policy, admission.reason != OutputReason::None ?
          admission.reason : OutputReason::NoGPUProcessing};
    return {OutputRoute::HDR10, policy, preferences.output == OutputMode::PlayerLed ?
        OutputReason::NoPlayerLedTransport : OutputReason::NoDVTransport};
  }
  if (policy.mode == DVBRIDGE_MODE_PLAYER_LED)
  {
    if (!caps.playerLed) result.reason = OutputReason::NoPlayerLedTransport;
    else if (!caps.processingKnown) result.reason = OutputReason::ProcessingPending;
    else if (const auto& mode = caps.processing[policy.mode]; !mode.usable)
      result.reason = mode.reason == OutputReason::None ? OutputReason::NoGPUProcessing : mode.reason;
    else
    {
      result.route = OutputRoute::PlayerLed;
      const auto& display = caps.lldvDisplay;
      result.policy.tv = {display.peak, DVBRIDGE_PANEL_UNSET, DVBRIDGE_GAMUT_BT2020};
      result.policy.expert_black_nits = display.black;
      result.policy.lldv_edid_generation = caps.lldvEdidGeneration;
      result.policy.lldv_edid_version = display.sink.version;
      std::copy(display.primaries.begin(), display.primaries.end(), result.policy.lldv_primaries);
      result.lldvEdidGeneration = caps.lldvEdidGeneration;
      result.lldvRgb10Scanout = caps.lldvRgb10Scanout;
    }
    result.lldvFormat = preferences.lldvFormat;
    return result;
  }
  if (policy.mode == DVBRIDGE_MODE_SDR)
  {
    if (!preferences.ProfileValid(policy.mode)) return {OutputRoute::Vanilla,{},OutputReason::SDRTargetInvalid};
    if (!caps.processingKnown || !caps.processing[policy.mode].usable)
      return {OutputRoute::Vanilla,{},!caps.processingKnown ? OutputReason::ProcessingPending : caps.processing[policy.mode].reason!=OutputReason::None ?
          caps.processing[policy.mode].reason : OutputReason::NoGPUProcessing};
    return {OutputRoute::SDR,policy,OutputReason::None};
  }
  if ((policy.mode == DVBRIDGE_MODE_HDR10_BASIC || policy.mode == DVBRIDGE_MODE_HDR10_EXPERT) &&
      caps.HDR10DepthLimited())
    return {OutputRoute::Vanilla, policy, OutputReason::HDR10LinkDepthTooLow};
  // Display configuration is known before the first GPU import.
  if (preferences.output == OutputMode::HDR10 && policy.mode == DVBRIDGE_MODE_HDR10_EXPERT &&
      !preferences.ProfileValid(policy.mode))
    return {OutputRoute::Vanilla, {}, OutputReason::TVProfileRequired};
  if (!caps.processingKnown)
  {
    result.reason = OutputReason::ProcessingPending;
    return result;
  }
  if (policy.mode == DVBRIDGE_MODE_HDR10PLUS && !caps.hdr10plus)
    return {OutputRoute::Vanilla, policy, OutputReason::NoHDR10PlusTransport};
  const auto admission = [&](dvbridge_mode mode) {
    if ((mode == DVBRIDGE_MODE_HDR10_EXPERT || mode == DVBRIDGE_MODE_ENHANCED_DV) &&
        !preferences.ProfileValid(mode))
      return ModeAdmission{false, OutputReason::TVProfileRequired};
    auto value = caps.processing[mode];
    if (!value.usable && value.reason == OutputReason::None)
      value.reason = OutputReason::NoGPUProcessing;
    return value;
  };
  const bool requestedDV = preferences.output != OutputMode::HDR10;
  auto candidate = admission(policy.mode);
  if ((requestedDV ? caps.nativeDV : caps.hdr10) && candidate.usable)
    return {requestedDV ? OutputRoute::NativeDV : OutputRoute::HDR10, policy, OutputReason::None};
  result.reason = !(requestedDV ? caps.nativeDV : caps.hdr10) ?
      (requestedDV ? OutputReason::NoDVTransport : OutputReason::NoHDR10Transport) : candidate.reason;
  return {OutputRoute::Vanilla, policy, result.reason};
}
} // namespace DVBRIDGE
#endif
