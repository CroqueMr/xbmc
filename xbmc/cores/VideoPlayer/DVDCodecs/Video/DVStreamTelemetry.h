/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "DVPlaybackInfo.h"
#include "DVMetadataBuffer.h"
#ifdef CB1_HAS_HDR10_AI
#include "cb1_circuit.h"
#endif

#include <array>
#include <bitset>
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
  struct Transaction
  {
    SourceBinding identity{};
    int requested{},effective{};
    const char* stage{""};
    OutputCapabilities capabilities{};
    OutputReason reason{OutputReason::None};
    int enhancement{}, atomicError{};
    unsigned retryCount{};
    bool restored{};
  } transaction;
  bool referenceWarning{};
  // Committed output is independent of sampled source metadata and counters.
  struct Output
  {
    unsigned flags{}, bits{};
    float target{};
    bool level6{};
    int requested{}, effective{1}, method{}, enhancement{}, panel{}, gamut{};
    double manualPeak{};
    uint64_t revision{};
    bool fallback{};
    uint64_t stream{}, picture{};
    uint32_t format{};
    unsigned width{}, height{};
    float refresh{};
    bool authoredLegacy{};
    std::array<double,3> rawStatistics{};
    float rawSum{};
    uint32_t sampleCount{};
    OutputReason reason{OutputReason::None};
    unsigned minimumLinkBits{};
    bool creativeKnown{};
    uint32_t creativeApplied{}, creativeEdited{};
    int creativeStatus{}, creativeReason{};
#ifdef CB1_HAS_HDR10_AI
    bool hdr10AI{};
    std::array<unsigned,6> aiL1L3{};
    bool emittedL1Known{};
    std::array<unsigned,3> emittedL1{};
    const char* Circuit() const
    {
      if (!flags) return "";
      const bool intense=enhancement==1;
      if (hdr10AI)
      {
        if (effective==1) return cb1_circuit_version(CB1_HDR10_DV_REFERENCE);
        if (effective==4) return cb1_circuit_version(intense ? CB1_HDR10_DV_INTENSE : CB1_HDR10_DV_NATURAL);
      }
      else
      {
        if (effective==1) return cb1_circuit_version(CB1_DV_DV_REFERENCE);
        if (effective==2) return cb1_circuit_version(CB1_DV_HDR10_BASIC);
        if (effective==3) return cb1_circuit_version(CB1_DV_HDR10_EXPERT);
        if (effective==4) return cb1_circuit_version(intense ? CB1_DV_DV_INTENSE : CB1_DV_DV_NATURAL);
      }
      return "";
    }
#endif
  } output;
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
  std::bitset<256> blocks;

  std::bitset<256> AppliedBlocks() const
  {
    std::bitset<256> result;
    if (!output.flags || !metadata) return result;
    result[5]=blocks[5]; // Active-picture geometry, not a creative trim.
    for(unsigned level=0;level<32;++level)
      if (output.creativeApplied & (1u<<level)) result[level]=blocks[level];
    if (output.flags & CDVPlaybackInfo::HDR10 || output.authoredLegacy)
    {
      result[1]=blocks[1] && pq[0]<=pq[2] && pq[2]<=pq[1] && pq[1]>0 && pq[1]<=4095;
      result[6]=blocks[6] && (output.effective>=3 || output.level6);
    }
    return result;
  }
  std::bitset<256> TransmittedBlocks() const
  {
    std::bitset<256> result;
    if (!(output.flags & CDVPlaybackInfo::HDMI) || !metadata) return result;
    if (output.authoredLegacy) { result[1]=result[5]=true; return result; }
    result=blocks;
    result[6]=false; // L6 is static HDR metadata, not dynamic DV wire data.
    result[5]=true; // Native serializer always supplies final-raster margins.
    return result;
  }
};

class StreamTelemetry
{
public:
  inline static std::atomic<bool> observing{false};

  // The adapter session fixes target/precision. Only flag transitions publish.
  static bool Presented(unsigned flags, unsigned bits, float target, bool level6)
  {
    if (CDVPlaybackInfo::presented.load() == flags)
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.output = {flags, bits, target, level6};
    CDVPlaybackInfo::presented.store(flags);
    return true;
  }

