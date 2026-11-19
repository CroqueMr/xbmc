/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <string>
#include <sstream>
#include <bitset>
#include <array>
#include <cstdint>

namespace DVBRIDGE
{
enum class OutputReason { None, NoDVTransport, NoHDR10Transport,
  NoGPUProcessing, SourceIneligible, TVProfileRequired, ActivationFailed, RestoreFailed,
  NoHDR10PlusTransport };
struct ModeAdmission { bool usable{}; OutputReason reason{OutputReason::None}; };
struct OutputCapabilities
{
  bool sourceKnown{}, isDV{}, processingKnown{}, nativeDV{}, hdr10{};
  bool stereoSource{};
  uint64_t generation{};
  std::array<ModeAdmission, 8> processing{};
  bool isHLG{}; // Plain HLG only: Dolby Vision RPU/configuration takes precedence.
  bool isHDR10{};
  bool hdr10PlusSource{}; // Native source classification, independent of metadata admission.
  bool isHDR10Plus{}; // Selected decoded picture, not receiver HDR10+ transport.
  std::array<ModeAdmission, 8> hdr10Processing{};
  bool hdr10plus{};
};
}

// The render thread publishes one coherent snapshot only after presentation.
// This is output telemetry, never a source-format or TV certification flag.
struct CDVPlaybackInfo
{
  template<class Output> static bool IsHDR10AI(const Output& output)
  {
    if constexpr (requires { output.hdr10AI; }) return output.hdr10AI;
    return false;
  }
  template<class Output> static bool IsHDR10PlusDV(const Output& output)
  {
    if constexpr (requires { output.hdr10PlusDV; }) return output.hdr10PlusDV;
    return false;
  }
  enum : unsigned { HDMI = 1, FEL = 2, CM4 = 4, HDR10 = 8, HDR10PLUS = 16, SDR = 32 };
  inline static std::atomic<unsigned> presented{0};
  static int ReasonLabel(DVBRIDGE::OutputReason reason)
  { return reason == DVBRIDGE::OutputReason::NoHDR10PlusTransport ? 40923 :
      reason == DVBRIDGE::OutputReason::None ? 0 : 40840 + static_cast<int>(reason); }

  static const char* Reason(DVBRIDGE::OutputReason reason)
  {
    switch(reason)
    {
      case DVBRIDGE::OutputReason::NoDVTransport: return "no-dv-transport";
      case DVBRIDGE::OutputReason::NoHDR10Transport: return "no-hdr10-transport";
      case DVBRIDGE::OutputReason::NoGPUProcessing: return "gpu-processing-unavailable";
      case DVBRIDGE::OutputReason::SourceIneligible: return "source-ineligible";
      case DVBRIDGE::OutputReason::TVProfileRequired: return "tv-profile-required";
      case DVBRIDGE::OutputReason::ActivationFailed: return "activation-failed";
      case DVBRIDGE::OutputReason::RestoreFailed: return "restoration-failed";
      case DVBRIDGE::OutputReason::NoHDR10PlusTransport: return "no-hdr10plus-transport";
      default: return "none";
    }
  }

  static std::string Detail(std::string source, unsigned output)
  {
    if (!(output & (HDMI | HDR10)))
      return source;
    if ((output & FEL) && source.find("FEL") == std::string::npos)
      source += " FEL";
    source += (output & CM4) ? " · CM4 metadata" : " · CM2.9 metadata";
    return source + ((output & HDR10) ? " · HDR10 Rendering" : " · HDMI Dolby Vision");
  }

  template<class Output> static const char* Policy(const Output& output)
  {
    if (IsHDR10PlusDV(output)) return "Dolby Vision Rendering";
    switch (output.effective)
    {
      case 2: return "HDR10 Rendering";
      case 3: return "HDR10 Rendering Reference";
      case 4: return "Dolby Vision Adaptive HDR Mapping";
      case 5: return "Dolby Vision Master Reference";
      case 6: return "Dolby Vision Adaptive HDR Mapping";
      case 8: return "SDR Rendering";
      case 7: return output.flags & HDR10PLUS ? "HDR10+ Rendering" : "HDR10 Rendering";
      default: return output.flags & HDR10 ? "HDR10 Rendering" : "Dolby Vision Reference";
    }
  }
  static const char* RequestedPolicy(int mode, bool hdr10AI = false, bool hdr10PlusDV = false)
  {
    if (hdr10PlusDV && mode == 1) return "Dolby Vision Rendering";
    if (hdr10AI && mode == 1) return "Dolby Vision Rendering";
    if (hdr10AI && mode == 4) return "Dolby Vision Adaptive HDR Mapping (AI)";
    switch(mode)
    {
      case 0:return "Dolby Vision Disabled (native Kodi)";
      case 1:return "Dolby Vision Reference";
      case 2:return "HDR10 Rendering";
      case 3:return "HDR10 Rendering Reference";
      case 4:return "Dolby Vision Adaptive HDR Mapping";
      case 5:return "Dolby Vision Master Reference";
      case 6:return "Dolby Vision Adaptive HDR Mapping";
      case 7:return "HDR10+ Rendering";
      case 8:return "SDR Rendering";
      default:return "";
    }
  }

  template<class Output> static std::string Mode(const Output& output)
  {
    if (output.effective==8 && (output.flags & SDR)) return "SDR Rendering";
    if (!output.flags) return "Kodi default";
    if (IsHDR10PlusDV(output)) return Policy(output);
    std::string mode = output.flags & HDR10 ? Policy(output) :
        output.effective == 6 ? "Dolby Vision Adaptive HDR Mapping" :
        output.effective == 5 ? "Dolby Vision Master Reference" :
        output.effective == 4 ? "Dolby Vision Adaptive HDR Mapping" : "Dolby Vision Reference";
    if (output.fallback)
      mode += " / compatibility fallback";
    return mode;
  }

  static std::string Blocks(const std::bitset<256>& blocks)
  {
    std::string text;
    for(unsigned level=0;level<blocks.size();++level)
      if(blocks[level]) text+=(text.empty() ? "" : " ")+std::string("L")+std::to_string(level);
    return text;
  }

  static std::string Transport(uint32_t format)
  {
    switch(format)
    {
      case 0x34325258: case 0x34325241: return "RGB 4:4:4 / 8-bit";
      case 0x30335258: case 0x30335241:
      case 0x30334258: case 0x30334241: return "RGB 4:4:4 / 10-bit";
      case 0x48344241: return "RGB 4:4:4 / 16-bit float";
      default: return ""; // Unknown negotiated format is unavailable, never guessed.
    }
  }

  template<class Output> static std::string Detail(std::string source, const Output& output)
  {
    if (!output.flags) return source;
    if ((output.flags & FEL) && source.find("FEL") == std::string::npos) source += " FEL";
    source += output.flags & CM4 ? " · CM4 metadata" : " · CM2.9 metadata";
    source += " · " + Mode(output);
    return source;
  }
};
