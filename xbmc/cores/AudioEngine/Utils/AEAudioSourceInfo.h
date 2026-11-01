/*
 * Copyright (C) 2026 Team Kodi
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace AE
{
struct AudioSourceInfo
{
  bool valid{false};
  int uniqueId{-1};
  int64_t demuxerId{-1};
  int codec{0};
  int profile{-99};
  int channels{0};
  int sampleRate{0};
  int bitsPerSample{0};
  uint64_t channelMask{0};
  int bedChannels{-1};
  int objects{-1};
  int presentation{-1};
  std::optional<int> dialnorm;
  std::optional<int> secondDialnorm;

  bool operator==(const AudioSourceInfo&) const = default;
};

// Observes accepted source packets. It never decodes or changes their contents.
class AudioSourceObserver
{
public:
  void Reset(const AudioSourceInfo& source);
  bool Observe(std::span<const uint8_t> packet);
  bool UpdateProfile(int profile);
  const AudioSourceInfo& Get() const { return m_source; }

private:
  AudioSourceInfo m_source;
  unsigned m_remaining{0};
};

std::string SourceCodec(const AudioSourceInfo& source);
std::string SourceTitle(const AudioSourceInfo& source);
std::string SourceFormat(const AudioSourceInfo& source);
std::string SourceChannels(const AudioSourceInfo& source);
std::string BedObjects(const AudioSourceInfo& source);
std::string BedObjectsLabel(const AudioSourceInfo& source);
std::string Presentation(const AudioSourceInfo& source);
std::string Dialnorm(const AudioSourceInfo& source);
std::string FormatChannels(uint64_t mask, int channels);
std::string FormatSampleRate(int sampleRate);
bool IsNativePCM(int codec);
} // namespace AE
