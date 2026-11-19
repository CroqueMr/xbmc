/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "DVBridgeEdid.h"

#include <cstdint>

namespace DVBRIDGE
{
// Stable preference values. A request of zero means off in the driver.
enum class LldvFormat { Auto, Y422_10, Y422_12, Rgb10, Rgb12, Y444_10, Y444_12 };
enum class LldvReason { Ready, Edid, Display, Driver, Timing };

constexpr LldvFormat LldvFormatPreference(int value)
{
  return value >= 0 && value <= static_cast<int>(LldvFormat::Y444_12)
             ? static_cast<LldvFormat>(value) : LldvFormat::Auto;
}

constexpr unsigned LldvBit(LldvFormat format)
{
  return 1U << static_cast<unsigned>(format);
}

constexpr unsigned LLDV_422 = LldvBit(LldvFormat::Y422_10) | LldvBit(LldvFormat::Y422_12);
constexpr LldvFormat LLDV_PREFERRED[] = {LldvFormat::Y422_10, LldvFormat::Rgb10,
                                        LldvFormat::Y444_10, LldvFormat::Y422_12,
                                        LldvFormat::Rgb12, LldvFormat::Y444_12};

constexpr bool LldvUses10Bits(LldvFormat format)
{
  return format == LldvFormat::Y422_10 || format == LldvFormat::Rgb10 ||
         format == LldvFormat::Y444_10;
}

constexpr bool LldvRequiresForcing(LldvFormat format, unsigned driverFormats, bool forced42210)
{
  return format == LldvFormat::Y422_10 && forced42210 &&
         !(driverFormats & LldvBit(format));
}

inline LldvReason LldvFormatReason(dvbridge_sink_caps sink, LldvFormat format,
                                   unsigned driverFormats, bool currentTiming,
                                   bool forced42210 = false)
{
  if (sink.status != DV_EDID_STANDARD && sink.status != DV_EDID_LLDV_ONLY &&
      sink.status != DV_EDID_NO_DV)
    return LldvReason::Edid;
  const bool y422 = (LldvBit(format) & LLDV_422) != 0;
  const unsigned bits = LldvUses10Bits(format) ? 10 : 12;
  if (y422 ? !dvbridge_lldv_422(sink) : dvbridge_lldv_444_bits(sink) != bits)
    return LldvReason::Display;
  if (!(driverFormats & LldvBit(format)) && !LldvRequiresForcing(format, driverFormats, forced42210))
    return LldvReason::Driver;
  return currentTiming ? LldvReason::Ready : LldvReason::Timing;
}

inline LldvFormat ChooseLldvFormat(dvbridge_sink_caps sink, unsigned driverFormats,
                                  bool currentTiming, bool forced42210 = false)
{
  const bool readyForced = LldvRequiresForcing(LldvFormat::Y422_10, driverFormats, forced42210) &&
      LldvFormatReason(sink, LldvFormat::Y422_10, driverFormats, currentTiming, true) ==
          LldvReason::Ready;
  for (const auto format : LLDV_PREFERRED)
    if (LldvFormatReason(sink, format, driverFormats, currentTiming) == LldvReason::Ready)
      return readyForced && !LldvUses10Bits(format) ? LldvFormat::Y422_10 : format;
  return readyForced ? LldvFormat::Y422_10 : LldvFormat::Auto;
}

// The connector and transport used to pack a particular framebuffer.
struct LldvTransport
{
  uint32_t connectorId{0};
  LldvFormat format{LldvFormat::Auto};
  bool forced{false};
};

struct LldvConnectorStatus
{
  dvbridge_sink_caps sink{};
  unsigned driverFormats{0};
  bool currentTiming{false};
  LldvFormat active{LldvFormat::Auto};
  bool forced42210{false};
  bool activeForced{false};
  uint32_t connectorId{0};
  bool timingCommitted{false};
};
} // namespace DVBRIDGE
