/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialogPlayerProcessInfo.h"

#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#ifdef HAVE_DVBRIDGE
#include "DVPlaybackModes.h"
#include "DVPixelFormat.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/VideoRenderers/UpscalerSettings.h"
#include "cores/VideoSourceSideData.h"
#include "guilib/guiinfo/VideoProcessingInfoGUI.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/CPUInfo.h"
#include "utils/DVBridgeOverlayTrace.h"
#include "utils/MemUtils.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "guilib/GUIMessage.h"
#include "filesystem/File.h"
#include "guilib/GUIControlGroupList.h"
#include "guilib/GUIFont.h"
#include "guilib/GUITextLayout.h"
#include "guilib/GUIFontManager.h"
#include "utils/TimeUtils.h"
#include "windowing/WinSystem.h"
#include "windowing/GraphicContext.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>
#endif

CGUIDialogPlayerProcessInfo::CGUIDialogPlayerProcessInfo(void)
    : CGUIDialog(WINDOW_DIALOG_PLAYER_PROCESS_INFO, "DialogPlayerProcessInfo.xml")
{
  m_loadType = KEEP_IN_MEMORY;
}

CGUIDialogPlayerProcessInfo::~CGUIDialogPlayerProcessInfo(void) = default;

bool CGUIDialogPlayerProcessInfo::OnMessage(CGUIMessage& message)
{
#ifdef HAVE_DVBRIDGE
  if (message.GetMessage() == GUI_MSG_WINDOW_INIT)
  {
    const bool enhanced = DVBRIDGE::EnhancedInfoEnabled();
    const bool trace = DVBRIDGE::OverlayDebugEnabled();
    const auto begin = trace ? DVBRIDGE::OverlayClock::now() : DVBRIDGE::OverlayClock::time_point{};
    if (trace)
      DVBRIDGE::overlayTrace.Observe(enhanced ? DVBRIDGE::PLAYER_INFO_CB1 : DVBRIDGE::PLAYER_INFO_KODI);
    const std::string file = enhanced ?
        "special://xbmc/media/cb1/DialogPlayerProcessInfo.xml" :
        "DialogPlayerProcessInfo.xml";
    if (GetProperty("xmlfile").asString() != file)
    {
      FreeResources(true);
      SetProperty("xmlfile", file);
    }
    if (enhanced)
    {
      const RESOLUTION_INFO resolution(1920, 1080);
      SetCoordsRes(resolution);
      // Noto Sans ships with Estuary, which every Kodi installs; DejaVu Sans is the fallback.
      const std::string noto = "special://xbmc/addons/skin.estuary/fonts/NotoSans-Regular.ttf";
      const std::string face = XFILE::CFile::Exists(noto) ? noto : "special://xbmc/media/Fonts/arial.ttf";
      g_fontManager.LoadTTF("CB1.InfoCaption", face, 0xfff4f6f6, 0, 16,
                            FONT_STYLE_BOLD | FONT_STYLE_UPPERCASE, false, 1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoBody", face, 0xfff4f6f6, 0, 20, FONT_STYLE_NORMAL, false,
                            1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoLead", face, 0xfff4f6f6, 0, 22, FONT_STYLE_NORMAL, false,
                            1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoMetric", face, 0xfff4f6f6, 0, 26, FONT_STYLE_BOLD, false,
                            1.0f, 1.0f, &resolution);
      g_fontManager.LoadTTF("CB1.InfoHero", face, 0xfff4f6f6, 0, 32, FONT_STYLE_BOLD, false,
                            1.0f, 1.0f, &resolution);
    }
    if (trace)
    {
      const auto resources = DVBRIDGE::OverlayElapsedUs(begin);
      const bool initialized = CGUIDialog::OnMessage(message);
      CLog::Log(LOGDEBUG, "CB1 Player Info open: kind={} opened_ms={} resource_setup_us={} init_total_us={}",
                DVBRIDGE::overlayTrace.kind, DVBRIDGE::overlayTrace.openedMs, resources,
                DVBRIDGE::OverlayElapsedUs(begin));
      return initialized;
    }
  }
#endif
  return CGUIDialog::OnMessage(message);
}

void CGUIDialogPlayerProcessInfo::OnInitWindow()
{
#ifdef HAVE_DVBRIDGE
  const bool enhanced = DVBRIDGE::EnhancedInfoEnabled();
  SetProperty("DV.Enhanced", enhanced);
  m_nextRefresh = 0;
  m_renderUsage.Reset();
  SetProperty("DV.GPUAvailable", false);
  SetProperty("DV.GPURender", "N/A");
  if (enhanced)
  {
    RefreshInfo();
    m_nextRefresh = CTimeUtils::GetFrameTime() + (m_renderUsage.Available() ? 200 : 500);
  }
#endif
  CGUIDialog::OnInitWindow();
#ifdef HAVE_DVBRIDGE
  if (enhanced)
    UpdatePanelHeight();
#endif
}

void CGUIDialogPlayerProcessInfo::OnDeinitWindow(int nextWindowID)
{
#ifdef HAVE_DVBRIDGE
  if (DVBRIDGE::IsPlayerInfoTrace() && DVBRIDGE::OverlayDebugEnabled())
    CLog::Log(LOGDEBUG, "CB1 Player Info close: kind={} opened_ms={} now_ms={}",
              DVBRIDGE::overlayTrace.kind, DVBRIDGE::overlayTrace.openedMs, DVBRIDGE::DiagnosticNowMs());
  m_renderUsage.Reset();
#endif
  CGUIDialog::OnDeinitWindow(nextWindowID);
}

