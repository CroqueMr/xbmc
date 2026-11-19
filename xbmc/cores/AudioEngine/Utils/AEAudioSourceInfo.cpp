/*
 * Copyright (C) 2026 Team Kodi
 * Atmos program/EMDF parsing adapted from avdvplus/xbmc e0614d7,
 * AEStreamInfo.cpp (GPL-2.0-or-later). No payload modification or DTS:X heuristics.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "AEAudioSourceInfo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <limits>
#include <string_view>
#include <fmt/format.h>

extern "C"
{
#include <libavcodec/ac3_parser.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/crc.h>
}

namespace
{
struct Bits
{
  const uint8_t* data;
  unsigned pos;
  unsigned limit;
  bool ok{true};

  bool Skip(unsigned count)
  {
    if (!ok || count > limit || pos > limit - count)
      return ok = false;
    pos += count;
    return true;
  }
  uint32_t Read(unsigned count)
  {
    if (count > 32 || !Skip(count))
    {
      ok = false;
      return 0;
    }
    uint32_t value = 0;
    for (unsigned bit = pos - count; bit < pos; ++bit)
      value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
    return value;
  }
};

bool Program(Bits& bits, int elements, bool trueHD, AE::AudioSourceInfo& info)
{
  if (bits.Read(1))
  {
    const int bed = bits.Read(1);
    if (!bits.ok || elements < bed)
      return false;
    info.bedChannels = bed;
    info.objects = elements - bed;
    return true;
  }
  const auto content = bits.Read(4);
  int bed = 0;
  if (content & 1)
  {
    bits.Skip(1);
    const unsigned beds = bits.Read(1) ? bits.Read(3) + 2 : 1;
    constexpr unsigned widths[] = {2, 1, 1, 2, 2, 2, 2, 2, 2, 1};
    for (unsigned n = 0; n < beds && bits.ok; ++n)
    {
      if (bits.Read(1))
        ++bed;
      else if (bits.Read(1))
      {
        for (unsigned i = 0; i < 10; ++i)
          if (bits.Read(1)) bed += widths[9 - i];
      }
      else
        bed += std::popcount(bits.Read(17));
    }
  }
  int spatial = 0;
  if (content & 2)
  {
    const auto index = bits.Read(3);
    constexpr int isf[] = {4, 8, 10, 14, 15, 30};
    constexpr int trueHDIsf[] = {0, 0, 10, 14, 15, 0, 0, 0};
    if (trueHD)
    {
      spatial = trueHDIsf[index];
      if (!spatial) return false;
    }
    else
    {
      if (index >= std::size(isf)) return false;
      spatial = isf[index];
    }
  }
  int objects = 0;
  if (content & 4)
  {
    auto count = bits.Read(5);
    if (count == 31) count += bits.Read(7);
    objects = static_cast<int>(count) + 1;
  }
  if (content & 8)
  {
    const auto bytes = bits.Read(4) + 1;
    bits.Skip(bytes * 8);
  }
  if (!bits.ok || bed + spatial + objects > elements)
    return false;
  info.bedChannels = bed;
  // An intermediate spatial representation does not declare its original object count.
  info.objects = spatial && !(content & 4) ? -1 : objects;
  return true;
}

uint32_t Variable(Bits& bits, unsigned width)
{
  uint32_t value = 0;
  for (unsigned guard = 0; guard < 8 && bits.ok; ++guard)
  {
    const auto part = bits.Read(width);
    if (value > std::numeric_limits<uint32_t>::max() - part) break;
    value += part;
    if (!bits.Read(1)) return value;
    const uint32_t increment = 1u << width;
    if (value > (std::numeric_limits<uint32_t>::max() - increment) >> width) break;
    value = (value << width) + increment;
  }
  bits.ok = false;
  return 0;
}

bool OAMD(Bits& bits, AE::AudioSourceInfo& info)
{
  auto version = bits.Read(2);
  if (version == 3) version += bits.Read(3);
  if (!bits.ok || version) return false;
  int elements = bits.Read(5) + 1;
  if (elements == 32) elements += bits.Read(7);
  if (!bits.ok || !Program(bits, elements, false, info)) return false;
  info.presentation = elements;
  info.profile = AV_PROFILE_EAC3_DDP_ATMOS;
  return true;
}

bool EMDF(const uint8_t* data, unsigned start, unsigned limit, AE::AudioSourceInfo& info)
{
  Bits bits{data, start, limit};
  bits.Skip(16);
  const auto bytes = bits.Read(16);
  if (!bits.ok || bytes > (limit - bits.pos) / 8) return false;
  bits.limit = bits.pos + bytes * 8;
  if (bits.Read(2) != 0) return false;
  if (bits.Read(3) == 7) Variable(bits, 3);
  for (unsigned count = 0; count < 32 && bits.ok; ++count)
  {
    auto id = bits.Read(5);
    if (id == 31) id += Variable(bits, 5);
    if (!bits.ok || !id) return false;
    const bool offset = bits.Read(1);
    if (offset) bits.Skip(12);
    if (bits.Read(1)) Variable(bits, 11);
    if (bits.Read(1)) Variable(bits, 2);
    if (bits.Read(1)) bits.Skip(8);
    if (!bits.Read(1))
    {
      bool aligned = false;
      if (!offset)
      {
        aligned = bits.Read(1);
        if (aligned) bits.Skip(2);
      }
      if (offset || aligned) bits.Skip(7);
    }
    const auto size = Variable(bits, 8);
    if (!bits.ok || size > (bits.limit - bits.pos) / 8) return false;
    const unsigned end = bits.pos + size * 8;
    if (id == 11)
    {
      Bits payload{data, bits.pos, end};
      if (OAMD(payload, info)) return true;
    }
    bits.pos = end;
  }
  return false;
}

bool ScanOAMD(std::span<const uint8_t> frame, AE::AudioSourceInfo& info)
{
  uint16_t window = 0;
  const unsigned limit = frame.size() * 8;
  for (unsigned bit = 0; bit < limit; ++bit)
  {
    window = (window << 1) | ((frame[bit / 8] >> (7 - bit % 8)) & 1);
    if (bit >= 15 && window == 0x5838 && EMDF(frame.data(), bit - 15, limit, info))
      return true;
  }
  return false;
}

int Dialogue(unsigned value)
{
  return value ? -static_cast<int>(value) : -31;
}

void AC3(std::span<const uint8_t> packet, AE::AudioSourceInfo& info)
{
  uint8_t bsid;
  uint16_t size;
  if (packet.size() < 8 || av_ac3_parse_header(packet.data(), packet.size(), &bsid, &size) < 0 ||
      size < 8 || size > packet.size() || bsid > 16 ||
      av_crc(av_crc_get_table(AV_CRC_16_ANSI), 0, packet.data() + 2, size - 2))
    return;
  const bool enhanced = bsid > 10;
  if (enhanced != (info.codec == AV_CODEC_ID_EAC3)) return;
  Bits bits{packet.data(), enhanced ? 16u : 48u, unsigned(size) * 8};
  unsigned mode;
  if (enhanced)
  {
    const auto type = bits.Read(2);
    const auto substream = bits.Read(3);
    if (type == 1 || type == 3 || substream) return;
    bits.Skip(11);
    bits.Skip(4); // sample-rate code and secondary-rate/block code
    mode = bits.Read(3);
    bits.Skip(1 + 5); // LFE and bsid
  }
  else
  {
    mode = bits.Read(3);
    if ((mode & 1) && mode != 1) bits.Skip(2);
    if (mode & 4) bits.Skip(2);
    if (mode == 2) bits.Skip(2);
    bits.Skip(1);
  }
  const int first = Dialogue(bits.Read(5));
  if (bits.Read(1)) bits.Skip(8);
  if (!enhanced)
  {
    if (bits.Read(1)) bits.Skip(8);
    if (bits.Read(1)) bits.Skip(7);
  }
  std::optional<int> second;
  if (mode == 0) second = Dialogue(bits.Read(5));
  if (!bits.ok) return;
  info.dialnorm = first;
  info.secondDialnorm = second;
  if (enhanced && info.objects < 0) ScanOAMD(packet.first(size), info);
}

void TrueHD(std::span<const uint8_t> packet, AE::AudioSourceInfo& info)
{
  if (packet.size() < 32 || packet[4] != 0xf8 || packet[5] != 0x72 ||
      packet[6] != 0x6f || packet[7] != 0xba || (packet[8] >> 4) == 15)
    return;
  const bool extension = packet[29] & 1;
  const unsigned words = extension ? packet[30] >> 4 : 0;
  const unsigned major = 28 + (extension ? 2 + words * 2 : 0);
  const unsigned unit = (((packet[0] & 0x0f) << 8) | packet[1]) * 2;
  if (unit < major + 4 || unit > packet.size()) return;
  static const auto table = [] {
    std::array<AVCRC, 1024> value{};
    av_crc_init(value.data(), 0, 16, 0x2d, sizeof(value));
    return value;
  }();
  const unsigned crc = av_crc(table.data(), 0, packet.data() + 4, major - 4) ^
                       ((packet[major + 1] << 8) | packet[major]);
  if (crc != unsigned(packet[major + 2] | (packet[major + 3] << 8))) return;
  const bool atmos = packet[21] & 0x80;
  info.profile = atmos ? AV_PROFILE_TRUEHD_ATMOS : AV_PROFILE_UNKNOWN;
  if (!atmos || !extension || !words) return;
  Bits bits{packet.data(), 244, 240 + (words + 1) * 16};
  const int dialogue = Dialogue(bits.Read(5));
  bits.Skip(6);
  const int elements = bits.Read(5) + 1;
  if (!bits.ok || elements > 16) return;
  info.dialnorm = dialogue;
  info.presentation = elements;
  Program(bits, elements, true, info);
}
} // namespace

namespace AE
{
void AudioSourceObserver::Reset(const AudioSourceInfo& source)
{
  m_source = source;
  m_remaining = 512;
}

bool AudioSourceObserver::UpdateProfile(int profile)
{
  // Zero is the audio decoder's unreported/default profile, not a source update.
  if (!m_source.valid || profile == AV_PROFILE_UNKNOWN || profile == 0 || profile == m_source.profile)
    return false;
  m_source.profile = profile;
  return true;
}

bool AudioSourceObserver::Observe(std::span<const uint8_t> packet)
{
  if (!m_source.valid || !m_remaining) return false;
  --m_remaining;
  packet = packet.first(std::min<size_t>(packet.size(), 65536));
  const auto previous = m_source;
  if (m_source.codec == AV_CODEC_ID_TRUEHD)
    TrueHD(packet, m_source);
  else if (m_source.codec == AV_CODEC_ID_AC3 || m_source.codec == AV_CODEC_ID_EAC3)
    AC3(packet, m_source);
  return m_source != previous;
}

bool IsNativePCM(int codec)
{
  const auto* descriptor = avcodec_descriptor_get(static_cast<AVCodecID>(codec));
  return descriptor && std::string_view(descriptor->name).starts_with("pcm_");
}

std::string SourceCodec(const AudioSourceInfo& source)
{
  if (!source.valid) return {};
  switch (source.codec)
  {
    case AV_CODEC_ID_TRUEHD: return "TrueHD";
    case AV_CODEC_ID_EAC3: return "Dolby Digital Plus";
    case AV_CODEC_ID_AC3: return "Dolby Digital";
    case AV_CODEC_ID_DTS:
      switch (source.profile)
      {
        case AV_PROFILE_DTS_HD_HRA: return "DTS-HD HRA";
        case AV_PROFILE_DTS_HD_MA:
        case AV_PROFILE_DTS_HD_MA_X:
        case AV_PROFILE_DTS_HD_MA_X_IMAX: return "DTS-HD MA";
        case AV_PROFILE_DTS_ES: return "DTS-ES";
        case AV_PROFILE_DTS_96_24: return "DTS 96/24";
        default: return "DTS";
      }
    default: break;
  }
  if (IsNativePCM(source.codec)) return "PCM";
  std::string name = avcodec_get_name(static_cast<AVCodecID>(source.codec));
  if (name == "unknown_codec") return {};
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
  return name;
}

std::string SourceTitle(const AudioSourceInfo& source)
{
  auto name = SourceCodec(source);
  if (name.empty()) return name;
  if ((source.codec == AV_CODEC_ID_TRUEHD && source.profile == AV_PROFILE_TRUEHD_ATMOS) ||
      (source.codec == AV_CODEC_ID_EAC3 && source.profile == AV_PROFILE_EAC3_DDP_ATMOS))
    name += " Atmos";
  else if (source.codec == AV_CODEC_ID_DTS &&
           (source.profile == AV_PROFILE_DTS_HD_MA_X || source.profile == AV_PROFILE_DTS_HD_MA_X_IMAX))
    name += " DTS:X";
  return name;
}

std::string FormatSampleRate(int rate)
{
  if (rate <= 0) return {};
  return rate % 1000 ? fmt::format("{:g} kHz", rate / 1000.0) : fmt::format("{} kHz", rate / 1000);
}

std::string SourceFormat(const AudioSourceInfo& source)
{
  if (!source.valid) return {};
  const auto* descriptor = avcodec_descriptor_get(static_cast<AVCodecID>(source.codec));
  auto rate = FormatSampleRate(source.sampleRate);
  if (source.bitsPerSample > 0 && descriptor && (descriptor->props & AV_CODEC_PROP_LOSSLESS))
    return fmt::format("{}-bit{}{}", source.bitsPerSample, rate.empty() ? "" : " / ", rate);
  return rate;
}

std::string FormatChannels(uint64_t mask, int channels)
{
  if (mask && (!channels || std::popcount(mask) == channels))
  {
    AVChannelLayout layout{};
    char name[128];
    if (av_channel_layout_from_mask(&layout, mask) == 0)
    {
      const int length = av_channel_layout_describe(&layout, name, sizeof(name));
      av_channel_layout_uninit(&layout);
      if (length >= 0 && length < int(sizeof(name))) return name;
    }
  }
  return channels > 0 ? fmt::format("{} channels", channels) : std::string{};
}

std::string SourceChannels(const AudioSourceInfo& source)
{
  return source.valid ? FormatChannels(source.channelMask, source.channels) : std::string{};
}

std::string BedObjects(const AudioSourceInfo& source)
{
  if (!source.valid) return {};
  if (source.bedChannels >= 0 && source.objects >= 0)
    return fmt::format("{} / {}", source.bedChannels, source.objects);
  if (source.bedChannels >= 0) return std::to_string(source.bedChannels);
  return source.objects >= 0 ? std::to_string(source.objects) : std::string{};
}

std::string BedObjectsLabel(const AudioSourceInfo& source)
{
  if (source.bedChannels >= 0 && source.objects >= 0) return "Bed / objects";
  return source.bedChannels >= 0 ? "Bed channels" : "Atmos objects";
}

std::string Presentation(const AudioSourceInfo& source)
{
  return source.valid && source.presentation > 0 ? fmt::format("{} elements", source.presentation) : std::string{};
}

std::string Dialnorm(const AudioSourceInfo& source)
{
  if (!source.valid || !source.dialnorm) return {};
  if (source.secondDialnorm)
    return fmt::format("{} / {} LKFS", *source.dialnorm, *source.secondDialnorm);
  return fmt::format("{} LKFS", *source.dialnorm);
}
} // namespace AE
