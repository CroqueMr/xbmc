// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DVBRIDGE
{
// /dev/kmsg records: priority,sequence,monotonic_us,flags;message\ncontext.
// Only kernel-origin DV messages are copied, never arbitrary system messages.
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
    if (timestamp > nowUs || nowUs - timestamp > 15000000)
      return {};
    auto message = record.substr(separator + 1);
    message = message.substr(0, message.find('\n'));
    if (message.find("DVBridge:") == message.npos)
      return {};
    // Kernel escapes untrusted characters. Bound the copied record as well.
    return "boot_us=" + std::to_string(timestamp) + " seq=" + std::to_string(sequence) +
           " " + std::string(message.substr(0, 2048));
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
};
}