#ifdef HAVE_DVBRIDGE
namespace
{
// RPU primaries indexes of L9 and L10, as dovi_tool and MediaInfo decode them.
std::string RPUPrimaries(unsigned index)
{
  static constexpr const char* names[] = {"P3-D65", "BT.709", "BT.2020", "BT.601 NTSC",
      "BT.601 PAL", "DCI-P3", "ACES", "S-Gamut", "S-Gamut3.Cine"};
  return index < std::size(names) ? names[index] : "Custom";
}

// Preset target displays named by L8 trims, the table CB1 resolves (cm4_preset).
long PresetTargetNits(unsigned index)
{
  switch (index)
  {
    case 1: return 100;
    case 16: case 18: case 21: return 48;
    case 27: case 28: return 600;
    case 37: case 38: return 2000;
    case 42: return 108;
    case 48: case 49: return 1000;
    default: return 0;
  }
}

std::string DVContentType(unsigned type)
{
  static constexpr const char* names[] = {"Default", "Cinema", "Game", "Sport", "User-generated"};
  return type < std::size(names) ? names[type] : StringUtils::Format("Type {}", type);
}

// Use the real panel font and its original 284-pixel value column, not a byte limit.
bool InfoValueFits(const std::string& text)
{
  auto* font = g_fontManager.GetFont("CB1.InfoBody", false);
  if (!font)
    return text.size() <= 28;
  CGUITextLayout label(font, false);
  label.Update(text);
  return label.GetTextWidth() <= 284.0f;
}

std::string LldvTrimText(unsigned levels, unsigned groups)
{
  using Trims = VideoProcessingInfo::PlayerLed;
  if (!groups)
    return {};
  std::string text = levels & (1u << 8) ? "L8" : "L2";
  if (groups & Trims::SOP) text += " · SOP";
  if (groups & Trims::Colour) text += " · Colour";
  if (groups & Trims::Mid) text += " · Mid";
  if (groups & Trims::Clip) text += " · Clip";
  return text;
}

std::string PolicyText(std::string text)
{
  if (text.starts_with("Dolby Vision "))
    text.replace(0, 13, "DV ");
  if (const auto at = text.find("Adaptive HDR Mapping"); at != std::string::npos)
    text.replace(at, 20, "Adaptive");
  return text;
}

std::string TargetList(std::vector<long> nits, const std::vector<unsigned>& unknown)
{
  std::sort(nits.begin(), nits.end());
  nits.erase(std::unique(nits.begin(), nits.end()), nits.end());
  std::string text;
  for (const long value : nits)
    text += (text.empty() ? "" : " · ") + std::to_string(value);
  if (!text.empty())
    text += " nits";
  for (const unsigned index : unknown)
    text += (text.empty() ? "#" : " · #") + std::to_string(index);
  if (text.empty() || InfoValueFits(text))
    return text;
  // State a count with the bounds; this does not invent intermediate authored targets.
  if (nits.empty())
    return StringUtils::Format("{} custom · #{}–#{}", unknown.size(),
        *std::min_element(unknown.begin(), unknown.end()), *std::max_element(unknown.begin(), unknown.end()));
  text = nits.size() == 1 ? StringUtils::Format("1 target · {} nits", nits.front()) :
      StringUtils::Format("{} targets · {}–{} nits", nits.size(), nits.front(), nits.back());
  if (!unknown.empty())
    text = nits.size() == 1 ? StringUtils::Format("1: {} nits · {} custom", nits.front(), unknown.size()) :
        StringUtils::Format("{}: {}–{} nits · {} custom", nits.size(), nits.front(), nits.back(), unknown.size());
  return text;
}

double PQNits(unsigned pq)
{
  const double p = std::pow(pq / 4095.0, 32.0 / 2523.0);
  return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
      (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
}

// A 12-bit PQ code is about 0.23 % of its luminance wide, so an authored target comes back
// slightly off (1000 nits is code 3079, 1000.6 nits); three significant digits name it.
long TargetNits(unsigned pq)
{
  const double nits = PQNits(pq);
  if (nits < 100)
    return std::lround(nits);
  const double step = std::pow(10.0, std::floor(std::log10(nits)) - 2);
  return std::lround(std::round(nits / step) * step);
}

std::string L2Targets(const VideoDVLevels& levels)
{
  std::vector<long> nits;
  for (unsigned i = 0; i < levels.l2Count; ++i)
    nits.push_back(TargetNits(levels.l2[i]));
  return TargetList(nits, {});
}

// As CB1 resolves an L8 target: a preset first, else the L10 block of that index.
std::string L8Targets(const VideoDVLevels& levels)
{
  std::vector<long> nits;
  std::vector<unsigned> unknown;
  for (unsigned i = 0; i < levels.l8Count; ++i)
  {
    long target = PresetTargetNits(levels.l8[i]);
    for (unsigned j = 0; !target && j < levels.l10Count; ++j)
      if (levels.l10[j].index == levels.l8[i])
        target = TargetNits(levels.l10[j].maxPQ);
    if (target)
      nits.push_back(target);
    else
      unknown.push_back(levels.l8[i]);
  }
  return TargetList(nits, unknown);
}

std::string L10Displays(const VideoDVLevels& levels)
{
  std::string text;
  long minimum = 0, maximum = 0;
  bool mixed = false;
  for (unsigned i = 0; i < levels.l10Count; ++i)
  {
    const long target = TargetNits(levels.l10[i].maxPQ);
    minimum = i ? std::min(minimum, target) : target;
    maximum = i ? std::max(maximum, target) : target;
    mixed = mixed || levels.l10[i].primaries != levels.l10[0].primaries;
    text += StringUtils::Format("{}{} nits {}", text.empty() ? "" : " · ",
        target, RPUPrimaries(levels.l10[i].primaries));
  }
  if (text.empty() || InfoValueFits(text))
    return text;
  return StringUtils::Format("{} displays · {}–{} nits · {}", levels.l10Count, minimum, maximum,
      mixed ? std::string("mixed") : RPUPrimaries(levels.l10[0].primaries));
}

// White points step by 375 K from D65; index 8 is D93.
std::string L11Content(const VideoDVLevels& levels)
{
  const unsigned white = levels.whitepoint;
  return StringUtils::Format("{} · {}{}", DVContentType(levels.contentType),
      white == 0 ? std::string("D65") : white == 8 ? std::string("D93") :
          StringUtils::Format("{} K", 6504 + 375 * white),
      levels.referenceMode ? " · reference mode" : "");
}

} // namespace

void CGUIDialogPlayerProcessInfo::RefreshInfo()
{
    const DVBRIDGE::OverlayTimer timing(DVBRIDGE::OverlayStage::Refresh);
    const auto render = m_renderUsage.Sample();
    const bool gpuAvailable = render.has_value() || m_renderUsage.Available();
    SetProperty("DV.GPUAvailable", gpuAvailable);
    SetProperty("DV.GPURender", render ? StringUtils::Format("{:.0f} %", *render) :
        gpuAvailable ? "--" : "N/A");
    SetProperty("DV.GPURenderPercent", render ? StringUtils::Format("{:.1f}", *render) : "0");
    for (const int gauge : {9400, 9401})
    {
      CGUIMessage usage(GUI_MSG_ITEM_SELECT, GetID(), gauge, render ? std::lround(*render) : 0);
      OnMessage(usage);
    }
    const auto data = CServiceBroker::GetDataCacheCore().GetVideoProcessingInfo();
    SetProperty("DV.SourcePixelFormat", DVBRIDGE::SourcePixelFormat(static_cast<AVPixelFormat>(data.sourceFormat)));
    SetProperty("DV.SourceBits", data.sourceBits ? StringUtils::Format("{}-bit", data.sourceBits) : "N/A");
    SetProperty("DV.DecodedHLG", !data.profile && data.tags.transfer == AVCOL_TRC_ARIB_STD_B67);
    // Profiles 5 and 10.0 carry IPT-PQc2, which their colour tags leave unspecified and Kodi
    // would otherwise present as an assumed BT.709.
    SetProperty("DV.SourceColour", data.profile == 5 || (data.profile == 10 && !data.compatibility) ?
        std::string("IPT-PQc2") : DVBRIDGE::SourceColour(data.tags.primaries, data.tags.transfer,
        data.tags.primariesGuessed, data.tags.fullRange));
    const auto active = data.output.flags;
    const bool hdr10 = active & CDVPlaybackInfo::HDR10;
    const bool lldv = active && data.output.lldv.has_value();
    const bool metadata = active && data.metadata;
    const auto yesno = [](bool value) { return value ? "Yes" : "No"; };
    const auto* winSystem = CServiceBroker::GetWinSystem();
    const auto resolution = winSystem->GetGfxContext().GetResInfo();
    const char* scanType = (resolution.dwFlags & D3DPRESENTFLAG_INTERLACED) ? "i" : "p";
    SetProperty("DV.Display", StringUtils::Format("{}x{}{} / {:.3f} Hz",
        resolution.iScreenWidth, resolution.iScreenHeight, scanType, resolution.fRefreshRate));
    if (active && data.output.width && data.output.height && data.output.refresh>0)
      SetProperty("DV.Display", StringUtils::Format("{}x{}p / {:.3f} Hz",
          data.output.width,data.output.height,data.output.refresh));
    const auto wire = winSystem->GetWireOutputTelemetry();
    const auto cpu = CServiceBroker::GetCPUInfo();
    SetProperty("DV.CPU", cpu ? StringUtils::Format("{} %", cpu->GetUsedPercentage()) : "N/A");
    // Sampled here: System.Memory parses /proc/meminfo on every frame it is displayed.
    KODI::MEMORY::MemoryStatus memory{};
    KODI::MEMORY::GetMemoryStatus(&memory);
    const int memoryUsed = memory.totalPhys ? static_cast<int>(
        100.0 * (memory.totalPhys - memory.availPhys) / memory.totalPhys + 0.5) : 0;
    SetProperty("DV.Memory", memory.totalPhys ? StringUtils::Format("{} %", memoryUsed) : "N/A");
    for (const int bar : {9402, 9403})
    {
      CGUIMessage usage(GUI_MSG_ITEM_SELECT, GetID(), bar, memoryUsed);
      OnMessage(usage);
    }
    SetProperty("DV.DroppedSkipped", data.counters ?
        StringUtils::Format("{} / {}", data.dropped, data.skipped) : "N/A");
    SetProperty("DV.SourceMastering", data.sourceMastering ?
        StringUtils::Format("{:.0f} / {:.4f} nits", data.sourceMax, data.sourceMin) : "N/A");
    SetProperty("DV.SourceLight", data.sourceLight ?
        StringUtils::Format("{} / {} nits", data.sourceCLL, data.sourceFALL) : "N/A");
    SetProperty("DV.Profile", data.profile ? StringUtils::Format("{}", data.profile) : "N/A");
    // Profile 8 is named with its base-layer compatibility: 8.1 HDR10, 8.2 SDR, 8.4 HLG.
    SetProperty("DV.ProfileDetail", (data.profile == 8 || data.profile == 10) ? StringUtils::Format("{}.{}", data.profile, data.compatibility) :
        data.profile ? StringUtils::Format("{}", data.profile) : "N/A");
    // Kodi's decoder tells FEL from MEL by the RPU's NLQ parameters (strDVELType).
    const std::string base = data.compatibility == 1 || data.compatibility == 6 ? "HDR10 BL" :
        data.compatibility == 2 ? "SDR BL" : data.compatibility == 4 ? "HLG BL" : "BL";
    SetProperty("DV.Layers", data.profile && data.bl && data.rpu ? base + (data.el ? " + " +
        (data.tags.enhancement.empty() ? std::string("EL") : data.tags.enhancement) : std::string()) +
        " + RPU" : "");
    SetProperty("DV.Level", data.profile ? StringUtils::Format("{}", data.level) : "N/A");
    SetProperty("DV.Version", data.profile ? StringUtils::Format("{}.{}", data.major, data.minor) : "N/A");
    SetProperty("DV.CM", metadata ? VideoProcessingInfoGUI::CMVersion(data) : "");
    const auto applied=data.AppliedBlocks();
    SetProperty("DV.ParsedBlocks", metadata ? CDVPlaybackInfo::Blocks(data.blocks) : "");
    SetProperty("DV.AppliedBlocks", metadata ? CDVPlaybackInfo::Blocks(applied | (active & CDVPlaybackInfo::HDR10PLUS ? data.blocks & std::bitset<256>(data.output.creativeEdited) : std::bitset<256>{})) : "");
    SetProperty("DV.TransmittedBlocks", active & CDVPlaybackInfo::HDMI ? CDVPlaybackInfo::Blocks(data.TransmittedBlocks())+
        (data.output.authoredLegacy ? " (authored)" : "") : "");
    SetProperty("DV.UnappliedBlocks", metadata ? CDVPlaybackInfo::Blocks(data.blocks & ~(applied | data.TransmittedBlocks())) : "");
    SetProperty("DV.RPU", data.profile ? yesno(data.rpu) : "N/A");
    SetProperty("DV.BL", data.profile ? yesno(data.bl) : "N/A");
    SetProperty("DV.EL", data.profile ? yesno(data.el) : "N/A");
    SetProperty("DV.FEL", active ? (active & CDVPlaybackInfo::FEL ? "Full FEL" : "No FEL") : "");
    SetProperty("DV.FELUpscaling", active & CDVPlaybackInfo::FEL ?
        (data.output.felUpscaling == 2 ? "Reference GPU" : data.output.felUpscaling == 0 ? "VAAPI" : data.output.felUpscaling == 1 ? "Bilinear GPU" : "") : "");
    const auto actualMode = VideoProcessingInfoGUI::Mode(data,
        CServiceBroker::GetDataCacheCore().HasVideoHDR10PlusSource());
    SetProperty("DV.Mode", actualMode.empty() ? "Kodi : Default" : actualMode);
    const int upscaler = CServiceBroker::GetDataCacheCore().GetVideoUpscaler();
    const int upscalerLabel = upscaler < 0 ? 0 : KODI::VIDEO::UPSCALER::Label(upscaler);
    SetProperty("DV.VideoUpscaler", upscalerLabel ?
        CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(upscalerLabel) : "");
    SetProperty("DV.Engine", data.output.engine);
    SetProperty("DV.Circuit", data.output.circuit);
    SetProperty("DV.Reconstruction", "");
    SetProperty("DV.AIModel", "");
    SetProperty("DV.BeforeAI", "");
    SetProperty("DV.AIL1", "");
    SetProperty("DV.AIL3", "");
    SetProperty("DV.OutputL1", "");
    SetProperty("DV.PartialBlocks", hdr10 && data.output.effective==3 && metadata ?
        std::string(data.blocks[1] ? "L1 max/avg" : "")+
        (data.blocks[3] ? (data.blocks[1] ? "; L3 max/avg" : "L3 max/avg") : "") : "");
    SetProperty("DV.ActivePolicy", actualMode.empty() ? "Kodi : Default" : actualMode);
    if (active && data.output.sdrExpanded && (data.output.flags & CDVPlaybackInfo::HDMI))
    {
      SetProperty("DV.Reconstruction", "Measured L1 / neutral L3");
      const auto& raw=data.output.aiL1L3;
      SetProperty("DV.AIL1", StringUtils::Format("{} | {} | {}",raw[0],raw[1],raw[2]));
      SetProperty("DV.AIL3", StringUtils::Format("{} | {} | {}",raw[3],raw[4],raw[5]));
      if (data.output.emittedL1Known)
        SetProperty("DV.OutputL1", StringUtils::Format("{} | {} | {}",
            data.output.emittedL1[0],data.output.emittedL1[1],data.output.emittedL1[2]));
    }
    if (active && data.output.hdr10AI)
    {
      const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
      const int label=data.output.hdr10PlusDV ? 40970 : data.output.effective==1 ? 40867 : 40868;
      SetProperty("DV.Mode", localizer.Get(label));
      SetProperty("DV.ActivePolicy", localizer.Get(label));
      const bool ai=!data.output.aiModel.empty();
      SetProperty("DV.Reconstruction", "AI-generated L1/L3");
      if (ai)
      {
        SetProperty("DV.AIModel", data.output.aiModel);
        const auto& base=data.output.aiBaseline;
        SetProperty("DV.BeforeAI", StringUtils::Format("{} | {} | {}",base[0],base[1],base[2]));
      }
      if (data.output.hdr10PlusDV)
        SetProperty("DV.Reconstruction", std::string(data.output.guidedSourceFields & 4u ?
            "HDR10+ max/avg · min est." : data.output.guidedSourceFields & 2u ?
            (ai ? "HDR10+ max · min/avg AI" : "HDR10+ max · min/avg est.") :
            (ai ? "Min/max/avg: AI" : "Min/max/avg: estimated")));
      SetProperty("DV.Profile", "");
      SetProperty("DV.ProfileDetail", "");
      SetProperty("DV.Layers", "");
      SetProperty("DV.Level", "");
      SetProperty("DV.Version", "");
      const auto& raw=data.output.aiL1L3;
      SetProperty("DV.AIL1", StringUtils::Format("{} | {} | {}",raw[0],raw[1],raw[2]));
      SetProperty("DV.AIL3", StringUtils::Format("{} | {} | {}",raw[3],raw[4],raw[5]));
      if (data.output.emittedL1Known)
        SetProperty("DV.OutputL1", StringUtils::Format("{} | {} | {}",
            data.output.emittedL1[0],data.output.emittedL1[1],data.output.emittedL1[2]));
    }
    const auto& localizer=CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
    const auto reasonText=[&](VideoProcessingReason reason) {
      const int id=CDVPlaybackInfo::ReasonLabel(static_cast<DVBRIDGE::OutputReason>(reason));
      return id ? localizer.Get(id) : std::string{};
    };
    SetProperty("DV.Fallback", active && data.output.fallback ? reasonText(data.output.reason) :
        data.nativeHDR10PlusFallbackLabel ? localizer.Get(data.nativeHDR10PlusFallbackLabel) : "");
    SetProperty("DV.MinimumLinkBits", active && data.output.minimumLinkBits ?
        StringUtils::Format("{}-bit",data.output.minimumLinkBits) : "");
    SetProperty("DV.Transport", active ? CDVPlaybackInfo::Transport(data.output.format) :
        (wire && wire->scanoutFourcc) ? CDVPlaybackInfo::Transport(wire->scanoutFourcc) : "");
    using LldvEncoding = VideoProcessingInfo::PlayerLed::Encoding;
    const std::string wireHdmiFormat = (wire && !wire->format.empty()) ?
        (wire->bpc > 0 ? StringUtils::Format("{} / {}-bit", wire->format, wire->bpc) : wire->format) :
        std::string{};
    SetProperty("DV.HdmiFormat", lldv ?
        (data.output.lldv->encoding == LldvEncoding::RGB ? "RGB" :
         data.output.lldv->encoding == LldvEncoding::YCbCr444 ? "YCbCr 4:4:4" :
         data.output.lldv->forcedTransport ? localizer.Get(41339) : "YCbCr 4:2:2") : wireHdmiFormat);
    SetProperty("DV.SignalIntent", lldv ? "LLDV" : active & CDVPlaybackInfo::HDMI ?
        (data.output.iptTransport ? "TV-Led / IPT-PQc2 tunnel" : "TV-Led / YCbCr tunnel") :
        active & CDVPlaybackInfo::HDR10PLUS ? "PQ / BT.2020 / HDR10+" :
        hdr10 ? "PQ / BT.2020" : "");
    const char* sdrGamut = "";
    if (data.outputType == VideoOutputType::SDR)
    {
      if (data.output.gamut == DVBRIDGE_GAMUT_BT709)
        sdrGamut = "BT.709";
      else if (data.output.gamut == DVBRIDGE_GAMUT_BT2020)
        sdrGamut = "BT.2020";
    }
    SetProperty("DV.SDROutputGamut", sdrGamut);
    SetProperty("DV.Committed", active && data.output.stream ? StringUtils::Format("{} / {} / {}",
        data.output.stream,data.output.picture,data.output.revision) : "");
    // Next-picture preparation is not an output-mode transition.
    const bool transaction=data.transaction.identity.Valid() &&
        std::string_view(data.transaction.stage)!="Committed" &&
        (data.transaction.atomicError ||
         data.transaction.reason==VideoProcessingReason::ActivationFailed ||
         data.transaction.reason==VideoProcessingReason::RestoreFailed ||
         std::string_view(data.transaction.stage)=="Failed commit" ||
         (active ? (
          data.transaction.identity.stream!=data.output.stream ||
          data.transaction.requested!=data.output.requested ||
          data.transaction.effective!=data.output.effective ||
          data.transaction.reason!=data.output.reason) :
          (data.transaction.requested || data.transaction.reason!=VideoProcessingReason::None)));
    SetProperty("DV.ProcessingVisible", bool(active) || transaction || data.nativeHDR10PlusFallbackLabel);
    SetProperty("DV.RequestedPolicy", transaction ? PolicyText(CDVPlaybackInfo::RequestedPolicy(data.transaction.requested,
        data.transaction.hdr10AI || data.transaction.sdrExpanded)) :
        active ? PolicyText(data.output.sdrExpanded ? CDVPlaybackInfo::Policy(data.output) :
            CDVPlaybackInfo::RequestedPolicy(data.output.requested,data.output.hdr10AI)) : "");
    SetProperty("DV.AdmissionReason", transaction ? reasonText(data.transaction.reason) : "");
    SetProperty("DV.AtomicResult", transaction && data.transaction.atomicError ?
        StringUtils::Format("{} / {} / {}",data.transaction.atomicError,
            data.transaction.retryCount,yesno(data.transaction.restored)) : "");
    SetProperty("DV.OutputStage", transaction ?
        std::string(data.transaction.stage) : "");
    SetProperty("DV.ReferenceWarning", data.referenceWarning ? "Consider Reference" : "");
    SetProperty("DV.TVProfile", lldv ? StringUtils::Format("EDID v{} / {:.0f} nits",
        data.output.lldv->edidVersion, data.output.target) : active && data.output.effective==8 ?
        StringUtils::Format("{} / {:g} nits / {:g}",
            data.output.gamut == 3 ? "BT.2020" : "BT.709", data.output.target, data.output.sdrGamma) :
        active && (data.output.effective==3 || data.output.effective==4 || data.output.effective==6) ?
        StringUtils::Format("{}{} / {:g} nits",
            data.output.panel==1 || data.output.panel==2 ? localizer.Get(40833+data.output.panel)+" · " : "",
            data.output.gamut==1 ? "BT.709" : data.output.gamut==2 ? "P3-D65" : "BT.2020",
            data.output.effective==3 ? static_cast<double>(data.output.target) : data.output.manualPeak) : "");
    SetProperty("DV.DisplayBlack", (active && (data.output.lldv || (hdr10 && data.output.effective==3) ||
        data.output.effective==8)) ?
        StringUtils::Format("{:g} nits", data.output.displayBlack) : "");
    for (unsigned i = 0; i < 3; ++i)
    {
      constexpr const char* keys[] = {"DV.LldvRed", "DV.LldvGreen", "DV.LldvBlue"};
      std::string value;
      if (lldv)
      {
        const auto& display = *data.output.lldv;
        const auto j = i * 2;
        value = StringUtils::Format("{:.4f}, {:.4f}", display.primaries[j], display.primaries[j + 1]);
      }
      SetProperty(keys[i], value);
    }
    SetProperty("DV.HDR10Policy", hdr10 ? CDVPlaybackInfo::Policy(data.output) : "");
    SetProperty("DV.HDR10Target", hdr10 && data.output.effective!=3 ? StringUtils::Format("{:g} nits", data.output.target) : "");
    SetProperty("DV.HDR10TargetLabel", hdr10 && data.output.effective!=3 ? (data.output.sdrExpanded ? "Expansion target" : data.output.effective==2 ? "Source reference" :
        data.output.effective==7 ? "Curve reference" : "Mapping target") : "");
    // Basic takes its reference from L6 max_luminance, else the RPU's source_max_pq (dvbridge_hdr10_target).
    SetProperty("DV.HDR10TargetOrigin", hdr10 && data.output.effective==2 && !data.output.sdrExpanded ?
        (data.output.level6 ? "L6 max" : "RPU source max") : "");
    SetProperty("DV.CreativeTrims", active && data.output.effective != 8 ? (data.output.creativeKnown ?
        (data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Invalid) ||
         data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Unsupported) ?
            "Treatment unavailable" :
         data.output.creativeStatus==static_cast<int>(VideoCreativeStatus::Neutral) ?
            "Neutral" :
         hdr10 ? (data.output.creativeApplied & ((1u<<2)|(1u<<8)) ?
            "Applied locally" : "No compatible trims") :
          data.output.creativeApplied ? "Metadata-guided expansion" :
          data.output.creativeEdited ? "Metadata adjusted for TV" : "Original metadata") :
        data.output.authoredLegacy || hdr10 ? "Not applied" : "Forwarded to TV") : "");
    if (active && data.output.effective==7)
      SetProperty("DV.CreativeTrims", !(active & CDVPlaybackInfo::HDR10PLUS) ? "HDR10 fallback" :
          data.output.creativeEdited & (1u<<8) ? "HDR10+ curve from L8" :
          data.output.creativeEdited & (1u<<2) ? "HDR10+ curve from L2" :
          data.output.creativeEdited & (1u<<1) ? "HDR10+ scene curve" : "Statistics only");
    if (lldv)
      SetProperty("DV.CreativeTrims", LldvTrimText(data.output.creativeApplied, data.output.lldv->trimGroups));
    SetProperty("DV.LocalCM4", lldv && data.output.lldv->cm4Appended ? "CM4 added locally" : "");
    SetProperty("DV.Precision", active ? StringUtils::Format("{}-bit", data.output.bits) : "N/A");
    SetProperty("DV.L1PQ", metadata && data.l1 ? StringUtils::Format("{} | {} | {}", data.pq[0], data.pq[1], data.pq[2]) : "N/A");
    SetProperty("DV.L1Nits", metadata && data.l1 ? StringUtils::Format("{:.1f} | {:.1f} | {:.1f} nits",
        PQNits(data.pq[0]), PQNits(data.pq[1]), PQNits(data.pq[2])) : "N/A");
    // Use the accepted route's partial components, without changing the original source values.
    const bool partialL1 = metadata && data.l1 && applied[1] &&
        (data.output.lldv || data.output.effective == 8 ||
         (hdr10 && (data.output.effective == 2 || data.output.effective == 3)));
    const auto sceneMax = data.output.lldv ? data.output.lldv->scenePQ[0] : data.pq[1];
    const auto sceneAvg = data.output.lldv ? data.output.lldv->scenePQ[1] : data.pq[2];
    SetProperty("DV.UsedL1PQ", partialL1 ? StringUtils::Format("{} | {}", sceneMax, sceneAvg) : "");
    std::string usedL1Nits = metadata && data.l1 && applied[1] ?
        (partialL1 ? StringUtils::Format("{:.1f} | {:.1f} nits", PQNits(sceneMax), PQNits(sceneAvg)) :
            StringUtils::Format("{:.1f} | {:.1f} | {:.1f} nits",
                PQNits(data.pq[0]), PQNits(data.pq[1]), PQNits(data.pq[2]))) : "";
    if (!InfoValueFits(usedL1Nits) && usedL1Nits.ends_with(" nits"))
      usedL1Nits.resize(usedL1Nits.size() - 5); // The row caption already states nits.
    SetProperty("DV.UsedL1Nits", usedL1Nits);
    SetProperty("DV.L5", metadata && data.l5 ? StringUtils::Format("{} | {} | {} | {}",
        data.area[0], data.area[1], data.area[2], data.area[3]) : "N/A");
    SetProperty("DV.Mastering", metadata && data.l6 ? StringUtils::Format("{} | {:.4f} nits",
        data.maxMaster, data.minMaster / 10000.0) : "N/A");
    SetProperty("DV.UsedL6Max", metadata && hdr10 && applied[6] &&
        (data.output.effective == 2 || data.output.effective == 3) ?
        StringUtils::Format("{} nits", data.maxMaster) : "");
    SetProperty("DV.CLL", metadata && data.l6 ? StringUtils::Format("{} | {} nits", data.maxCLL, data.maxFALL) : "N/A");
    // The other levels' values; the parsed chips say which levels the file carries.
    const auto& levels = data.levels;
    const auto carried = [&](unsigned level) { return metadata && data.blocks[level]; };
    SetProperty("DV.L2Targets", carried(2) ? L2Targets(levels) : "");
    // L2/L8 are forwarded unchanged when present in the accepted transmitted mask.
    // Synthesized/legacy output masks exclude these authored trims.
    const auto forwarded = data.TransmittedBlocks();
    SetProperty("DV.OutputL2Targets", active && forwarded[2] ? L2Targets(levels) : "");
    SetProperty("DV.OutputL8Targets", active && forwarded[8] ? L8Targets(levels) : "");
    SetProperty("DV.L3", carried(3) ? StringUtils::Format("{} | {} | {}",
        levels.l3[0], levels.l3[1], levels.l3[2]) : "");
    SetProperty("DV.UsedL3", carried(3) && applied[3] &&
        (data.output.lldv || (hdr10 && data.output.effective == 3)) ?
        StringUtils::Format("{} | {}", levels.l3[1], levels.l3[2]) : "");
    SetProperty("DV.L4", carried(4) ? StringUtils::Format("{} | {}", levels.l4[0], levels.l4[1]) : "");
    SetProperty("DV.L8Targets", carried(8) ? L8Targets(levels) : "");
    SetProperty("DV.L9", carried(9) ? RPUPrimaries(levels.l9) : "");
    SetProperty("DV.L10", carried(10) ? L10Displays(levels) : "");
    SetProperty("DV.L11", carried(11) ? L11Content(levels) : "");
    SetProperty("DV.L254", carried(254) ? StringUtils::Format("{} / {}", levels.dmMode, levels.dmVersion) : "");
    SetProperty("DV.L255", carried(255) ? StringUtils::Format("{} / {}", levels.runMode, levels.runVersion) : "");
    // Input always describes the original accepted picture, including unused HDR10+.
    const auto side = CServiceBroker::GetDataCacheCore().GetVideoSourceSideDataSnapshot();
    const AVDynamicHDRPlus* plus = side ? side->HDR10Plus() : nullptr;
    const auto scaled = [](AVRational value) { return 10000.0 * av_q2d(value); };
    const bool plainHDRSDR = active && data.output.effective == 8 && !data.profile;
    // These bits and values belong to the accepted import; pointer presence is not proof of use.
    const unsigned imported = plainHDRSDR ? data.output.sdrHDRInputUsed : 0u;
    const unsigned plusUsed = plainHDRSDR ? ((imported & 8u ? 2u : 0u) | (imported & 16u ? 4u : 0u)) :
        active && data.output.hdr10PlusDV ? data.output.guidedSourceFields : 0u;
    SetProperty("DV.UsedHDRStatic", bool(imported & 7u));
    SetProperty("DV.UsedHDRMin", imported & 1u ? StringUtils::Format("{:.4f} nits", data.sourceMin) : "");
    SetProperty("DV.UsedHDRMax", imported & 2u ? StringUtils::Format("{:.0f} nits", data.sourceMax) : "");
    SetProperty("DV.UsedHDRCLL", imported & 4u ? StringUtils::Format("{} nits", data.sourceCLL) : "");
    SetProperty("DV.UsedHDR10PlusMax", bool(plusUsed & 2u));
    SetProperty("DV.UsedHDR10PlusAvg", bool(plusUsed & 4u));
    const auto& importedValues = data.output.sdrHDRInputValues;
    SetProperty("DV.UsedHDR10PlusMaxRGB", imported & 8u ? StringUtils::Format("{:.0f} | {:.0f} | {:.0f} nits",
        importedValues[0], importedValues[1], importedValues[2]) : "");
    SetProperty("DV.UsedHDR10PlusAverage", imported & 16u ? StringUtils::Format("{:.1f} nits", importedValues[3]) : "");
    const double display = plus ? av_q2d(plus->targeted_system_display_maximum_luminance) : 0.0;
    SetProperty("DV.HDR10PlusCarried", plus ? StringUtils::Format("HDR10+ v{} / {} windows",
        plus->application_version, plus->num_windows) :
        side && (side->RejectedSections() & 4u) ? std::string("HDR10+ · unavailable") : "");
    SetProperty("DV.HDR10PlusSection", plus ? (plus->num_windows > 1 ?
        StringUtils::Format("HDR10+ · window 1/{}", plus->num_windows) : std::string("HDR10+ metadata")) : "");
    SetProperty("DV.HDR10PlusTarget", plus ? StringUtils::Format("{} · version {}",
        display > 0 ? StringUtils::Format("{:.0f} nits", display) : std::string("Unset"),
        plus->application_version) : "");
    SetProperty("DV.HDR10PlusMaxSCL", plus ? StringUtils::Format("{:.0f} | {:.0f} | {:.0f} nits",
        scaled(plus->params[0].maxscl[0]), scaled(plus->params[0].maxscl[1]),
        scaled(plus->params[0].maxscl[2])) : "");
    SetProperty("DV.HDR10PlusAverage", plus ?
        StringUtils::Format("{:.1f} nits", scaled(plus->params[0].average_maxrgb)) : "");
    SetProperty("DV.HDR10PlusCurve", !plus ? "" : plus->params[0].tone_mapping_flag ?
        StringUtils::Format("{} anchors · knee {:.3f} / {:.3f}", plus->params[0].num_bezier_curve_anchors,
            av_q2d(plus->params[0].knee_point_x), av_q2d(plus->params[0].knee_point_y)) :
        std::string("None"));
    SetProperty("DV.HDR10PlusSaturation", !plus ? "" : plus->params[0].color_saturation_mapping_flag ?
        StringUtils::Format("{:.3f}×", av_q2d(plus->params[0].color_saturation_weight)) : std::string("None"));
}
#endif

