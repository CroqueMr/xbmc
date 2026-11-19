/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <algorithm>
#include <optional>
#include <cmath>

namespace DVBRIDGE
{
inline bool ContinuousFastRenderingPicture(double currentUs, double previousUs)
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
        !std::isfinite(pts) || (pts!=m_pts && !ContinuousFastRenderingPicture(pts,m_pts));
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
