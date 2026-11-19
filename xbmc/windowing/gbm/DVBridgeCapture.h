/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

class CWinSystemBase;
namespace KODI::RENDERING::CAPTURE { struct ReadbackBuffer; }

namespace KODI::WINDOWING::GBM
{
bool HasDVBridgeScreenshot(CWinSystemBase& window);
bool IsDVBridgeScreenshotSDR(CWinSystemBase& window);
bool ReadDVBridgeScreenshot(CWinSystemBase& window, KODI::RENDERING::CAPTURE::ReadbackBuffer& out,
                            bool videoOnly = false);
}
