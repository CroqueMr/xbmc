/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>

namespace DVBRIDGE
{
// Output recovery only; diagnostic counters never influence this state.
// Preserve early transient recovery, then space attempts without sleeping on
// the GUI thread or permanently blacklisting a potentially usable display.
class OutputRetry
{
public:
  bool Ready(uint64_t now) const { return !m_failures || now >= m_next; }
  void Failed(uint64_t now, int error)
  {
    if (!m_failures)
      m_since = now;
    ++m_failures;
    m_error = error;
    m_delay = m_failures <= 2 ? 0 : (now - m_since < 3000 ? 250 : 1000);
    m_next = now + m_delay;
  }
  void Reset() { *this = {}; }
  bool PollDue(uint64_t now)
  {
    if (!m_failures || (m_polled && now - m_poll < 1000))
      return false;
    m_polled = true;
    m_poll = now;
    return true;
  }
  uint64_t Count() const { return m_failures; }
  unsigned Delay() const { return m_delay; }
  int Error() const { return m_error; }

private:
  uint64_t m_failures{0}, m_since{0}, m_next{0}, m_poll{0};
  unsigned m_delay{0};
  int m_error{0};
  bool m_polled{false};
};
} // namespace DVBRIDGE
