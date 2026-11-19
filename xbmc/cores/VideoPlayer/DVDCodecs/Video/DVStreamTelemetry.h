/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
}

namespace DVBRIDGE
{
struct StreamSnapshot
{
  unsigned profile{}, level{}, major{}, minor{};
  bool bl{}, el{}, rpu{}, metadata{}, cm4{}, fel{};
  bool l1{}, l5{}, l6{};
  std::array<unsigned, 3> pq{};
  std::array<unsigned, 4> area{};
  unsigned maxMaster{}, minMaster{}, maxCLL{}, maxFALL{};
  unsigned framebufferBits{}, sourceBits{};
  AVPixelFormat sourceFormat{AV_PIX_FMT_NONE};
  int dropped{}, skipped{};
  bool counters{};
  bool sourceMastering{}, sourceLight{};
  double sourceMax{}, sourceMin{};
  unsigned sourceCLL{}, sourceFALL{};
};

class StreamTelemetry
{
public:
  inline static std::atomic<bool> observing{false};

  static void SourceBits(unsigned bits, AVPixelFormat format)
  {
    m_sourceBits.store(bits, std::memory_order_relaxed);
    m_sourceFormat.store(format, std::memory_order_relaxed);
  }

  static void Counters(int dropped, int skipped)
  {
    m_dropped.store(dropped, std::memory_order_relaxed);
    m_skipped.store(skipped, std::memory_order_relaxed);
    m_counters.store(true, std::memory_order_relaxed);
  }

  static void StaticMetadata(const AVMasteringDisplayMetadata* mastering,
                             const AVContentLightMetadata* light)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (mastering && mastering->has_luminance &&
        mastering->max_luminance.den > 0 && mastering->min_luminance.den > 0)
    {
      m_snapshot.sourceMastering = true;
      m_snapshot.sourceMax = av_q2d(mastering->max_luminance);
      m_snapshot.sourceMin = av_q2d(mastering->min_luminance);
    }
    if (light)
    {
      m_snapshot.sourceLight = true;
      m_snapshot.sourceCLL = light->MaxCLL;
      m_snapshot.sourceFALL = light->MaxFALL;
    }
  }

  static void Begin(unsigned profile, unsigned level, unsigned major, unsigned minor,
                    bool bl, bool el, bool rpu)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = {};
    m_counters.store(false, std::memory_order_relaxed);
    m_sourceBits.store(0, std::memory_order_relaxed);
    m_sourceFormat.store(AV_PIX_FMT_NONE, std::memory_order_relaxed);
    m_snapshot.profile = profile;
    m_snapshot.level = level;
    m_snapshot.major = major;
    m_snapshot.minor = minor;
    m_snapshot.bl = bl;
    m_snapshot.el = el;
    m_snapshot.rpu = rpu;
    m_next = {};
  }

  // Called on the renderer thread only, after its metadata bounds validation.
  static void Metadata(const AVDOVIMetadata* data, bool fel, unsigned bits)
  {
    if (!observing.load(std::memory_order_relaxed))
      return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (now < m_next)
      return;
    m_next = now + std::chrono::milliseconds(500);
    auto& out = m_snapshot;
    out.metadata = true;
    out.fel = fel;
    out.framebufferBits = bits;
    out.l1 = out.l5 = out.l6 = out.cm4 = false;
    for (int i = 0; i < data->num_ext_blocks; ++i)
    {
      const auto* ext = av_dovi_get_ext(data, i);
      switch (ext->level)
      {
        case 1:
          out.l1 = true;
          out.pq = {ext->l1.min_pq, ext->l1.max_pq, ext->l1.avg_pq};
          break;
        case 5:
          out.l5 = true;
          out.area = {ext->l5.left_offset, ext->l5.right_offset,
                      ext->l5.top_offset, ext->l5.bottom_offset};
          break;
        case 6:
          out.l6 = true;
          out.maxMaster = ext->l6.max_luminance;
          out.minMaster = ext->l6.min_luminance;
          out.maxCLL = ext->l6.max_cll;
          out.maxFALL = ext->l6.max_fall;
          break;
        case 3: case 8: case 9: case 10: case 11: case 254:
          out.cm4 = true;
          break;
      }
    }
  }

  static StreamSnapshot Read()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_snapshot;
    result.counters = m_counters.load(std::memory_order_relaxed);
    result.dropped = m_dropped.load(std::memory_order_relaxed);
    result.skipped = m_skipped.load(std::memory_order_relaxed);
    result.sourceBits = m_sourceBits.load(std::memory_order_relaxed);
    result.sourceFormat = m_sourceFormat.load(std::memory_order_relaxed);
    return result;
  }

private:
  inline static std::atomic<int> m_dropped{}, m_skipped{};
  inline static std::atomic<unsigned> m_sourceBits{0};
  inline static std::atomic<AVPixelFormat> m_sourceFormat{AV_PIX_FMT_NONE};
  inline static std::atomic<bool> m_counters{false};
  inline static std::mutex m_mutex;
  inline static StreamSnapshot m_snapshot;
  inline static std::chrono::steady_clock::time_point m_next;
};
} // namespace DVBRIDGE
