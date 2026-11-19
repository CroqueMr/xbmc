/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>
extern "C"
{
#include <libavcodec/packet.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

// A value owned by the demuxer which read this clip, never a later global play item.
struct CBlurayVideoWindow
{
  bool active{false};
  int64_t first{0}, last{0}; // authored source PTS in 90 kHz
  bool DecodeOnly(int64_t pts, AVRational clock) const
  {
    if (!active)
      return false;
    if (pts == AV_NOPTS_VALUE || clock.num <= 0 || clock.den <= 0)
      return true;
    // FFmpeg can unwrap MPEG-TS to negative timestamps near its 33-bit rollover.
    // Compare exact rationals in the authored epoch, without rounding a boundary
    // or changing either decoder's clock. The products fit in signed 128 bits.
    const __int128 scale = clock.den;
    const __int128 period = (__int128{1} << 33) * scale;
    __int128 source = (__int128{pts} * clock.num * 90000) % period;
    if (source < 0)
      source += period;
    return source < __int128{first} * scale || source > __int128{last} * scale;
  }
};

// Present separate HEVC layers as the joined stream consumed by CB1.
class CBlurayDovi
{
public:
  explicit CBlurayDovi(uint64_t* epochs = nullptr, bool transportClock = false)
    : m_epochs(epochs), m_transportClock(transportClock) { Reset(); }
  // Preserve each layer's HEVC configuration and time base.
  bool Configure(const AVCodecParameters* base, const AVCodecParameters* enhancement,
                 AVRational baseClock, AVRational enhancementClock, bool transportClock = false)
  {
    const auto* filter = av_bsf_get_by_name("hevc_mp4toannexb");
    if (!filter || baseClock.num <= 0 || baseClock.den <= 0 ||
        enhancementClock.num <= 0 || enhancementClock.den <= 0)
      return false;
    std::array<Filter, 2> prepared;
    const AVCodecParameters* parameters[] = {base, enhancement};
    const AVRational clocks[] = {baseClock, enhancementClock};
    for (unsigned i = 0; i < prepared.size(); ++i)
    {
      AVBSFContext* context = nullptr;
      if (av_bsf_alloc(filter, &context) < 0)
        return false;
      prepared[i].reset(context);
      if (avcodec_parameters_copy(context->par_in, parameters[i]) < 0)
        return false;
      context->time_base_in = clocks[i];
      if (av_bsf_init(context) < 0)
        return false;
    }
    m_filters = std::move(prepared);
    m_transportClock = transportClock;
    return true;
  }
  const AVCodecParameters* BaseParameters() const { return m_filters[0]->par_out; }
  const AVCodecParameters* OriginalBaseParameters() const
  { return m_filters[0] ? m_filters[0]->par_in : nullptr; }

  void Reset(bool resync = false)
  {
    for (auto& filter : m_filters)
      if (filter) av_bsf_flush(filter.get());
    m_base.clear();
    m_enhancement.clear();
    m_rpu.clear();
    m_bytes = 0;
    m_error = "";
    m_resync = resync;
    m_preroll = 0;
    m_lastDts = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    m_discontinuity = false;
    m_restartPeer = -1;
    m_elAnchor = AV_NOPTS_VALUE;
    m_timing = {};
    m_epoch = m_epochs ? ++*m_epochs : m_epoch + 1;
  }

  bool Push(const AVPacket* packet, bool enhancement)
  {
    m_error = "";
    if (!packet || packet->size <= 0 || static_cast<size_t>(packet->size) > MAX_BYTES)
      return Reject("packet");
    Packet normalized;
    if (m_filters[enhancement])
    {
      Packet input(av_packet_clone(packet));
      normalized.reset(av_packet_alloc());
      auto* filter = m_filters[enhancement].get();
      if (!input || !normalized || av_bsf_send_packet(filter, input.get()) < 0 ||
          av_bsf_receive_packet(filter, normalized.get()) < 0)
        return Reject("hevc-normalization");
      const auto from = filter->time_base_out, to = m_filters[0]->time_base_out;
      if (av_cmp_q(from, to))
      {
        const auto pts = normalized->pts;
        av_packet_rescale_ts(normalized.get(), from, to);
        // PTS identifies the picture; DTS/duration use FFmpeg's native rescaling.
        if (pts != AV_NOPTS_VALUE && av_compare_ts(pts, from, normalized->pts, to))
          return Reject("layer-time-base");
      }
      packet = normalized.get();
    }
    if (m_transportClock && enhancement && packet->pts != AV_NOPTS_VALUE &&
        packet->dts != AV_NOPTS_VALUE && packet->pts < packet->dts)
      return Reject("transport-presentation-before-decode");
    // Wait for both transport clocks before accepting a new layer epoch.
    if (m_restartPeer == int(enhancement))
    {
      if (packet->dts == AV_NOPTS_VALUE) return Reject("restart-peer-dts");
      if (packet->dts >= m_lastDts[enhancement]) return SkipPreroll();
      m_restartPeer = -1;
      m_lastDts[enhancement] = packet->dts;
    }
    if (packet->dts != AV_NOPTS_VALUE)
    {
      if (m_lastDts[enhancement] != AV_NOPTS_VALUE && packet->dts < m_lastDts[enhancement])
      {
        if (m_restartPeer != -1) return Reject("overlapping-discontinuity");
        const auto peerDts = m_lastDts[!enhancement];
        if (peerDts == AV_NOPTS_VALUE) return Reject("missing-peer-dts");
        Reset(true);
        m_lastDts[!enhancement] = peerDts;
        m_restartPeer = !enhancement;
        m_discontinuity = true;
      }
      m_lastDts[enhancement] = packet->dts;
    }
    if (static_cast<size_t>(packet->size) > MAX_BYTES - m_bytes)
      return Reject("byte-limit");
    if (packet->pts == AV_NOPTS_VALUE)
    {
      if (m_resync && !enhancement) return SkipPreroll();
      return Reject("packet");
    }
    auto& queue = enhancement ? m_enhancement : m_base;
    if (queue.size() >= MAX_PACKETS)
      return Reject(enhancement ? "enhancement-queue" : "base-queue");
    if (std::any_of(queue.begin(), queue.end(), [&](const Packet& p) { return p->pts == packet->pts; }))
      return Reject("duplicate-layer-pts");

    std::vector<uint8_t> layer, rpu;
    auto begin = Start(packet->data, packet->size, 0);
    if (begin[0] != 0)
      return Reject("annex-b");
    while (begin[0] < packet->size)
    {
      const auto next = Start(packet->data, packet->size, begin[1]);
      const int size = next[0] - begin[1];
      if (size < 2 || packet->data[begin[1]] & 0x80 || !(packet->data[begin[1]+1] & 7))
        return Reject("nal-header");
      const int type = (packet->data[begin[1]] >> 1) & 63;
      if (type == 62)
      {
        if (!rpu.empty()) return Reject("multiple-rpu");
        if (!enhancement)
          layer.insert(layer.end(), packet->data, packet->data + begin[0]);
        rpu.insert(rpu.end(), {0, 0, 0, 1});
        rpu.insert(rpu.end(), packet->data + begin[1], packet->data + next[0]);
      }
      else if (enhancement)
      {
        layer.insert(layer.end(), {0, 0, 0, 1, 0x7e, 1});
        layer.insert(layer.end(), packet->data + begin[1], packet->data + next[0]);
      }
      else if (!rpu.empty())
        layer.insert(layer.end(), packet->data + begin[0], packet->data + next[0]);
      if (layer.size() + rpu.size() > MAX_BYTES - m_bytes)
        return Reject("byte-limit");
      begin = next;
    }
    if ((enhancement || !rpu.empty()) && layer.empty()) return Reject("missing-layer");
    const auto saved = m_rpu.find(packet->pts);
    const unsigned layerBit = enhancement ? 2 : 1;
    if (saved != m_rpu.end() && (saved->second.layers & layerBit))
      return Reject("duplicate-layer-pts");
    if (!rpu.empty() && saved != m_rpu.end() && !saved->second.bytes.empty() &&
        saved->second.bytes != rpu)
      return Reject("conflicting-rpu");
    const bool store = !rpu.empty() && (saved == m_rpu.end() || saved->second.bytes.empty());
    if (saved == m_rpu.end() && m_rpu.size() >= 2 * MAX_PACKETS)
      return Reject("rpu-queue");
    Packet owned;
    if (!enhancement && rpu.empty())
      owned.reset(av_packet_clone(packet));
    else
    {
      owned.reset(av_packet_alloc());
      if (!owned || av_new_packet(owned.get(), layer.size()) < 0 ||
          av_packet_copy_props(owned.get(), packet) < 0)
        return Reject("allocation");
      std::memcpy(owned->data, layer.data(), layer.size());
    }
    if (!owned) return Reject("allocation");
    if (enhancement && m_resync && m_elAnchor == AV_NOPTS_VALUE && IsIrap(*owned, true))
      m_elAnchor = packet->pts;
    const size_t bytes = owned->size + (store ? rpu.size() : 0);
    if (bytes > MAX_BYTES - m_bytes) return Reject("byte-limit");
    m_bytes += bytes;
    queue.push_back(std::move(owned));
    auto& record = m_rpu[packet->pts];
    record.layers |= layerBit;
    if (store) record.bytes = std::move(rpu);
    if (record.emitted && record.layers == 3)
    {
      m_bytes -= record.bytes.size();
      m_rpu.erase(packet->pts);
    }
    return true;
  }

  int Take(AVPacket* output, bool end = false)
  {
    m_timing = {};
    if (m_base.empty())
    {
      if (!end) return AVERROR(EAGAIN);
      if (m_enhancement.empty())
      {
        m_rpu.clear();
        m_bytes = 0;
        return AVERROR_EOF;
      }
      m_timing = {AV_NOPTS_VALUE, m_enhancement.front()->pts, m_epoch, false};
      m_bytes -= m_enhancement.front()->size;
      av_packet_move_ref(output, m_enhancement.front().get());
      m_enhancement.pop_front();
      return 0; // Explicit EL-only terminal unit; never sent to the BL decoder.
    }
    if (m_enhancement.empty() && !end)
      return AVERROR(EAGAIN);
    auto rpu = m_rpu.find(m_base.front()->pts);
    const bool missing = rpu == m_rpu.end() || rpu->second.bytes.empty();
    // At EOF no more metadata can arrive. Keep the BL for reference decoding
    // even without its own RPU; Timing().presentable excludes that picture.
    const bool seekPreroll = m_resync && m_transportClock &&
        m_lastDts[1] != AV_NOPTS_VALUE && m_base.front()->pts < m_lastDts[1];
    if (missing && !end && !seekPreroll)
      return AVERROR(EAGAIN);
    const auto* layer = m_enhancement.empty() ? nullptr : m_enhancement.front().get();
    const int baseSize = m_base.front()->size;
    const size_t rpuSize = missing ? 0 : rpu->second.bytes.size();
    const size_t extra = (layer ? layer->size : 0) + rpuSize;
    if (extra > MAX_BYTES - static_cast<size_t>(baseSize))
      return AVERROR_INVALIDDATA;
    Packet joined(av_packet_clone(m_base.front().get()));
    if (!joined || av_grow_packet(joined.get(), extra) < 0)
      return AVERROR(ENOMEM);
    if (layer) std::memcpy(joined->data + baseSize, layer->data, layer->size);
    if (rpuSize)
      std::memcpy(joined->data + baseSize + (layer ? layer->size : 0), rpu->second.bytes.data(), rpuSize);
    m_timing = {joined->pts, layer ? layer->pts : AV_NOPTS_VALUE, m_epoch, !missing};
    m_bytes -= baseSize + (layer ? layer->size : 0);
    av_packet_move_ref(output, joined.get());
    m_base.pop_front();
    if (layer) m_enhancement.pop_front();
    if (rpu != m_rpu.end())
    {
      rpu->second.emitted = true;
      if (missing || rpu->second.layers == 3)
      {
        m_bytes -= rpu->second.bytes.size();
        m_rpu.erase(rpu);
      }
    }
    if (!missing && m_elAnchor != AV_NOPTS_VALUE && output->pts >= m_elAnchor)
      m_resync = false;
    return 0;
  }

  struct SourceTiming
  {
    int64_t basePts{AV_NOPTS_VALUE}, enhancementPts{AV_NOPTS_VALUE};
    uint64_t epoch{0};
    bool presentable{false};
  };
  SourceTiming Timing() const { return m_timing; }

  bool Pending() const { return !m_base.empty() || !m_enhancement.empty() || !m_rpu.empty(); }
  const char* LastError() const { return m_error; }
  struct QueueState
  {
    int64_t basePts, baseDts, elPts, elDts;
    size_t basePackets, elPackets, rpuPackets;
    bool resync, baseIrap, elIrap;
    int restartPeer;
  };
  QueueState GetState() const
  {
    const auto* base = m_base.empty() ? nullptr : m_base.front().get();
    const auto* el = m_enhancement.empty() ? nullptr : m_enhancement.front().get();
    return {base ? base->pts : AV_NOPTS_VALUE, base ? base->dts : AV_NOPTS_VALUE,
            el ? el->pts : AV_NOPTS_VALUE, el ? el->dts : AV_NOPTS_VALUE,
            m_base.size(), m_enhancement.size(), size_t(std::count_if(m_rpu.begin(), m_rpu.end(),
              [](const auto& item) { return !item.second.bytes.empty(); })),
            m_resync, base && IsIrap(*base, false), el && IsIrap(*el, true), m_restartPeer};
  }
  int64_t BasePTS() const { return m_base.empty() ? AV_NOPTS_VALUE : m_base.front()->pts; }
  int64_t RpuPTS() const
  {
    const auto entry = std::find_if(m_rpu.begin(), m_rpu.end(),
      [](const auto& item) { return !item.second.bytes.empty(); });
    return entry == m_rpu.end() ? AV_NOPTS_VALUE : entry->first;
  }
  size_t TakePrerollCount() { const auto count=m_preroll; m_preroll=0; return count; }
  bool TakeDiscontinuity() { const bool changed=m_discontinuity; m_discontinuity=false; return changed; }

private:
  bool Reject(const char* reason) { m_error = reason; return false; }
  bool SkipPreroll()
  {
    if (m_preroll >= MAX_PACKETS) return Reject("seek-preroll-limit");
    ++m_preroll;
    return true;
  }
  static constexpr size_t MAX_PACKETS = 32;
  static constexpr size_t MAX_BYTES = 64u * 1024u * 1024u;
  struct Release { void operator()(AVPacket* p) const { av_packet_free(&p); } };
  using Packet = std::unique_ptr<AVPacket, Release>;
  struct ReleaseFilter { void operator()(AVBSFContext* p) const { av_bsf_free(&p); } };
  using Filter = std::unique_ptr<AVBSFContext, ReleaseFilter>;
  std::array<Filter, 2> m_filters;

  static bool IsIrap(const AVPacket& packet, bool wrapped)
  {
    bool irap = false;
    auto begin = Start(packet.data, packet.size, 0);
    if (begin[0] != 0)
      return false;
    while (begin[0] < packet.size)
    {
      const auto next = Start(packet.data, packet.size, begin[1]);
      int payload = begin[1];
      int size = next[0] - payload;
      if (wrapped)
      {
        if (size < 4 || ((packet.data[payload] >> 1) & 63) != 63)
          return false;
        payload += 2;
        size -= 2;
      }
      if (size < 2 || (packet.data[payload] & 0x80) || !(packet.data[payload+1] & 7))
        return false;
      const int type = (packet.data[payload] >> 1) & 63;
      if (type < 32)
      {
        if (type < 16 || type > 23)
          return false;
        irap = true;
      }
      begin = next;
    }
    return irap;
  }

  // Return the prefix and payload positions of the next Annex-B NAL.
  static std::array<int, 2> Start(const uint8_t* data, int size, int from)
  {
    for (int i = from; i < size - 2; ++i)
    {
      if (data[i] || data[i+1])
        continue;
      if (data[i+2] == 1)
        return {i, i+3};
      if (i < size - 3 && !data[i+2] && data[i+3] == 1)
        return {i, i+4};
    }
    return {size, size};
  }

  std::deque<Packet> m_base, m_enhancement;
  // Keep each layer's decode order; associate RPU only by presentation time.
  struct Rpu
  {
    std::vector<uint8_t> bytes;
    unsigned layers{0};
    bool emitted{false};
  };
  // Retain emitted metadata until the peer arrives, so late conflicts cannot escape validation.
  std::map<int64_t, Rpu> m_rpu;
  size_t m_bytes{0};
  const char* m_error{""};
  bool m_resync{false};
  size_t m_preroll{0};
  std::array<int64_t, 2> m_lastDts{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
  bool m_discontinuity{false};
  int m_restartPeer{-1};
  int64_t m_elAnchor{AV_NOPTS_VALUE};
  uint64_t m_epoch{1};
  uint64_t* m_epochs{};
  bool m_transportClock{false};
  SourceTiming m_timing;
};
