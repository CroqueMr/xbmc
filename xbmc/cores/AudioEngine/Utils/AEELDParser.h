/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstring>
#include <optional>
#include <stdint.h>

class CAEDeviceInfo;

struct AEELDDtsCapabilities
{
  bool core;
  bool hd;
};

class CAEELDParser {
public:
  static std::optional<AEELDDtsCapabilities> Parse(const uint8_t* data,
                                                    size_t length,
                                                    CAEDeviceInfo& info);
};

