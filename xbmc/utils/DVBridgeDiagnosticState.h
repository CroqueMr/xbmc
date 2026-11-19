/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace DVBRIDGE
{
// Enabled without Kodi's global debug mode. Developers can disable the entire
// supplement in one place; it must never change output or retry decisions.
inline bool DiagnosticsEnabled()
{
  static const bool enabled = [] {
    const char* setting = std::getenv("DVBRIDGE_DIAGNOSTICS");
    return !setting || setting[0] != '0' || setting[1] != '\0';
  }();
  return enabled;
}

inline uint64_t DiagnosticNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

class DiagnosticInterval
{
public:
  bool Due(uint64_t now)
  {
    if (m_reported && (now<m_last || now-m_last<5000)) return false;
    m_reported=true;m_last=now;return true;
  }
private:
  uint64_t m_last{};
  bool m_reported{};
};

// A bounded first-event/change budget plus a 30-second summary. Alternating
// failures/recoveries cannot reset the budget and turn this into per-frame IO.
class DiagnosticFailures
{
public:
  bool Record(uint64_t now, const char* stage = "failure", int error = 0)
  {
    ++m_count;
    if (!m_consecutive) m_firstConsecutive=now;
    ++m_consecutive;
    const bool changed = stage != m_stage || error != m_error;
    m_stage = stage;
    m_error = error;
    if (!m_reported || now - m_window >= 30000)
    {
      m_window = now;
      m_reports = 0;
    }
    if ((!m_reported || changed || now - m_last >= 30000) && m_reports < 4)
    {
      m_reported = true;
      ++m_reports;
      m_last = now;
      return true;
    }
    return false;
  }
  bool Recovered(uint64_t now)
  {
    if (!m_consecutive)
      return false;
    m_consecutive = 0;
    ++m_recoveries;
    if (m_recoveries == 1 || now - m_lastRecovery >= 30000)
    {
      m_lastRecovery = now;
      return true;
    }
    return false;
  }
  uint64_t Count() const { return m_count; }
  uint64_t Recoveries() const { return m_recoveries; }
  bool ReferenceDue(uint64_t now,bool eligible)
  {
    if (!eligible || !m_consecutive) { m_firstConsecutive=now; return false; }
    if (m_referenceWarned || m_consecutive<30 || now<m_firstConsecutive ||
        now-m_firstConsecutive<5000) return false;
    m_referenceWarned=true;
    return true;
  }
  void Reset() { *this = {}; }

private:
  uint64_t m_count{0}, m_consecutive{0}, m_recoveries{0};
  uint64_t m_window{0}, m_last{0}, m_lastRecovery{0};
  unsigned m_reports{0};
  const char* m_stage{nullptr}; // Callers pass static stage labels.
  int m_error{0};
  bool m_reported{false};
  uint64_t m_firstConsecutive{};
  bool m_referenceWarned{};
};
} // namespace DVBRIDGE
