/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CaptureConvert.h"

#include "rendering/capture/CapturePixels.h"
#include "rendering/capture/CaptureTypes.h"
#include "utils/log.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

extern "C"
{
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace KODI
{
namespace RENDERING
{
namespace CAPTURE
{

namespace
{

bool CaptureIsDeep(AVPixelFormat format)
{
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
  return desc && desc->comp[0].depth > 8;
}

// source coding to width x height BGRA8, channel order and orientation only, no
// tonemap: the Python RenderCapture contract and the SDR fallback.
bool RawToBGRA8(const CaptureResult& result,
                const uint8_t* src0,
                unsigned int width,
                unsigned int height,
                uint8_t* buffer)
{
  // byte-exact fast path for a BGRA source at the requested size (the Python
  // RenderCapture contract): a stride-aware row copy, no swscale rounding
  if (result.format == AV_PIX_FMT_BGRA && result.width == width && result.height == height &&
      result.stride > 0)
  {
    for (unsigned int y = 0; y < height; y++)
      std::memcpy(buffer + static_cast<size_t>(y) * width * 4,
                  src0 + static_cast<size_t>(y) * result.stride, static_cast<size_t>(width) * 4);
    return true;
  }

  SwsContext* context =
      sws_getContext(static_cast<int>(result.width), static_cast<int>(result.height), result.format,
                     static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
                     SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!context)
    return false;

  const uint8_t* src[] = {src0, nullptr, nullptr, nullptr};
  const int srcStride[] = {result.stride, 0, 0, 0};
  uint8_t* dst[] = {buffer, nullptr, nullptr, nullptr};
  const int dstStride[] = {static_cast<int>(width * 4), 0, 0, 0};
  sws_scale(context, src, srcStride, 0, static_cast<int>(result.height), dst, dstStride);
  sws_freeContext(context);
  return true;
}

// swscale forces full range on RGB input, so limited-range RGB is expanded to full
bool ExpandRange(const CaptureResult& result, const uint8_t* src0, std::vector<uint8_t>& out)
{
  const int stride = static_cast<int>(result.width * 8);
  out.resize(static_cast<size_t>(stride) * result.height);

  SwsContext* context =
      sws_getContext(static_cast<int>(result.width), static_cast<int>(result.height), result.format,
                     static_cast<int>(result.width), static_cast<int>(result.height),
                     AV_PIX_FMT_RGBA64LE, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!context)
    return false;

  const uint8_t* src[] = {src0, nullptr, nullptr, nullptr};
  const int srcStride[] = {result.stride, 0, 0, 0};
  uint8_t* dst[] = {out.data(), nullptr, nullptr, nullptr};
  const int dstStride[] = {stride, 0, 0, 0};
  const int rows = sws_scale(context, src, srcStride, 0, static_cast<int>(result.height), dst, dstStride);
  sws_freeContext(context);
  if (rows != static_cast<int>(result.height))
    return false;

  // 16..235 scaled by 257 to 16-bit -> full 0..65535, alpha left alone
  constexpr int64_t lo = 16 * 257;
  constexpr int64_t hi = 235 * 257;
  uint16_t* samples = reinterpret_cast<uint16_t*>(out.data());
  const size_t count = out.size() / 2;
  for (size_t i = 0; i < count; i++)
  {
    if (i % 4 == 3)
      continue; // alpha
    const int64_t v = (static_cast<int64_t>(samples[i]) - lo) * 65535 / (hi - lo);
    samples[i] = static_cast<uint16_t>(std::clamp<int64_t>(v, 0, 65535));
  }
  return true;
}

// Color-managed conversion honoring the capture's tags: PQ/HLG BT.2020 input
// tonemapped to 8-bit sRGB BT.709, matching what the thumbnail extractor does.
bool ConvertColorManaged(const CaptureResult& result,
                         const uint8_t* src0,
                         unsigned int width,
                         unsigned int height,
                         uint8_t* buffer,
                         AVPixelFormat destination = AV_PIX_FMT_BGRA,
                         bool displaySDR = false)
{
  std::vector<uint8_t> expanded;
  const uint8_t* srcData = src0;
  AVPixelFormat srcFormat = result.format;
  int srcStride = result.stride;
  if (result.color.range == AVCOL_RANGE_MPEG)
  {
    if (ExpandRange(result, src0, expanded))
    {
      srcData = expanded.data();
      srcFormat = AV_PIX_FMT_RGBA64LE;
      srcStride = static_cast<int>(result.width * 8);
    }
    else if (displaySDR)
      return false;
  }

  bool converted = false;

#if LIBSWSCALE_BUILD >= AV_VERSION_INT(9, 0, 100)
  {
    AVFrame* srcFrame = av_frame_alloc();
    AVFrame* dstFrame = av_frame_alloc();
    SwsContext* sws = sws_alloc_context();
    if (srcFrame && dstFrame && sws)
    {
      srcFrame->width = static_cast<int>(result.width);
      srcFrame->height = static_cast<int>(result.height);
      srcFrame->format = srcFormat;
      srcFrame->data[0] = const_cast<uint8_t*>(srcData);
      srcFrame->linesize[0] = srcStride; // signed; negative = bottom-up
      srcFrame->colorspace = AVCOL_SPC_RGB;
      srcFrame->color_range = AVCOL_RANGE_JPEG; // full: raw was full, or expanded above
      srcFrame->color_primaries = static_cast<AVColorPrimaries>(result.color.primaries);
      srcFrame->color_trc = static_cast<AVColorTransferCharacteristic>(result.color.transfer);

      bool colorMetadataReady = true;
      const auto& mastering = result.displayMetadata;
      // Match the sRGB reference display with a stable nominal PQ source peak
      // when mastering is unknown. Overlays must not change movie exposure.
      const bool nominalPQ = displaySDR && srcFrame->color_trc == AVCOL_TRC_SMPTE2084 &&
          (!result.hasDisplayMetadata || !mastering.has_luminance ||
           mastering.max_luminance.num <= 0 || mastering.max_luminance.den <= 0);
      if (result.hasDisplayMetadata || nominalPQ)
      {
        auto* mdm = av_mastering_display_metadata_create_side_data(srcFrame);
        if (mdm)
        {
          *mdm = mastering;
          if (nominalPQ)
          {
            mdm->has_luminance = 1;
            mdm->min_luminance = {0, 1};
            mdm->max_luminance = {1000, 1};
          }
        }
        else if (displaySDR)
          colorMetadataReady = false;
      }
      if (result.hasLightMetadata)
      {
        AVContentLightMetadata* clm = av_content_light_metadata_create_side_data(srcFrame);
        if (clm)
          *clm = result.lightMetadata;
      }

      dstFrame->width = static_cast<int>(width);
      dstFrame->height = static_cast<int>(height);
      dstFrame->format = destination;
      dstFrame->data[0] = buffer;
      dstFrame->linesize[0] = static_cast<int>(width * (destination == AV_PIX_FMT_RGBA64LE ? 8 : 4));
      dstFrame->colorspace = AVCOL_SPC_RGB;
      dstFrame->color_range = AVCOL_RANGE_JPEG;
      dstFrame->color_primaries = AVCOL_PRI_BT709;
      dstFrame->color_trc = displaySDR ? AVCOL_TRC_IEC61966_2_1 : AVCOL_TRC_BT709;
      if (displaySDR)
      {
        // sRGB reference display.
        auto* targetPeak = av_mastering_display_metadata_create_side_data(dstFrame);
        if (targetPeak)
        {
          targetPeak->has_luminance = 1;
          targetPeak->min_luminance = {0, 1};
          targetPeak->max_luminance = {100, 1};
        }
        else
          colorMetadataReady = false;
      }

      sws->flags = SWS_BILINEAR;
      // stated explicitly: the default intent is relative colorimetric, which clips
      sws->intent = SWS_INTENT_PERCEPTUAL;

      const int res = colorMetadataReady ? sws_scale_frame(sws, dstFrame, srcFrame) : AVERROR(ENOMEM);
      if (res < 0)
        CLog::LogF(LOGWARNING, "capture color conversion failed ({})", res);
      else
        converted = true;
    }
    sws_free_context(&sws);
    av_frame_free(&srcFrame);
    av_frame_free(&dstFrame);
  }
#endif

  if (!converted && displaySDR)
    return false; // Never encode the original HDR codes as sRGB.
  if (!converted)
  {
    // pre-CMS swscale cannot tonemap: plain-convert so the image at least has
    // the right size and depth; the output coding stays baked in
    SwsContext* context =
        sws_getContext(static_cast<int>(result.width), static_cast<int>(result.height), srcFormat,
                       static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
                       SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!context)
      return false;
    const uint8_t* src[] = {srcData, nullptr, nullptr, nullptr};
    const int srcStrides[] = {srcStride, 0, 0, 0};
    uint8_t* dst[] = {buffer, nullptr, nullptr, nullptr};
    const int dstStride[] = {static_cast<int>(width * 4), 0, 0, 0};
    sws_scale(context, src, srcStrides, 0, static_cast<int>(result.height), dst, dstStride);
    sws_freeContext(context);
    converted = true;
  }

  return converted;
}

} // namespace

bool CaptureCopyBGRA8(const CaptureResult& result,
                      unsigned int width,
                      unsigned int height,
                      uint8_t* buffer)
{
  if (!result.pixels || result.width == 0 || result.height == 0 || width == 0 || height == 0)
    return false;

  CScopedCapturePixels lock(*result.pixels);
  if (!lock.data())
    return false;

  return RawToBGRA8(result, CaptureSrcRow0(lock.data(), result.stride, result.height), width,
                    height, buffer);
}

bool CaptureToBGRA(const CaptureResult& result,
                   unsigned int width,
                   unsigned int height,
                   uint8_t* buffer)
{
  if (!result.pixels || result.width == 0 || result.height == 0 || width == 0 || height == 0)
    return false;

  // tonemap by tags, not depth: an 8-bit PQ capture still needs the treatment
  const bool hdr = result.color.transfer == AVCOL_TRC_SMPTE2084 ||
                   result.color.transfer == AVCOL_TRC_ARIB_STD_B67;

  CScopedCapturePixels lock(*result.pixels);
  if (!lock.data())
    return false;
  const uint8_t* src0 = CaptureSrcRow0(lock.data(), result.stride, result.height);

  if (CaptureIsDeep(result.format) || hdr)
    return ConvertColorManaged(result, src0, width, height, buffer);

  return RawToBGRA8(result, src0, width, height, buffer);
}

bool CaptureToDisplaySDR(const CaptureResult& result, CaptureResult& sdr)
{
  if (!result.pixels || !result.width || !result.height || !result.colorEncodingVerified)
    return false;
  CScopedCapturePixels lock(*result.pixels);
  if (!lock.data())
    return false;

  const bool deep = CaptureIsDeep(result.format);
  const AVPixelFormat format = deep ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_BGRA;
  const auto stride = result.width * (deep ? 8 : 4);
  auto pixels = std::make_unique<uint8_t[]>(static_cast<size_t>(stride) * result.height);
  const auto* src0 = CaptureSrcRow0(lock.data(), result.stride, result.height);
  if (!ConvertColorManaged(result, src0, result.width, result.height, pixels.get(), format, true))
    return false;

  sdr.width = result.width;
  sdr.height = result.height;
  sdr.stride = static_cast<int>(stride);
  sdr.format = format;
  sdr.color = {AVCOL_PRI_BT709, AVCOL_TRC_IEC61966_2_1, AVCOL_RANGE_JPEG};
  sdr.pixels = std::make_shared<CHeapCapturePixels>(std::move(pixels));
  return true;
}

} // namespace CAPTURE
} // namespace RENDERING
} // namespace KODI
