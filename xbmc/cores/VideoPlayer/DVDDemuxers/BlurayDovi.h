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
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

// Present separate Blu-ray layers as the joined HEVC stream consumed by CB1.
class CBlurayDovi
{
public:
  void Reset()
  {
    m_base.clear();
    m_enhancement.clear();
    m_rpu.clear();
    m_bytes = 0;
  }

  bool Push(const AVPacket* packet, bool enhancement)
  {
    if (!packet || packet->pts == AV_NOPTS_VALUE || packet->size <= 0 ||
        static_cast<size_t>(packet->size) > MAX_BYTES - m_bytes)
      return false;
    if (!enhancement)
    {
      if (m_base.size() >= MAX_PACKETS)
        return false;
      Packet owned(av_packet_clone(packet));
      if (!owned)
        return false;
      m_bytes += packet->size;
      m_base.push_back(std::move(owned));
      return true;
    }
    if (m_enhancement.size() >= MAX_PACKETS || m_rpu.size() >= MAX_PACKETS ||
        m_rpu.contains(packet->pts))
      return false;

    std::vector<uint8_t> layer, rpu;
    auto begin = Start(packet->data, packet->size, 0);
    if (begin[0] != 0)
      return false;
    while (begin[0] < packet->size)
    {
      const auto next = Start(packet->data, packet->size, begin[1]);
      const int size = next[0] - begin[1];
      if (size < 2 || packet->data[begin[1]] & 0x80 || !(packet->data[begin[1]+1] & 7))
        return false;
      const int type = (packet->data[begin[1]] >> 1) & 63;
      auto& destination = type == 62 ? rpu : layer;
      destination.insert(destination.end(), {0, 0, 0, 1});
      if (type != 62)
        destination.insert(destination.end(), {0x7e, 1});
      destination.insert(destination.end(), packet->data + begin[1], packet->data + next[0]);
      if (layer.size() + rpu.size() > MAX_BYTES - m_bytes)
        return false;
      begin = next;
    }
    if (layer.empty() || rpu.empty())
      return false;
    Packet owned(av_packet_alloc());
    if (!owned || av_new_packet(owned.get(), layer.size()) < 0)
      return false;
    std::memcpy(owned->data, layer.data(), layer.size());
    m_bytes += layer.size() + rpu.size();
    m_enhancement.push_back(std::move(owned));
    m_rpu.emplace(packet->pts, std::move(rpu));
    return true;
  }

  int Take(AVPacket* output)
  {
    if (m_base.empty() || m_enhancement.empty())
      return AVERROR(EAGAIN);
    auto rpu = m_rpu.find(m_base.front()->pts);
    if (rpu == m_rpu.end())
      return AVERROR(EAGAIN);
    const auto& layer = m_enhancement.front();
    const int baseSize = m_base.front()->size;
    const size_t extra = layer->size + rpu->second.size();
    if (extra > MAX_BYTES - static_cast<size_t>(baseSize))
      return AVERROR_INVALIDDATA;
    Packet joined(av_packet_clone(m_base.front().get()));
    if (!joined || av_grow_packet(joined.get(), extra) < 0)
      return AVERROR(ENOMEM);
    std::memcpy(joined->data + baseSize, layer->data, layer->size);
    std::memcpy(joined->data + baseSize + layer->size, rpu->second.data(), rpu->second.size());
    m_bytes -= baseSize + extra;
    av_packet_move_ref(output, joined.get());
    m_base.pop_front();
    m_enhancement.pop_front();
    m_rpu.erase(rpu);
    return 0;
  }

  bool Pending() const { return !m_base.empty() || !m_enhancement.empty() || !m_rpu.empty(); }

private:
  static constexpr size_t MAX_PACKETS = 32;
  static constexpr size_t MAX_BYTES = 64u * 1024u * 1024u;
  struct Release { void operator()(AVPacket* p) const { av_packet_free(&p); } };
  using Packet = std::unique_ptr<AVPacket, Release>;

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
  std::map<int64_t, std::vector<uint8_t>> m_rpu;
  size_t m_bytes{0};
};
