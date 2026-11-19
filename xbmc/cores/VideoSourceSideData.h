/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

extern "C"
{
#include <libavcodec/codec_id.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Original source facts, owned with a picture and published only after acceptance.
// Logical data is immutable; only the on-demand wire representation is cached.
class VideoSourceSideData
{
public:
  enum class Hybrid
  {
    DolbyVision,
    ProbeHDR10Plus,
    HDR10Plus,
  };

  static std::shared_ptr<const VideoSourceSideData> Capture(
      uint64_t generation, uint64_t serial, AVCodecID codec, const AVFrame* frame,
      const AVDOVIDecoderConfigurationRecord* config,
      const AVMasteringDisplayMetadata* mastering = nullptr,
      const AVContentLightMetadata* light = nullptr,
      Hybrid hybrid = Hybrid::DolbyVision) noexcept;
  uint64_t Generation() const { return m_generation; }
  uint64_t Serial() const { return m_serial; }
  unsigned RejectedSections() const { return m_rejected; }
  const AVDynamicHDRPlus* HDR10Plus() const { return m_hdr10plus.get(); }
  bool HDR10PlusIsPrimary() const { return m_hdr10PlusPrimary; }
  const std::string& JSON() const;

private:
  VideoSourceSideData(uint64_t generation, uint64_t serial)
    : m_generation(generation), m_serial(serial) {}
  const uint64_t m_generation, m_serial;
  bool m_hdr10PlusPrimary{};
  unsigned m_rejected{};
  std::vector<uint8_t> m_config, m_rpu, m_mdcv, m_cll;
  std::unique_ptr<const AVDynamicHDRPlus> m_hdr10plus;
  mutable std::once_flag m_jsonOnce;
  mutable std::string m_json;
};
