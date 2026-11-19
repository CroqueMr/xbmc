/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ServiceBroker.h"
#include "utils/DVBridgeDiagnosticState.h"
#include "utils/log.h"

#include <algorithm>
#include <array>

namespace DVBRIDGE
{
inline bool OverlayDebugEnabled()
{
  return DetailedDiagnosticsEnabled() && CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG);
}

using OverlayClock = std::chrono::steady_clock;

inline uint64_t OverlayElapsedUs(OverlayClock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(OverlayClock::now() - start).count();
}

enum class OverlayStage { Refresh, Draw, Composite, Present, Commit, FenceWait, Count };

// Kodi's GUI, composition and DRM calls run on the application thread.
struct OverlayTrace
{
  struct Samples
  {
    uint64_t count{}, first{}, max{};
    void Add(uint64_t us)
    {
      if (count++ == 0) first = us;
      max = std::max(max, us);
    }
  };

  const char* kind{};
  uint64_t openedMs{};
  std::array<Samples, static_cast<size_t>(OverlayStage::Count)> samples{};
  DiagnosticInterval slowFrames;

  void Observe(const char* next)
  {
    if (kind == next) return;
    if (kind)
    {
      const auto& [refresh, draw, composite, present, commit, wait] = samples;
      CLog::Log(LOGDEBUG, "CB1 overlay summary: kind={} opened_ms={} elapsed_ms={} "
          "refresh_count={} refresh_first_us={} refresh_max_us={} draw_count={} draw_first_us={} draw_max_us={} "
          "composite_count={} composite_max_us={} present_count={} present_max_us={} "
          "commit_count={} commit_max_us={} fence_wait_count={} fence_wait_max_us={}",
          kind, openedMs, DiagnosticNowMs() - openedMs, refresh.count, refresh.first, refresh.max,
          draw.count, draw.first, draw.max, composite.count, composite.max, present.count,
          present.max, commit.count, commit.max, wait.count, wait.max);
    }
    *this = {};
    if (next)
    {
      kind = next;
      openedMs = DiagnosticNowMs();
      CLog::Log(LOGDEBUG, "CB1 overlay start: kind={} opened_ms={}", kind, openedMs);
    }
  }

  void Record(OverlayStage stage, uint64_t us)
  {
    samples[static_cast<size_t>(stage)].Add(us);
    if (us < 20000) return;
    const uint64_t now = DiagnosticNowMs();
    if (!slowFrames.Due(now)) return;
    slowFrames.Emitted(now);
    constexpr std::array names{"refresh", "draw", "composite", "present", "drm-commit", "drm-fence-wait"};
    CLog::Log(LOGDEBUG, "CB1 overlay slow stage: kind={} opened_ms={} now_ms={} stage={} elapsed_us={}",
              kind, openedMs, now, names[static_cast<size_t>(stage)], us);
  }
};

inline thread_local OverlayTrace overlayTrace;
inline constexpr char PLAYER_INFO_CB1[] = "player-info-cb1";
inline constexpr char PLAYER_INFO_KODI[] = "player-info-kodi";
inline constexpr char VIDEO_OSD[] = "video-osd";

inline bool IsPlayerInfoTrace()
{
  return overlayTrace.kind == PLAYER_INFO_CB1 || overlayTrace.kind == PLAYER_INFO_KODI;
}

class OverlayTimer
{
public:
  explicit OverlayTimer(OverlayStage stage) : m_stage(stage)
  {
    if (overlayTrace.kind) m_start = OverlayClock::now();
  }

  ~OverlayTimer() { Finish(); }

  void Finish()
  {
    if (m_start == OverlayClock::time_point{}) return;
    overlayTrace.Record(m_stage, OverlayElapsedUs(m_start));
    m_start = {};
  }

private:
  OverlayStage m_stage;
  OverlayClock::time_point m_start;
};
} // namespace DVBRIDGE
