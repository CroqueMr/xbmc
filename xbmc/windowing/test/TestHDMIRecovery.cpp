/*
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "windowing/gbm/HDMIRecovery.h"

#include <vector>

#include <gtest/gtest.h>

using namespace KODI::WINDOWING::GBM;
using namespace std::chrono_literals;

using Mode = CHDMIRecovery::Mode;
using Request = CHDMIRecovery::Request;

TEST(TestHDMIRecovery, Modes)
{
  const auto now = CHDMIRecovery::Clock::time_point{} + 10s;
  for (const auto mode : {Mode::Off, Mode::Manual, Mode::Automatic, Mode::AutomaticAndManual})
  {
    CHDMIRecovery recovery;
    const bool manual = mode == Mode::Manual || mode == Mode::AutomaticAndManual;
    const bool automatic = mode == Mode::Automatic || mode == Mode::AutomaticAndManual;

    EXPECT_EQ(recovery.Arm(mode, true, now), manual);
    EXPECT_EQ(recovery.Poll(mode, true, now), manual ? Request::Manual : Request::None);
    EXPECT_EQ(recovery.Arm(mode, false, now), automatic);
    EXPECT_EQ(recovery.Poll(mode, true, now), automatic ? Request::Automatic : Request::None);
  }
}

TEST(TestHDMIRecovery, WaitsForSettledOutputAndLimitsProbes)
{
  const auto now = CHDMIRecovery::Clock::time_point{} + 10s;
  CHDMIRecovery recovery;
  recovery.Arm(Mode::Automatic, false, now);

  EXPECT_EQ(recovery.Poll(Mode::Automatic, false, now), Request::None);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 100ms), Request::Automatic);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 110ms), Request::None);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 500ms), Request::Automatic);
  recovery.AutomaticAttempted();
  EXPECT_FALSE(recovery.Pending());
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 1s), Request::None);
}

TEST(TestHDMIRecovery, ManualHasPriorityAndDebounces)
{
  const auto now = CHDMIRecovery::Clock::time_point{} + 10s;
  CHDMIRecovery recovery;
  recovery.Arm(Mode::AutomaticAndManual, false, now);
  recovery.Arm(Mode::AutomaticAndManual, true, now);
  EXPECT_EQ(recovery.Poll(Mode::AutomaticAndManual, true, now), Request::Manual);
  EXPECT_FALSE(recovery.Pending());
  EXPECT_FALSE(recovery.Arm(Mode::AutomaticAndManual, true, now + 100ms));
  EXPECT_TRUE(recovery.Arm(Mode::AutomaticAndManual, true, now + 2s));
}

TEST(TestHDMIRecovery, CancelsAndExpires)
{
  const auto now = CHDMIRecovery::Clock::time_point{} + 10s;
  CHDMIRecovery recovery;
  recovery.Arm(Mode::AutomaticAndManual, true, now);
  EXPECT_EQ(recovery.Poll(Mode::Off, true, now), Request::None);
  EXPECT_FALSE(recovery.Pending());
  recovery.Arm(Mode::Automatic, false, now);
  recovery.Cancel();
  EXPECT_FALSE(recovery.Pending());
  recovery.Arm(Mode::Automatic, false, now);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 10s), Request::None);
  EXPECT_FALSE(recovery.Pending());
}

TEST(TestHDMIRecovery, AllowsConfiguredRefreshDelay)
{
  const auto now = CHDMIRecovery::Clock::time_point{} + 10s;
  CHDMIRecovery recovery;
  ASSERT_TRUE(recovery.Arm(Mode::Automatic, false, now, 5s));
  EXPECT_EQ(recovery.Poll(Mode::Automatic, false, now + 5s), Request::None);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 5500ms), Request::Automatic);
  EXPECT_EQ(recovery.Poll(Mode::Automatic, true, now + 10s), Request::None);
}

TEST(TestHDMIRecovery, SelectsLinkRepairOnlyForReportedFailure)
{
  using Procedure = CHDMIRecovery::Procedure;
  EXPECT_EQ(CHDMIRecovery::SelectProcedure(false, false), Procedure::None);
  EXPECT_EQ(CHDMIRecovery::SelectProcedure(true, false), Procedure::PowerCycle);
  EXPECT_EQ(CHDMIRecovery::SelectProcedure(false, true), Procedure::RestoreLink);
  EXPECT_EQ(CHDMIRecovery::SelectProcedure(true, true), Procedure::RestoreLink);
}

TEST(TestHDMIRecovery, PowerCycleNeverRepeatsOff)
{
  using Step = CHDMIRecovery::PowerStep;
  std::vector<Step> steps;
  const auto cycle = [&](bool off, bool on, bool retry)
  {
    steps.clear();
    return CHDMIRecovery::PowerCycle(
        [&](Step step)
        {
          steps.push_back(step);
          if (step == Step::Off)
            return off;
          return step == Step::On ? on : retry;
        });
  };

  EXPECT_TRUE(cycle(false, false, false));
  EXPECT_EQ(steps, (std::vector{Step::Off}));
  EXPECT_TRUE(cycle(true, true, false));
  EXPECT_EQ(steps, (std::vector{Step::Off, Step::On}));
  EXPECT_TRUE(cycle(true, false, true));
  EXPECT_EQ(steps, (std::vector{Step::Off, Step::On, Step::RetryOn}));
  EXPECT_FALSE(cycle(true, false, false));
  EXPECT_EQ(steps, (std::vector{Step::Off, Step::On, Step::RetryOn}));
}
