/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

namespace DVBRIDGE
{
// Diagnostics only: never use this state to decide whether a frame is presented.
// Keep the first failure, then at most one report every five seconds, even when
// different failure stages alternate. Successful steady-state frames do no work.
class DiagnosticFailures
{
public:
  bool Record(uint64_t nowMs)
  {
    ++m_count;
    if (m_count == 1 || nowMs - m_lastReportMs >= 5000)
    {
      m_lastReportMs = nowMs;
      return true;
    }
    return false;
  }
  uint64_t Count() const { return m_count; }
  void Reset() { m_count = 0; m_lastReportMs = 0; }

private:
  uint64_t m_count{0};
  uint64_t m_lastReportMs{0};
};
}
