/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The GBM context class that carries DVBridge output, for GL and GLES builds alike.
#if defined(HAS_GLES)
#include "windowing/gbm/WinSystemGbmGLESContext.h"
#else
#include "windowing/gbm/WinSystemGbmGLContext.h"
#endif

namespace KODI::WINDOWING::GBM
{
#if defined(HAS_GLES)
using CWinSystemGbmDVContext = CWinSystemGbmGLESContext;
#else
using CWinSystemGbmDVContext = CWinSystemGbmGLContext;
#endif
} // namespace KODI::WINDOWING::GBM
