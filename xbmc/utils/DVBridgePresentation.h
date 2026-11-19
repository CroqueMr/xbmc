/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <ctime>

namespace DVBRIDGE
{
inline bool ContinuousPresentationPicture(double currentUs, double previousUs)
{
  // Kodi picture timestamps use microseconds; seeks and repeats are excluded.
  return std::isfinite(currentUs) && std::isfinite(previousUs) &&
      currentUs > previousUs && currentUs - previousUs < 250000;
}

// One picture of timing per accepted fence; no quality change before a latched switch.
class MasterSafety
{
public:
  void Begin(uint64_t stream,uint64_t policy,uint64_t source,double pts,bool enabled)
  {
    const bool reset=!enabled || stream!=m_stream || policy!=m_policy || source!=m_source ||
        !std::isfinite(pts) || (pts!=m_pts && !ContinuousPresentationPicture(pts,m_pts));
    if(reset){m_warm=m_samples=m_low=0;m_active=false;}
    m_stream=stream;m_policy=policy;m_source=source;m_pts=pts;m_enabled=enabled;
  }
  bool Observe(std::optional<int64_t> headroom,bool eligible)
  {
    if(!m_enabled || m_active || !eligible || !headroom)return false;
    if(m_warm<60){++m_warm;return false;}
    m_low+=*headroom<2500000;
    if(++m_samples<60)return false;
    const bool switchNow=m_low>=12;
    m_samples=m_low=0;
    m_active=switchNow;
    return switchNow;
  }
  bool Active() const{return m_active;}
  bool Active(uint64_t stream,uint64_t policy,uint64_t source) const
  {return m_active && stream==m_stream && policy==m_policy && source==m_source;}
private:
  uint64_t m_stream{},m_policy{},m_source{};
  double m_pts{};
  unsigned m_warm{},m_samples{},m_low{};
  bool m_enabled{},m_active{};
};

inline uint64_t MonotonicNowNs()
{
  timespec now{};
  return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
      uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec) : 0;
}

// Measured delay from picture selection to the start of its scanout.
// Selection records a CLOCK_MONOTONIC time per picture; the KMS completion
// fence of the commit that carries that picture closes the sample.
class PresentationLatency
{
public:
  static constexpr uint64_t MAX_SAMPLE_NS = 500000000;
  static constexpr unsigned MIN_SAMPLES = 8;

  void Reset() { *this = {}; }
  void Selected(double pts, uint64_t nowNs)
  {
    if (!std::isfinite(pts) || !nowNs) return;
    for (const auto& selection : m_selected)
      if (selection.ns && selection.pts == pts) return;
    m_selected[m_nextSelection] = {pts, nowNs};
    m_nextSelection = (m_nextSelection + 1) % m_selected.size();
  }
  void Completed(double pts, uint64_t completedNs)
  {
    if (!completedNs || completedNs == m_lastCompletedNs) return;
    m_lastCompletedNs = completedNs;
    for (auto& selection : m_selected)
    {
      if (!selection.ns || selection.pts != pts) continue;
      // Only the first scanout of a picture ends its selection delay.
      if (completedNs > selection.ns && completedNs - selection.ns < MAX_SAMPLE_NS)
      {
        m_samples[m_nextSample] = completedNs - selection.ns;
        m_nextSample = (m_nextSample + 1) % m_samples.size();
        m_count = std::min<unsigned>(m_count + 1, m_samples.size());
      }
      selection = {};
      return;
    }
  }
  // Interquartile mean in microseconds: single stalls cannot move it, and a
  // pipeline alternating between two depths is centred between them.
  std::optional<double> EstimateUs() const
  {
    if (m_count < MIN_SAMPLES) return std::nullopt;
    auto values = m_samples;
    std::sort(values.begin(), values.begin() + m_count);
    const unsigned first = m_count / 4, last = m_count - m_count / 4;
    double sum = 0;
    for (unsigned i = first; i < last; ++i) sum += static_cast<double>(values[i]);
    return sum / (last - first) / 1000.0;
  }

private:
  struct Selection
  {
    double pts{};
    uint64_t ns{};
  };
  std::array<Selection, 8> m_selected{};
  std::array<uint64_t, 15> m_samples{};
  unsigned m_nextSelection{}, m_nextSample{}, m_count{};
  uint64_t m_lastCompletedNs{};
};

inline uint64_t MasterFramePeriodNs(double fps)
{
  return std::isfinite(fps) && fps >= 10 && fps <= 240 ?
      static_cast<uint64_t>(std::llround(1e9/fps)) : 0;
}

inline std::optional<int64_t> PresentationHeadroom(uint64_t previousNs, uint64_t readyNs,
    uint64_t submittedNs, uint64_t presentedNs, uint64_t periodNs)
{
  if (!previousNs || !readyNs || !submittedNs || periodNs < 1000000 || periodNs > 100000000 ||
      presentedNs <= previousNs || readyNs > presentedNs || submittedNs > presentedNs ||
      presentedNs - previousNs > 4 * periodNs)
    return std::nullopt;
  // A delayed actual presentation must not move the missed deadline forward.
  return static_cast<int64_t>(previousNs) + static_cast<int64_t>(periodNs) -
      static_cast<int64_t>(std::max(readyNs, submittedNs));
}

}
