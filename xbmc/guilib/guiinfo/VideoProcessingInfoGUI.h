/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "GUIInfoLabels.h"
#include "cores/VideoProcessingInfo.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "ServiceBroker.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"

struct VideoProcessingInfoGUI
{
  static std::string CMVersion(const VideoProcessingInfo& info)
  {
    if (!info.metadata) return "";
    if (info.cm4) return "CM4.0 (original)";
    if (info.output.lldv) return "CM2.9";
    if ((info.output.flags & CDVPlaybackInfo::HDMI) && !info.output.hdr10AI &&
        !info.output.authoredLegacy && (info.output.effective==1 || info.output.effective==5))
      return info.output.flags & CDVPlaybackInfo::CM4 ? "CM2.9 (CM4 added)" : "CM2.9 (CM4 not added)";
    return "CM2.9";
  }
  static std::string Mode(const VideoProcessingInfo& info, bool hdr10PlusSource = false)
  {
    if (!info.output.flags)
    {
      const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
      if (info.outputType == VideoOutputType::HDR10Plus)
        return strings.Get(41014);
      if (info.outputType == VideoOutputType::HDR10)
        return hdr10PlusSource ? strings.Get(40866) : strings.Get(41310);
      if (info.outputType == VideoOutputType::HLG)
        return strings.Get(41311);
      return "";
    }
    if (CDVPlaybackInfo::IsHDR10PlusDV(info.output)) return CDVPlaybackInfo::Mode(info.output);
    if (info.output.hdr10AI)
      return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(
          info.output.effective==1 ? 40867 : 40868);
    return CDVPlaybackInfo::Mode(info.output);
  }
  static bool GetLabel(uint32_t id, const VideoProcessingInfo& info, std::string& value)
  {
    value.clear();
    switch (id)
    {
      case PLAYER_PROCESS_VIDEOOUTPUTHDRTYPE:
        if (info.outputType)
          switch (*info.outputType)
          {
            case VideoOutputType::SDR: value="sdr"; break;
            case VideoOutputType::HDR10: value="hdr10"; break;
            case VideoOutputType::HDR10Plus: value="hdr10plus"; break;
            case VideoOutputType::HLG: value="hlg"; break;
            case VideoOutputType::DolbyVision: value="dolbyvision"; break;
          }
        break;
      case PLAYER_PROCESS_VIDEOPROCESSINGMODE: value=Mode(info); break;
      case PLAYER_PROCESS_VIDEOPROCESSINGENGINE: value=info.output.engine; break;
      case PLAYER_PROCESS_VIDEOPROCESSINGCIRCUIT: value=info.output.circuit; break;
      case PLAYER_PROCESS_VIDEOPROCESSINGFALLBACK:
        if (info.output.fallback)
          value=CDVPlaybackInfo::Reason(static_cast<DVBRIDGE::OutputReason>(info.output.reason));
        break;
      case PLAYER_PROCESS_VIDEODVCMVERSION:
        value=CMVersion(info);
        break;
      case PLAYER_PROCESS_VIDEODVPARSEDBLOCKS:
        if (info.metadata) value=CDVPlaybackInfo::Blocks(info.blocks);
        break;
      case PLAYER_PROCESS_VIDEODVAPPLIEDBLOCKS:
        if (info.metadata) value=CDVPlaybackInfo::Blocks(info.appliedBlocks);
        break;
      case PLAYER_PROCESS_VIDEODVTRANSMITTEDBLOCKS:
        value=CDVPlaybackInfo::Blocks(info.transmittedBlocks);
        break;
      case PLAYER_PROCESS_VIDEOSOURCEBITS:
        if (info.sourceBits) value=std::to_string(info.sourceBits)+"-bit";
        break;
      case PLAYER_PROCESS_VIDEODVPROFILE:
        if (info.profile) value=std::to_string(info.profile);
        break;
      case PLAYER_PROCESS_VIDEODVLEVEL:
        if (info.profile) value=std::to_string(info.level);
        break;
      case PLAYER_PROCESS_VIDEODVVERSION:
        if (info.profile) value=std::to_string(info.major)+"."+std::to_string(info.minor);
        break;
      case PLAYER_PROCESS_VIDEODVLAYERS:
        if (info.profile) value=info.fel ? "FEL" : info.el ? "MEL" : "BL";
        break;
      case PLAYER_PROCESS_VIDEOOUTPUTFORMAT:
        if (info.output.flags) value=CDVPlaybackInfo::Transport(info.output.format);
        break;
      case PLAYER_PROCESS_VIDEOOUTPUTBITS:
        if (info.output.bits) value=std::to_string(info.output.bits)+"-bit";
        break;
      default: return false;
    }
    return true;
  }
};
