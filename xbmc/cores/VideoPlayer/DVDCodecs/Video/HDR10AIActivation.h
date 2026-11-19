/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cb1_l1l3_model.h"
#include <cmath>
#include <cstdint>

namespace DVBRIDGE
{
class HDR10AIActivation
{
public:
  enum class Decision { Wait, Ignore, Native, Prepare };

  void Begin(uint64_t stream, uint64_t revision, uint64_t now)
  {
    m_stream = stream;
    m_revision = revision;
    m_started = m_waitSince = now;
    m_spent = 0;
    m_deadline = 0;
    m_waitScheduled = false;
    m_active = stream && revision;
    m_waiting = m_active;
    m_native = m_event = m_prepared = m_presented = false;
    m_paused = false;
    m_inputPending = false;
  }
  void WaitForInput() { m_inputPending = true; m_waiting = false; }
  void InputReady(uint64_t now)
  {
    if (!m_active || m_native || !m_inputPending) return;
    m_inputPending = false;
    m_waiting = true;
    m_started = m_waitSince = m_paused ? m_pauseSince : now;
  }
  void SetPaused(bool paused, uint64_t now)
  {
    if (paused == m_paused) return;
    if (paused) m_pauseSince = now;
    else if (now >= m_pauseSince)
    {
      if (m_waiting) m_waitSince += now - m_pauseSince;
      m_started += now - m_pauseSince;
    }
    m_paused = paused;
  }

  void SetPresentationDeadline(uint64_t deadline) { m_deadline = deadline; }

  void AnalysisCompleted(cb1_ai_identity id, uint64_t now)
  {
    // Priming can process many pictures. Keep the stall limit, and bound the
    // whole priming interval even when pictures continue to finish.
    if (!Matches(id) || m_presented || m_prepared || m_inputPending || m_native || m_paused ||
        now < m_started || now - m_started >= 3000 ||
        now < m_waitSince || now - m_waitSince >= 1000 - m_spent)
      return;
    m_waitSince = now;
    m_spent = 0;
  }

  Decision Observe(cb1_ai_status status, cb1_ai_identity id, uint64_t now, bool required = true)
  {
    if (!Matches(id))
      return Decision::Ignore;
    if (m_native)
      return Decision::Native;
    if (status == CB1_AI_INCOMPATIBLE)
      return Fallback();
    if (m_inputPending)
      return Decision::Wait;
    if (m_paused && now >= m_pauseSince) now = m_pauseSince;
    if (!m_presented && now >= m_started && now - m_started >= 3000)
      return Fallback();
    if (m_waiting)
    {
      if (now < m_waitSince && !m_waitScheduled)
        return Fallback();
      const uint64_t elapsed = now >= m_waitSince ? now - m_waitSince : 0;
      if (elapsed > 1000 - m_spent ||
          (now >= m_waitSince && elapsed == 1000 - m_spent && status != CB1_AI_READY))
        return Fallback();
      m_spent += elapsed;
      if (now >= m_waitSince)
      {
        m_waitSince = now;
        m_waitScheduled = false;
      }
    }
    if (status == CB1_AI_READY)
    {
      m_waiting = false;
      m_prepared = true;
      m_identity = id;
      return Decision::Prepare;
    }
    if (!m_waiting)
    {
      if (!required)
        return Decision::Wait;
      if (m_spent == 1000 && now >= m_deadline)
        return Fallback();
      m_waiting = true;
      m_waitScheduled = now < m_deadline;
      m_waitSince = m_waitScheduled ? m_deadline : now;
    }
    return Decision::Wait;
  }

  bool Presented(cb1_ai_identity id, bool success)
  {
    if (!Matches(id) || m_native || !m_prepared || id.picture != m_identity.picture)
      return false;
    m_prepared = false;
    if (!success)
      Fallback();
    m_presented = success;
    return success;
  }

  Decision CancelPrepared(cb1_ai_identity id, uint64_t now)
  {
    if (!Matches(id) || !m_prepared || id.picture != m_identity.picture)
      return Decision::Ignore;
    // Settle real waiting before retiring its owner; never refund the budget.
    if (m_waiting && Observe(CB1_AI_PENDING, id, now) == Decision::Native)
      return Decision::Native;
    m_prepared = m_waiting = m_waitScheduled = false;
    return Decision::Wait;
  }

  bool TakeFallbackEvent()
  {
    const bool event = m_event;
    m_event = false;
    return event;
  }

  void Stop() { m_active = m_prepared = m_waiting = false; }

  static unsigned BufferCount(double fps)
  {
    return std::isfinite(fps) && fps > 0 && fps <= 60 ?
        static_cast<unsigned>(std::ceil(fps)) + 3 : 0;
  }

private:
  bool Matches(cb1_ai_identity id) const
  {
    return m_active && id.stream == m_stream && id.revision == m_revision && id.picture;
  }
  Decision Fallback()
  {
    m_native = m_event = true;
    m_prepared = m_waiting = false;
    return Decision::Native;
  }

  uint64_t m_stream{}, m_revision{}, m_started{}, m_waitSince{}, m_spent{}, m_pauseSince{}, m_deadline{};
  cb1_ai_identity m_identity{};
  bool m_active{}, m_waiting{}, m_native{}, m_event{}, m_prepared{}, m_presented{}, m_paused{}, m_waitScheduled{}, m_inputPending{};
};
}
