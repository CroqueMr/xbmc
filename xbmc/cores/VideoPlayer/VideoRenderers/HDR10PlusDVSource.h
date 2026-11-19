/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "cores/VideoSourceSideData.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVMetadataBuffer.h"
extern "C"
{
#include "cb1_hdr10plus_dovi.h"
}

namespace DVBRIDGE
{
inline bool HDR10PlusDVWholeSource(const float (&crop)[4], int width, int height)
{
  return width > 0 && height > 0 && crop[0] == 0 && crop[1] == 0 &&
      ((!crop[2] && !crop[3]) || (crop[2] == width && crop[3] == height));
}

// Serial and SourceBinding picture belong to different captured-owner counters.
inline bool HDR10PlusDVSnapshotMatches(const std::shared_ptr<const VideoSourceSideData>& source,
    SourceBinding selected, SourceBinding captured, uint64_t generation, uint64_t serial,
    bool wholeSource)
{
  return wholeSource && selected.Valid() && selected == captured && generation && serial &&
      source && source->Generation() == generation && source->Serial() == serial && source->HDR10Plus();
}

inline bool DescribeHDR10PlusDV(cb1_hdr10plus_dovi_result& out,
    const cb1_l1l3& estimate, const std::shared_ptr<const VideoSourceSideData>& source,
    SourceBinding selected, SourceBinding captured, uint64_t generation, uint64_t serial,
    bool wholeSource)
{
  if(!HDR10PlusDVSnapshotMatches(source,selected,captured,generation,serial,wholeSource))return false;
  // params[0] is FFmpeg's implicit full-frame window, not an authored crop.
  return cb1_hdr10plus_dovi_describe(&out, source->HDR10Plus(), &estimate) ==
      CB1_HDR10PLUS_DOVI_OK;
}
}
