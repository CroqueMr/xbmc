/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>

enum class VideoOutputType { SDR, HDR10, HLG, DolbyVision, HDR10Plus };
enum class VideoProcessingReason { None, NoDVTransport, NoHDR10Transport,
  NoGPUProcessing, SourceIneligible, TVProfileRequired, ActivationFailed, RestoreFailed,
  NoHDR10PlusTransport, SDRTargetInvalid, ProcessingPending };
enum class VideoCreativeStatus { Invalid, Unsupported, Neutral, Ready };

// How the decoder tagged the picture: FFmpeg AVColor* codes, -1 when unknown.
struct VideoSourceTags
{
  int primaries{-1}, transfer{-1};
  bool primariesGuessed{}, fullRange{};
  std::string enhancement; // "FEL" or "MEL" from Kodi's RPU check, empty without an EL
};

// RPU level values beyond L1/L5/L6, as FFmpeg parsed the latest sampled picture.
// Presence is the matching bit of the parsed block set.
struct VideoDVLevels
{
  static constexpr unsigned MaxTrims = 32;
  struct Target
  {
    uint8_t index{}, primaries{};
    uint16_t maxPQ{}, minPQ{};
  };
  std::array<uint16_t, MaxTrims> l2{}; // target_max_pq of each trim pass
  std::array<uint8_t, MaxTrims> l8{}; // target_display_index of each trim pass
  std::array<Target, MaxTrims> l10{};
  unsigned l2Count{}, l8Count{}, l10Count{};
  std::array<uint16_t, 3> l3{}; // min/max/avg PQ offsets, 2048 is neutral
  std::array<uint16_t, 2> l4{}; // anchor PQ and power
  unsigned l9{}; // source_primary_index
  unsigned contentType{}, whitepoint{};
  bool referenceMode{};
  unsigned dmMode{}, dmVersion{}; // L254
  unsigned runMode{}, runVersion{}; // L255
};

// Native playback facts. Renderer objects and full metadata buffers stay with their owners.
struct VideoProcessingInfo
{
  struct Identity
  {
    uint64_t stream{}, picture{}, revision{};
    bool Valid() const { return stream && picture && revision; }
  };
  struct Pending
  {
    Identity identity{};
    int requested{}, effective{}, atomicError{};
    std::string stage;
    VideoProcessingReason reason{VideoProcessingReason::None};
    unsigned retryCount{};
    bool restored{};
    bool hdr10AI{};
  } transaction;
  struct Output
  {
    unsigned flags{}, bits{};
    float target{};
    bool level6{};
    int requested{}, effective{1}, method{}, panel{}, gamut{};
    double manualPeak{}, displayBlack{}, sdrGamma{};
    unsigned sdrHDRInputUsed{}; // MDCV min/max, MaxCLL fallback, HDR10+ max/avg: bits 1/2/4/8/16.
    std::array<float,4> sdrHDRInputValues{}; // Imported scene_max RGB and scene_avg, in nits.
    uint64_t revision{}, stream{}, picture{};
    uint32_t format{};
    unsigned width{}, height{};
    float refresh{};
    bool fallback{}, authoredLegacy{};
    VideoProcessingReason reason{VideoProcessingReason::None};
    unsigned minimumLinkBits{};
    bool creativeKnown{};
    uint32_t creativeApplied{}, creativeEdited{};
    int creativeStatus{}, creativeReason{};
    bool iptTransport{};
    int felUpscaling{-1};
    bool hdr10AI{}, emittedL1Known{};
    bool hdr10PlusDV{}, untranslatedCurve{}, untranslatedSaturation{};
    unsigned guidedSourceFields{}, guidedEstimatedFields{};
    std::array<unsigned,6> aiL1L3{};
    std::string aiModel{};
    std::array<unsigned,3> aiBaseline{};
    std::array<unsigned,3> emittedL1{};
    std::string engine, circuit;
  } output;
  std::optional<VideoOutputType> outputType;
  uint64_t informationGeneration{};
  bool referenceWarning{};
  unsigned profile{}, level{}, major{}, minor{}, compatibility{};
  bool bl{}, el{}, rpu{}, metadata{}, cm4{}, fel{};
  bool l1{}, l5{}, l6{};
  std::array<unsigned,3> pq{};
  std::array<unsigned,4> area{};
  unsigned maxMaster{}, minMaster{}, maxCLL{}, maxFALL{};
  VideoDVLevels levels;
  unsigned framebufferBits{}, sourceBits{};
  int sourceFormat{-1};
  VideoSourceTags tags;
  int dropped{}, skipped{};
  bool counters{}, sourceMastering{}, sourceLight{};
  double sourceMax{}, sourceMin{};
  unsigned sourceCLL{}, sourceFALL{};
  std::bitset<256> blocks, appliedBlocks, transmittedBlocks;
  const std::bitset<256>& AppliedBlocks() const { return appliedBlocks; }
  const std::bitset<256>& TransmittedBlocks() const { return transmittedBlocks; }
};
