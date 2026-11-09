/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DVOutputSettings.h"
#include "DVPlaybackInfo.h"
#ifdef HAVE_DVBRIDGE
#include <array>

namespace DVBRIDGE
{
enum class OutputRoute { Vanilla, NativeDV, HDR10, SDR };
struct OutputSelection
{
  OutputRoute route{OutputRoute::Vanilla};
  dvbridge_policy policy{};
  OutputReason reason{OutputReason::None};
};

inline OutputSelection SelectOutput(const OutputPreferences& preferences,
                                    const OutputCapabilities& caps)
{
  OutputSelection result;
  if (caps.stereoSource)
    return {OutputRoute::Vanilla, {}, OutputReason::SourceIneligible};
  if (caps.sourceKnown && caps.isHLG && !caps.isDV)
  {
    if (preferences.hlgSource == HLGSourceMode::Native) return result;
    auto policy=preferences.Policy();
    policy.mode=DVBRIDGE_MODE_STANDARD; policy.master_ipt=false; policy.tv={};
    policy.adaptive=DVBRIDGE_ADAPTIVE_A; policy.expert_black_nits=0;
    if (!caps.nativeDV) return {OutputRoute::Vanilla,{},OutputReason::NoDVTransport};
    if (!caps.processingKnown || !caps.processing[DVBRIDGE_MODE_STANDARD].usable)
      return {OutputRoute::Vanilla,{},!caps.processingKnown ? OutputReason::ProcessingPending : caps.processing[policy.mode].reason!=OutputReason::None ?
          caps.processing[policy.mode].reason : OutputReason::NoGPUProcessing};
    return {OutputRoute::NativeDV,policy,OutputReason::None};
  }
  if (caps.sourceKnown && caps.isHDR10 && !caps.isDV)
  {
    const auto source = preferences.HDR10Mode(caps.hdr10PlusSource);
    if (source == HDR10SourceMode::Native) return result;
    if (caps.hdr10PlusSource && source != HDR10SourceMode::HDR10PlusDV && source != HDR10SourceMode::SDR)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    if (source == HDR10SourceMode::HDR10PlusDV && !caps.isHDR10Plus)
      return {OutputRoute::Vanilla,{},OutputReason::SourceIneligible};
    auto policy = preferences.HDR10Policy(caps.hdr10PlusSource);
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
  if (policy.mode == DVBRIDGE_MODE_SDR)
  {
    if (!preferences.ProfileValid(policy.mode)) return {OutputRoute::Vanilla,{},OutputReason::SDRTargetInvalid};
    if (!caps.processingKnown || !caps.processing[policy.mode].usable)
      return {OutputRoute::Vanilla,{},!caps.processingKnown ? OutputReason::ProcessingPending : caps.processing[policy.mode].reason!=OutputReason::None ?
          caps.processing[policy.mode].reason : OutputReason::NoGPUProcessing};
    return {OutputRoute::SDR,policy,OutputReason::None};
  }
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
  {
    if (!caps.hdr10 || !caps.processing[DVBRIDGE_MODE_HDR10_BASIC].usable)
      return {OutputRoute::Vanilla, {}, OutputReason::NoHDR10PlusTransport};
    policy.mode = DVBRIDGE_MODE_HDR10_BASIC;
    return {OutputRoute::HDR10, policy, OutputReason::NoHDR10PlusTransport};
  }
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
  if (!requestedDV) return result;
  if(policy.mode==DVBRIDGE_MODE_MASTER_REFERENCE && !preferences.allowMasterFallback)
    return {OutputRoute::Vanilla,policy,result.reason};
  if (!caps.hdr10)
  {
    result.reason = OutputReason::NoHDR10Transport;
    return result;
  }
  candidate = admission(DVBRIDGE_MODE_HDR10_BASIC);
  if (!candidate.usable)
  {
    result.reason = candidate.reason;
    return result;
  }
  policy.mode = DVBRIDGE_MODE_HDR10_BASIC;
  policy.master_ipt = false;
  policy.adaptive = DVBRIDGE_ADAPTIVE_A;
  policy.tv = {};
  return {OutputRoute::HDR10, policy, result.reason};
}
} // namespace DVBRIDGE
#endif
