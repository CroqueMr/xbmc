// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace DVBRIDGE
{
// /dev/kmsg records: priority,sequence,monotonic_us,flags;message\ncontext.
// Only kernel-origin DV/display incidents are copied, never general system logs.
class KernelLogFilter
{
public:
  std::optional<std::string> Accept(std::string_view record, uint64_t nowUs)
  {
    uint64_t priority, sequence, timestamp;
    if (!Number(record, priority) || !Number(record, sequence) || !Number(record, timestamp))
      return {};
    const auto separator = record.find(';');
    if (separator == record.npos || priority > 7)
      return {};
    if (m_seen && sequence <= m_sequence)
      return {};
    m_seen = true;
    m_sequence = sequence;
    if (timestamp > nowUs || nowUs - timestamp > 60000000)
      return {};
    auto message = record.substr(separator + 1);
    message = message.substr(0, message.find('\n'));
    // i915/xe faults can explain a failed flip without a DV-specific rejection.
    // Restrict these additional records to warning-or-worse display incidents.
    const bool intel = message.find("i915 ") != message.npos ||
                       message.find("xe ") != message.npos;
    const bool incident = priority <= 4 && intel &&
        (message.find("GPU HANG") != message.npos ||
         message.find("Resetting") != message.npos ||
         message.find("reset failed") != message.npos ||
         message.find("underrun") != message.npos ||
         message.find("flip_done timed out") != message.npos ||
         message.find("vblank wait timed out") != message.npos ||
         message.find("link training failed") != message.npos ||
         message.find("link-status BAD") != message.npos);
    if (message.find("DVBridge:") == message.npos && !incident)
      return {};
    // Kernel escapes untrusted characters. Bound the copied record as well.
    const std::string text(message.substr(0, 2048));
    for (const auto& recent : m_recent)
      if (recent.first == text && nowUs - recent.second < 30000000)
        return {};
    m_recent.emplace_back(text, nowUs);
    if (m_recent.size() > 16)
      m_recent.pop_front();
    return "boot_us=" + std::to_string(timestamp) + " seq=" + std::to_string(sequence) +
           " " + text;
  }

private:
  static bool Number(std::string_view& input, uint64_t& value)
  {
    const auto comma = input.find(',');
    if (comma == input.npos || comma == 0)
      return false;
    const auto result = std::from_chars(input.data(), input.data() + comma, value);
    if (result.ec != std::errc{} || result.ptr != input.data() + comma)
      return false;
    input.remove_prefix(comma + 1);
    return true;
  }
  uint64_t m_sequence{0};
  bool m_seen{false};
  std::deque<std::pair<std::string, uint64_t>> m_recent;
};
}
