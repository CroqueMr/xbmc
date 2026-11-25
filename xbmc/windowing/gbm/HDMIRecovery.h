/*
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>

namespace KODI::WINDOWING::GBM
{
class CHDMIRecovery
{
public:
  using Clock = std::chrono::steady_clock;

  static constexpr auto SETTING_MODE = "videoscreen.hdmirecovery";
  static constexpr auto SETTING_SHORTCUT = "videoscreen.hdmirecoverykey";
  static constexpr auto SETTING_ASSIGN = "videoscreen.hdmirecoveryshortcut";

  enum class Mode
  {
    Off,
    Manual,
    Automatic,
    AutomaticAndManual,
  };

  enum class Request
  {
    None,
    Manual,
    Automatic,
  };

  enum class Procedure
  {
    None,
    RestoreLink,
    PowerCycle,
  };

  enum class PowerStep
  {
    Off,
    On,
    RetryOn,
  };

  static Procedure SelectProcedure(bool manual, bool linkBad)
  {
    if (linkBad)
      return Procedure::RestoreLink;
    return manual ? Procedure::PowerCycle : Procedure::None;
  }

  // False means the output was disabled and still needs reactivation.
  template<typename Submit>
  static bool PowerCycle(Submit&& submit)
  {
    return !submit(PowerStep::Off) || submit(PowerStep::On) || submit(PowerStep::RetryOn);
  }

  bool Arm(Mode mode, bool manual, Clock::time_point now, Clock::duration displayDelay = {})
  {
    using namespace std::chrono_literals;

    if (manual)
    {
      if (!AllowsManual(mode) || now < m_manualCooldown)
        return false;
      m_manualUntil = now + displayDelay + 4s;
    }
    else
    {
      if (!AllowsAutomatic(mode))
        return false;
      m_automaticUntil = now + displayDelay + 4s;
      m_nextProbe = now;
    }

    return true;
  }

  Request Poll(Mode mode, bool ready, Clock::time_point now)
  {
    using namespace std::chrono_literals;

    if (!AllowsManual(mode) || now >= m_manualUntil)
      m_manualUntil = {};
    if (!AllowsAutomatic(mode) || now >= m_automaticUntil)
      m_automaticUntil = {};

    if (!ready)
      return Request::None;

    if (m_manualUntil != Clock::time_point{})
    {
      m_manualCooldown = now + 1s;
      Cancel();
      return Request::Manual;
    }

    if (m_automaticUntil == Clock::time_point{} || now < m_nextProbe)
      return Request::None;

    m_nextProbe = now + 250ms;
    return Request::Automatic;
  }

  void AutomaticAttempted() { m_automaticUntil = {}; }
  void Cancel()
  {
    m_manualUntil = {};
    AutomaticAttempted();
  }
  bool Pending() const
  {
    return m_manualUntil != Clock::time_point{} || m_automaticUntil != Clock::time_point{};
  }

  static bool AllowsManual(Mode mode)
  {
    return mode == Mode::Manual || mode == Mode::AutomaticAndManual;
  }

private:
  static bool AllowsAutomatic(Mode mode)
  {
    return mode == Mode::Automatic || mode == Mode::AutomaticAndManual;
  }

  Clock::time_point m_manualUntil{};
  Clock::time_point m_manualCooldown{};
  Clock::time_point m_automaticUntil{};
  Clock::time_point m_nextProbe{};
};
} // namespace KODI::WINDOWING::GBM
