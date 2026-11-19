/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "DVBridgeEdid.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace DVBRIDGE
{
enum class LldvEdidStatus
{
  Missing, Incomplete, Invalid, NoDolbyVision, UnsupportedBlock, NoLowLatency,
  InvalidLuminance, InvalidPrimaries, Ready, UnsupportedGamut
};

inline const char* LldvEdidReason(LldvEdidStatus status)
{
  switch (status)
  {
    case LldvEdidStatus::Missing: return "edid-unavailable";
    case LldvEdidStatus::Incomplete: return "edid-incomplete";
    case LldvEdidStatus::Invalid: return "edid-invalid";
    case LldvEdidStatus::NoDolbyVision: return "no-dolby-vision-block";
    case LldvEdidStatus::UnsupportedBlock: return "dolby-block-unusable";
    case LldvEdidStatus::NoLowLatency: return "tv-does-not-announce-lldv";
    case LldvEdidStatus::InvalidLuminance: return "target-luminance-invalid";
    case LldvEdidStatus::InvalidPrimaries: return "target-primaries-invalid";
    case LldvEdidStatus::UnsupportedGamut: return "target-gamut-outside-bt2020";
    case LldvEdidStatus::Ready: return "target-ready";
  }
  return "unknown";
}

struct LldvDisplay
{
  dvbridge_sink_caps sink{};
  LldvEdidStatus status{LldvEdidStatus::Missing};
  size_t received{}, expected{};
  double peak{}, black{};
  std::array<double, 6> reported{}, primaries{};
};

inline double LldvPqNits(unsigned code)
{
  const double value = std::pow(code / 4095.0, 32.0 / 2523);
  return 10000 * std::pow(std::fmax(0, value - 3424.0 / 4096) /
                             (2413.0 / 128 - (2392.0 / 128) * value), 16384.0 / 2610);
}

inline double LldvEdge(double x, double y, const double* a, const double* b)
{
  return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
}

inline bool ValidLldvPrimaries(const std::array<double, 6>& primaries)
{
  for (size_t i = 0; i < 3; ++i)
    if (primaries[i * 2] <= 0 || primaries[i * 2 + 1] <= 0 ||
        primaries[i * 2] + primaries[i * 2 + 1] > 1.0 ||
        LldvEdge(.3127, .3290, &primaries[i * 2], &primaries[((i + 1) % 3) * 2]) <= 0.000001)
      return false;
  return true;
}

inline bool FitLldvPrimaries(std::array<double, 6>& primaries)
{
  constexpr double bt2020[][2] = {{.708, .292}, {.170, .797}, {.131, .046}};
  for (size_t i = 0; i < primaries.size(); i += 2)
  {
    double& x = primaries[i];
    double& y = primaries[i + 1];
    double nearestDistance = 1, nearestX = x, nearestY = y;
    bool outside = false;
    for (size_t j = 0; j < 3; ++j)
    {
      const auto* a = bt2020[j];
      const auto* b = bt2020[(j + 1) % 3];
      outside |= LldvEdge(x, y, a, b) < 0;
      const double dx = b[0] - a[0], dy = b[1] - a[1];
      const double t = std::fmax(0, std::fmin(1, ((x - a[0]) * dx + (y - a[1]) * dy) /
                                                      (dx * dx + dy * dy)));
      const double px = a[0] + t * dx, py = a[1] + t * dy;
      const double distance = std::hypot(x - px, y - py);
      if (distance < nearestDistance)
      {
        nearestDistance = distance;
        nearestX = px;
        nearestY = py;
      }
    }
    if (outside)
    {
      // Allow one 1/256 step on each xy axis at the quantized gamut boundary.
      if (nearestDistance > std::sqrt(2.0) / 256)
        return false;
      x = nearestX;
      y = nearestY;
    }
  }
  return ValidLldvPrimaries(primaries);
}

inline LldvDisplay ReadLldvDisplay(const uint8_t* edid, size_t size)
{
  LldvDisplay display;
  display.sink = dvbridge_parse_edid(edid, size);
  display.received = size;
  if (!edid || !size)
    return display;
  display.expected = size < 128 ? 128 : (size_t(edid[126]) + 1) * 128;
  if (size >= 256 && edid[126] && edid[128] == 2 && edid[129] >= 3 &&
      edid[130] >= 7 && edid[130] <= 127 && edid[132] == 0xe2 && edid[133] == 0x78 && edid[134])
    display.expected = (size_t(edid[134]) + 1) * 128;
  switch (display.sink.status)
  {
    case DV_EDID_INVALID:
      display.status = size < display.expected ? LldvEdidStatus::Incomplete : LldvEdidStatus::Invalid;
      return display;
    case DV_EDID_NO_DV:
      display.status = LldvEdidStatus::NoDolbyVision;
      return display;
    case DV_EDID_STANDARD:
    case DV_EDID_LLDV_ONLY:
      break;
    default:
      display.status = LldvEdidStatus::UnsupportedBlock;
      return display;
  }

  const auto* data = edid + display.sink.offset;
  const auto* p = data + 4;
  if (!dvbridge_lldv_422(display.sink) ||
      (display.sink.version == 1 && (p[3] & 3) != 1))
  {
    display.status = LldvEdidStatus::NoLowLatency;
    return display;
  }
  if (display.sink.version == 2)
  {
    display.black = LldvPqNits(20 * (p[1] >> 3));
    display.peak = LldvPqNits(2055 + 65 * (p[2] >> 3));
    display.reported = {.625 + (p[5] >> 3) / 256.0, .25 + (p[6] >> 3) / 256.0,
                        (p[3] >> 1) / 256.0, .5 + (p[4] >> 1) / 256.0,
                        .125 + (p[5] & 7) / 256.0, .03125 + (p[6] & 7) / 256.0};
  }
  else
  {
    const double black = (p[2] >> 1) / 127.0;
    display.black = black * black;
    display.peak = 100 + 50 * (p[1] >> 1);
    display.reported = {.625 + (p[6] >> 3) / 256.0,
        .25 + (((p[6] & 7) << 2) | ((p[5] & 1) << 1) | (p[4] & 1)) / 256.0,
        (p[4] >> 1) / 256.0, .5 + (p[5] >> 1) / 256.0,
        .125 + (p[3] >> 5) / 256.0, .03125 + ((p[3] >> 2) & 7) / 256.0};
  }
  display.primaries = display.reported;
  if (display.black >= display.peak || display.peak > 10000)
    display.status = LldvEdidStatus::InvalidLuminance;
  else if (!ValidLldvPrimaries(display.reported))
    display.status = LldvEdidStatus::InvalidPrimaries;
  else if (!FitLldvPrimaries(display.primaries))
    display.status = LldvEdidStatus::UnsupportedGamut;
  else
    display.status = LldvEdidStatus::Ready;
  return display;
}

// Stable preference values. A request of zero means off in the driver.
enum class LldvFormat { Auto, Y422_10, Y422_12, Rgb10, Rgb12, Y444_10, Y444_12 };
enum class LldvReason { Ready, Edid, Display, Driver, Timing };

constexpr int LLDV_FORCE_422_10 = 7; // Saved choice, not an HDMI format or DRM enum.
constexpr int LLDV_PACKED_422_12 = 8;
constexpr int LLDV_CHOOSE_FORMAT_LABEL = 41340;

constexpr LldvFormat LldvFormatPreference(int value)
{
  if (value == LLDV_FORCE_422_10)
    return LldvFormat::Y422_10;
  if (value == LLDV_PACKED_422_12)
    return LldvFormat::Y422_12;
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
                                  bool currentTiming)
{
  for (const auto format : LLDV_PREFERRED)
    if (LldvFormatReason(sink, format, driverFormats, currentTiming) == LldvReason::Ready)
      return format;
  return LldvFormat::Auto;
}

// The connector and transport used to pack a particular framebuffer.
struct LldvTransport
{
  uint32_t connectorId{0};
  LldvFormat format{LldvFormat::Auto};
  bool forced{false};
  uint64_t edidGeneration{};
  bool rgb10Scanout{false};
};

struct LldvConnectorStatus
{
  dvbridge_sink_caps sink{};
  unsigned driverFormats{0};
  bool currentTiming{false};
  LldvFormat active{LldvFormat::Auto};
  bool forced42210{false};
  bool activeForced{false};
  bool rgb10Scanout{false};
  bool activeRgb10Scanout{false};
  uint32_t connectorId{0};
  bool timingCommitted{false};
  bool availableTiming{false};
  LldvDisplay display;
  uint64_t edidGeneration{};
  LldvFormat rejected{LldvFormat::Auto};
  bool cta93FractionalOnly{};
  bool packed42212{};
};

inline unsigned LldvAutomaticFormats(const LldvConnectorStatus& status)
{
  const unsigned formats = status.driverFormats & LLDV_422;
  // The experimental R615 carrier is an explicit choice, like forced 10-bit.
  return status.packed42212 ? formats & ~LldvBit(LldvFormat::Y422_12) : formats;
}

inline int LldvUnavailableLabel(const LldvConnectorStatus& status, int preference,
                                bool allowModeSwitch)
{
  if (status.display.status != LldvEdidStatus::Ready)
    return status.display.status == LldvEdidStatus::UnsupportedGamut ? 41300 :
        41280 + static_cast<int>(status.display.status);
  if (!status.connectorId || (!status.driverFormats && !status.forced42210)) return 41211;
  auto format = LldvFormatPreference(preference);
  if (format == LldvFormat::Auto)
    format = ChooseLldvFormat(status.sink, LldvAutomaticFormats(status), true);
  if (format == LldvFormat::Auto) return 41213;
  if (format == LldvFormat::Y422_12 && status.packed42212)
  {
    if (!dvbridge_lldv_422_12(status.sink))
      return 41210;
    if (preference != LLDV_PACKED_422_12)
      return LLDV_CHOOSE_FORMAT_LABEL;
  }
  const auto reason = LldvFormatReason(status.sink, format, status.driverFormats, true,
      preference == LLDV_FORCE_422_10 && status.forced42210);
  if (reason != LldvReason::Ready) return 41208 + static_cast<int>(reason);
  if (!(LldvBit(format) & LLDV_422)) return 41211;
  if (!(status.currentTiming || (allowModeSwitch && status.availableTiming))) return 41212;
  return 0;
}

inline bool LldvCanChooseFormat(const LldvConnectorStatus& status)
{
  return !LldvUnavailableLabel(status, 0, true) ||
         !LldvUnavailableLabel(status, LLDV_FORCE_422_10, true) ||
         !LldvUnavailableLabel(status, LLDV_PACKED_422_12, true);
}

inline LldvTransport SelectLldvTransport(const LldvConnectorStatus& status, int preference,
                                        bool allowModeSwitch = false)
{
  const bool timing = status.currentTiming || (allowModeSwitch && status.availableTiming);
  auto format = LldvFormatPreference(preference);
  if (format == LldvFormat::Auto)
    format = ChooseLldvFormat(status.sink, LldvAutomaticFormats(status), timing);
  if (LldvUnavailableLabel(status, preference, allowModeSwitch) || !(LldvBit(format) & LLDV_422))
    return {};
  return {status.connectorId, format,
          LldvRequiresForcing(format, status.driverFormats, status.forced42210), status.edidGeneration,
          status.rgb10Scanout && format == LldvFormat::Y422_10};
}

inline const char* LldvCompatibilityReason(const LldvConnectorStatus& status, int preference)
{
  if (status.display.status != LldvEdidStatus::Ready) return LldvEdidReason(status.display.status);
  switch (LldvUnavailableLabel(status, preference, true))
  {
    case 0: return "available";
    case 41210: return "format-not-advertised-by-tv";
    case 41211: return "format-not-supported-by-driver-or-port";
    case 41212: return "hdmi-timing-unavailable";
    case LLDV_CHOOSE_FORMAT_LABEL: return "experimental-format-not-selected";
    default: return "no-common-format";
  }
}
} // namespace DVBRIDGE
