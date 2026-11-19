/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "DVStreamTelemetry.h"
#include "utils/DVBridgeLldv.h"
#include "cb1_master_reference.h"
#include "dvbridge_creative.h"

namespace DVBRIDGE
{
// Call after confirmed presentation with the packed framebuffer's binding and transport.
// Match both against the presented picture and current connector readback.
inline std::optional<StreamSnapshot::Output> PlayerLedOutput(
    cb1_master_status rendered, SourceBinding mapped, SourceBinding presented,
    LldvTransport packed, const LldvConnectorStatus& connector,
    const dvbridge_creative_plan& plan, bool fel, bool felUpscaled = false)
{
  const auto format = connector.active;
  if (rendered != CB1_MASTER_READY || !presented.Valid() || mapped != presented ||
      !packed.connectorId || packed.connectorId != connector.connectorId ||
      packed.edidGeneration != connector.edidGeneration || packed.edidGeneration != plan.policy.lldv_edid_generation ||
      connector.display.status != LldvEdidStatus::Ready ||
      !connector.timingCommitted || packed.format != format ||
      packed.forced != connector.activeForced ||
      packed.rgbScanoutBits != connector.activeRgbScanoutBits ||
      plan.status == DVBRIDGE_CREATIVE_INVALID || plan.status == DVBRIDGE_CREATIVE_UNSUPPORTED ||
      !(LldvBit(format) & LLDV_IMPLEMENTED) ||
      (connector.activeForced && format != LldvFormat::Y422_10) ||
      LldvFormatReason(connector.sink, format, connector.driverFormats, connector.currentTiming,
                       connector.activeForced && connector.forced42210) != LldvReason::Ready ||
      (connector.activeForced && !connector.forced42210))
    return {};

  using Trims = VideoProcessingInfo::PlayerLed;
  constexpr unsigned l2 = 1u << 2, l8 = 1u << 8;
  const unsigned family = plan.applied_levels & l8 ? l8 : plan.applied_levels & l2;
  unsigned groups = 0;
  if (family == l8)
  {
    dvbridge_cm4_coefficients cm4{};
    if (dvbridge_creative_cm4_coefficients(&cm4, &plan))
    {
      if (cm4.sop[0] != 1 || cm4.sop[1] != 0 || cm4.sop[2] != 1) groups |= Trims::SOP;
      if (cm4.chroma_base != 1 || cm4.saturation_gain != 1) groups |= Trims::Colour;
      if (cm4.mid_exponent != 1) groups |= Trims::Mid;
      if (cm4.clip_strength != 0) groups |= Trims::Clip;
    }
  }
  else if (family == l2)
  {
    if (plan.codes[0] != 2048 || plan.codes[1] != 2048 || plan.codes[2] != 2048)
      groups |= Trims::SOP;
    // The Expert L2 colour shader gates both controls on non-neutral saturation.
    if (plan.codes[4] != 2048) groups |= Trims::Colour;
  }

  StreamSnapshot::Output output;
  output.flags = CDVPlaybackInfo::HDMI;
  if (fel) output.flags |= CDVPlaybackInfo::FEL;
  if (plan.cm4 || plan.cmv40_status == CB1_CMV40_APPEND) output.flags |= CDVPlaybackInfo::CM4;
  output.requested = output.effective = CDVPlaybackInfo::PlayerLedMode;
  output.stream = presented.stream;
  output.picture = presented.picture;
  output.revision = presented.revision;
  output.target = static_cast<float>(plan.policy.tv.peak_nits);
  output.panel = plan.policy.tv.panel;
  output.gamut = plan.policy.tv.gamut;
  output.displayBlack = plan.policy.expert_black_nits;
  output.bits = LldvUses10Bits(format) ? 10 : 12;
  output.minimumLinkBits = format == LldvFormat::Rgb10 ? 10 : 12;
  output.creativeKnown = true;
  output.creativeApplied = (plan.applied_levels & ~(l2 | l8)) | (groups ? family : 0);
  output.creativeStatus = plan.status;
  output.creativeReason = plan.reason;
  output.felUpscaling = fel && felUpscaled ? 2 : -1;
  output.lldv = Trims{{plan.analysis[1], plan.analysis[2]}, groups,
                      plan.cmv40_status == CB1_CMV40_APPEND, plan.policy.lldv_edid_version,
                      connector.display.primaries,
                      connector.display.reported, connector.activeForced, format == LldvFormat::Rgb10};
  return output;
}
} // namespace DVBRIDGE
