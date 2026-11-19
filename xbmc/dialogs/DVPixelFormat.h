/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <string>
extern "C"
{
#include <libavutil/pixdesc.h>
}

namespace DVBRIDGE
{
// Describe the decoded source, never the opaque hardware surface or HDMI tunnel.
inline std::string SourcePixelFormat(AVPixelFormat format)
{
  const auto* desc = av_pix_fmt_desc_get(format);
  if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
    return {};
  if (desc->flags & AV_PIX_FMT_FLAG_RGB)
    return "RGB 4:4:4";
  if (desc->nb_components < 3)
    return "Grayscale";
  if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
    return "YUV 4:2:0";
  if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
    return "YUV 4:2:2";
  if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
    return "YUV 4:4:4";
  return desc->name ? desc->name : "";
}

// Name the decoder's colour tags: primaries, transfer and range. Kodi assumes primaries
// for untagged pictures, which is said rather than presented as a file fact.
inline std::string SourceColour(int primaries, int transfer, bool guessed, bool fullRange)
{
  if (primaries < 0 && transfer < 0)
    return {};
  std::string gamut;
  switch (primaries)
  {
    case AVCOL_PRI_BT709: gamut = "BT.709"; break;
    case AVCOL_PRI_BT2020: gamut = "BT.2020"; break;
    case AVCOL_PRI_SMPTE432: gamut = "P3-D65"; break;
    case AVCOL_PRI_SMPTE431: gamut = "DCI-P3"; break;
    case AVCOL_PRI_BT470BG: gamut = "BT.601 PAL"; break;
    case AVCOL_PRI_SMPTE170M: gamut = "BT.601 NTSC"; break;
    case AVCOL_PRI_UNSPECIFIED: break;
    default:
      if (const char* name = av_color_primaries_name(static_cast<AVColorPrimaries>(primaries)))
        gamut = name;
  }
  if (!gamut.empty() && guessed)
    gamut += " (assumed)";
  std::string curve;
  switch (transfer)
  {
    case AVCOL_TRC_SMPTE2084: curve = "PQ"; break;
    case AVCOL_TRC_ARIB_STD_B67: curve = "HLG"; break;
    case AVCOL_TRC_BT709: case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10: case AVCOL_TRC_BT2020_12: curve = "SDR"; break;
    case AVCOL_TRC_IEC61966_2_1: curve = "sRGB"; break;
    case AVCOL_TRC_UNSPECIFIED: break;
    default:
      if (const char* name = av_color_transfer_name(static_cast<AVColorTransferCharacteristic>(transfer)))
        curve = name;
  }
  std::string text;
  for (const std::string& part : {gamut, curve, std::string(fullRange ? "Full" : "Limited")})
    if (!part.empty())
      text += (text.empty() ? "" : " · ") + part;
  return text;
}
}
