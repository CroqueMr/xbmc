/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

class CWinSystemBase;

namespace KODI::WINDOWING::GBM
{
bool HasDVBridgeScreenshot(CWinSystemBase& window);
unsigned int GetDVBridgeScreenshotFramebuffer(CWinSystemBase& window);
}