  static void ClearOutput()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.output = {};
    m_snapshot.transaction = {};
    m_snapshot.referenceWarning=false;
    m_nativeActive = false;
    m_nativeMetadata.Reset();
    CDVPlaybackInfo::presented.store(0);
  }

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
    const auto output = m_snapshot.output;
    m_snapshot = {};
    m_snapshot.output = output;
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
    StoreMetadata(m_snapshot,data,fel,bits);
  }

  static void NativePresented(const SourceInfo& source,const CDVMetadataBuffer& metadata,
                              const StreamSnapshot::Output& output)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nativeActive = true;
    m_nativeSource = source;
    m_nativeMetadata = metadata;
    m_snapshot.output = output;
    if (m_snapshot.transaction.identity==SourceBinding{output.stream,output.picture,output.revision})
    {
      m_snapshot.output.reason=m_snapshot.transaction.reason;
      m_snapshot.output.requested=m_snapshot.transaction.requested;
      m_snapshot.output.minimumLinkBits=(output.flags & CDVPlaybackInfo::HDR10) ? 10 : 0;
    }
    m_snapshot.referenceWarning=false;
    if (m_snapshot.transaction.identity==SourceBinding{output.stream,output.picture,output.revision})
      m_snapshot.transaction.stage="Committed";
    CDVPlaybackInfo::presented.store(output.flags);
  }

  static void NativeRequest(SourceBinding identity,int requested,int effective,
                            SourceBinding admissionIdentity = {})
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_snapshot.transaction.identity!=identity &&
        (!admissionIdentity.Valid() || m_snapshot.transaction.identity!=admissionIdentity))
      m_snapshot.transaction={};
    auto& transaction=m_snapshot.transaction;
    transaction.identity=identity; transaction.requested=requested;
    transaction.effective=effective; transaction.stage="Requested";
  }
  static void NativeSelection(SourceBinding identity,const OutputCapabilities& capabilities,
                              int requested,int effective,int enhancement,OutputReason reason)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_snapshot.transaction.identity!=identity)
    {
      m_snapshot.transaction={};
      m_snapshot.transaction.stage="Admission checked";
    }
    auto& transaction=m_snapshot.transaction;
    transaction.identity=identity; transaction.capabilities=capabilities;
    transaction.requested=requested; transaction.effective=effective;
    transaction.enhancement=enhancement; transaction.reason=reason;
  }
  static void NativeFailure(OutputReason reason,int error,unsigned retries,bool restored)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& transaction=m_snapshot.transaction;
    transaction.reason=reason; transaction.atomicError=error;
    transaction.retryCount=retries; transaction.restored=restored;
    transaction.stage=restored ? "Activation failed" : "Restoration failed";
  }
  static void NativeStage(SourceBinding identity,const char* stage)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_snapshot.transaction.identity==identity) m_snapshot.transaction.stage=stage;
  }
  static void ReferenceWarning()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.referenceWarning=true;
  }

  static void NativeFormat(SourceBinding identity,uint32_t format,unsigned width,
                           unsigned height,float refresh)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& output=m_snapshot.output;
    if (!m_nativeActive || identity.stream!=output.stream || identity.picture!=output.picture ||
        identity.revision!=output.revision) return;
    output.format=format; output.width=width; output.height=height; output.refresh=refresh;
  }

private:
  static void StoreMetadata(StreamSnapshot& out,const AVDOVIMetadata* data,bool fel,unsigned bits)
  {
    out.metadata = true;
    out.fel = fel;
    out.framebufferBits = bits;
    out.l1 = out.l5 = out.l6 = out.cm4 = false;
    out.blocks.reset();
    for (int i = 0; i < data->num_ext_blocks; ++i)
    {
      const auto* ext = av_dovi_get_ext(data, i);
      out.blocks.set(ext->level);
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

public:
  static StreamSnapshot Read()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_snapshot;
    result.counters = m_counters.load(std::memory_order_relaxed);
    result.dropped = m_dropped.load(std::memory_order_relaxed);
    result.skipped = m_skipped.load(std::memory_order_relaxed);
    if (m_nativeActive)
    {
      result.metadata=false;
      result.blocks.reset();
      const auto& source = m_nativeSource;
      result.profile = source.profile; result.level = source.level;
      result.major = source.major; result.minor = source.minor;
      result.bl = source.bl; result.el = source.el; result.rpu = source.rpu;
      result.sourceBits = source.bits; result.sourceFormat = static_cast<AVPixelFormat>(source.format);
      result.sourceMastering = source.mastering; result.sourceLight = source.light;
      result.sourceMax = source.maxLuma; result.sourceMin = source.minLuma;
      result.sourceCLL = source.maxCLL; result.sourceFALL = source.maxFALL;
      if (observing.load(std::memory_order_relaxed) && m_nativeMetadata.Data())
        StoreMetadata(result,reinterpret_cast<const AVDOVIMetadata*>(m_nativeMetadata.Data()),
            (result.output.flags & CDVPlaybackInfo::FEL) != 0,result.output.bits);
    }
    else
    {
      result.sourceBits = m_sourceBits.load(std::memory_order_relaxed);
      result.sourceFormat = m_sourceFormat.load(std::memory_order_relaxed);
    }
    return result;
  }

private:
  inline static std::atomic<int> m_dropped{}, m_skipped{};
  inline static std::atomic<unsigned> m_sourceBits{0};
  inline static std::atomic<AVPixelFormat> m_sourceFormat{AV_PIX_FMT_NONE};
  inline static std::atomic<bool> m_counters{false};
  inline static std::mutex m_mutex;
  inline static StreamSnapshot m_snapshot;
  inline static SourceInfo m_nativeSource;
  inline static CDVMetadataBuffer m_nativeMetadata;
  inline static bool m_nativeActive{};
  inline static std::chrono::steady_clock::time_point m_next;
};
} // namespace DVBRIDGE