void CGUIDialogPlayerProcessInfo::Process(unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
#ifdef HAVE_DVBRIDGE
  const bool refresh = DVBRIDGE::EnhancedInfoEnabled() && currentTime >= m_nextRefresh;
  if (refresh)
  {
    m_nextRefresh = currentTime + 500;
    RefreshInfo();
  }
#endif
  CGUIDialog::Process(currentTime, dirtyregions);
#ifdef HAVE_DVBRIDGE
  if (refresh)
    UpdatePanelHeight();
#endif
}

#ifdef HAVE_DVBRIDGE
void CGUIDialogPlayerProcessInfo::UpdatePanelHeight()
{
  // Initial visibility is already resolved on open, before the first frame.
  for (int layout = 0; layout < 2; ++layout)
  {
    auto* panel = GetControl(9600 + layout);
    auto* surface = GetControl(9610 + layout);
    if (!panel || !surface)
      continue;
    float content = 0.0f;
    for (int column = 0; column < 3; ++column)
      if (const auto* list = dynamic_cast<const CGUIControlGroupList*>(
              GetControl(9620 + layout * 10 + column)))
        content = std::max(content, list->GetTotalSize());
    if (content <= 0.0f)
      continue;
    const float height = 108.0f + content + 16.0f;
    panel->SetHeight(height);
    panel->SetPosition(panel->GetXPosition(), GetCoordsRes().iHeight - 24.0f - height);
    surface->SetHeight(height + 40.0f);
  }
}
#endif

bool CGUIDialogPlayerProcessInfo::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_PLAYER_PROCESS_INFO)
  {
#ifdef HAVE_DVBRIDGE
    const bool showMenu = CDVPlaybackModes::MenuEnabled();
    Close(showMenu);
    if (showMenu)
      CDVPlaybackModes::ShowMenu();
#else
    Close();
#endif
    return true;
  }
  return CGUIDialog::OnAction(action);
}
