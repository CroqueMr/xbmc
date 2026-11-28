/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "DVDVideoCodec.h"

namespace DVBRIDGE
{
inline bool IsSDRExpansionColor(AVColorTransferCharacteristic transfer,
                                AVColorSpace matrix,
                                AVColorPrimaries primaries)
{
  switch (transfer)
  {
    case AVCOL_TRC_UNSPECIFIED:
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_GAMMA22:
    case AVCOL_TRC_GAMMA28:
    case AVCOL_TRC_IEC61966_2_1:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
      break;
    default:
      return false;
  }
  return (matrix == AVCOL_SPC_UNSPECIFIED || matrix == AVCOL_SPC_BT709 ||
          matrix == AVCOL_SPC_BT470BG || matrix == AVCOL_SPC_SMPTE170M ||
          matrix == AVCOL_SPC_BT2020_NCL) &&
         (primaries == AVCOL_PRI_BT709 || primaries == AVCOL_PRI_BT470BG ||
          primaries == AVCOL_PRI_SMPTE170M || primaries == AVCOL_PRI_BT2020);
}

inline bool IsSDRExpansionSource(const VideoPicture& picture)
{
  return picture.hdrType == StreamHdrType::HDR_TYPE_NONE && !picture.doviSourceInfo.profile &&
         !picture.doviMetadata.Data() && !picture.doviRpu &&
         (picture.stereoMode.empty() || picture.stereoMode == "mono") &&
         !(picture.iFlags & DVP_FLAG_INTERLACED) && picture.colorBits >= 8 &&
         picture.colorBits <= 12 &&
         IsSDRExpansionColor(picture.color_transfer, picture.color_space, picture.color_primaries);
}
} // namespace DVBRIDGE
