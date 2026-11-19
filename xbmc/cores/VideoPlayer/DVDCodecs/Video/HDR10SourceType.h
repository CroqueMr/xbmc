/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "cores/VideoPlayer/Interface/StreamInfo.h"

namespace DVBRIDGE
{
inline bool IsHDR10Family(StreamHdrType source)
{
  return source == StreamHdrType::HDR_TYPE_HDR10 ||
      source == StreamHdrType::HDR_TYPE_HDR10PLUS || source == StreamHdrType::HDR_TYPE_HLG;
}

inline bool IsHDR10AIInput(StreamHdrType source, bool guided)
{
  return source == StreamHdrType::HDR_TYPE_HDR10 ||
      (guided && source == StreamHdrType::HDR_TYPE_HDR10PLUS) ||
      (!guided && source == StreamHdrType::HDR_TYPE_HLG);
}
}
