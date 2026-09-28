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
#include "DVPixelFormat.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "utils/StringUtils.h"
#include "utils/Variant.h"
#include "utils/CPUInfo.h"
#include "guilib/GUIMessage.h"
#include "guilib/GUIControlGroupList.h"
#include "windowing/WinSystem.h"
#include "windowing/GraphicContext.h"
#include <algorithm>
#include <cmath>
#endif

CGUIDialogPlayerProcessInfo::CGUIDialogPlayerProcessInfo(void)
    : CGUIDialog(WINDOW_DIALOG_PLAYER_PROCESS_INFO, "DialogPlayerProcessInfo.xml")
{
  m_loadType = KEEP_IN_MEMORY;
}

CGUIDialogPlayerProcessInfo::~CGUIDialogPlayerProcessInfo(void) = default;

void CGUIDialogPlayerProcessInfo::OnInitWindow()
{
#ifdef HAVE_DVBRIDGE
  const bool enhanced = DVBRIDGE::EnhancedInfoEnabled();
  SetProperty("DV.Enhanced", enhanced);
  DVBRIDGE::StreamTelemetry::observing.store(enhanced);
  m_nextRefresh = 0;
  m_renderUsage.Reset();
  SetProperty("DV.GPUAvailable", false);
  SetProperty("DV.GPURender", "N/A");
#endif
  CGUIDialog::OnInitWindow();
}

void CGUIDialogPlayerProcessInfo::OnDeinitWindow(int nextWindowID)
{
#ifdef HAVE_DVBRIDGE
  DVBRIDGE::StreamTelemetry::observing.store(false);
  m_renderUsage.Reset();
#endif
  CGUIDialog::OnDeinitWindow(nextWindowID);
}

void CGUIDialogPlayerProcessInfo::Process(unsigned int currentTime, CDirtyRegionList& dirtyregions)
{
#ifdef HAVE_DVBRIDGE
  const bool refresh = DVBRIDGE::StreamTelemetry::observing.load() && currentTime >= m_nextRefresh;
  if (refresh)
  {
    m_nextRefresh = currentTime + 500;
    const auto render = m_renderUsage.Sample();
    SetProperty("DV.GPUAvailable", render.has_value());
    SetProperty("DV.GPURender", render ? StringUtils::Format("{:.0f} %", *render) : "N/A");
    SetProperty("DV.GPURenderPercent", render ? StringUtils::Format("{:.1f}", *render) : "0");
    for (const int gauge : {9400, 9401})
    {
      CGUIMessage usage(GUI_MSG_ITEM_SELECT, GetID(), gauge, render ? std::lround(*render) : 0);
      OnMessage(usage);
    }
    const auto data = DVBRIDGE::StreamTelemetry::Read();
    SetProperty("DV.SourcePixelFormat", DVBRIDGE::SourcePixelFormat(data.sourceFormat));
    SetProperty("DV.SourceBits", data.sourceBits ? StringUtils::Format("{}-bit", data.sourceBits) : "N/A");
    const auto active = CDVPlaybackInfo::presented.load();
    const bool metadata = active && data.metadata;
    const auto yesno = [](bool value) { return value ? "Yes" : "No"; };
    const auto resolution = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
    SetProperty("DV.Display", StringUtils::Format("{}x{} / {:.3f} Hz",
        resolution.iScreenWidth, resolution.iScreenHeight, resolution.fRefreshRate));
    const auto cpu = CServiceBroker::GetCPUInfo();
    SetProperty("DV.CPU", cpu ? StringUtils::Format("{} %", cpu->GetUsedPercentage()) : "N/A");
    SetProperty("DV.DroppedSkipped", data.counters ?
        StringUtils::Format("{} / {}", data.dropped, data.skipped) : "N/A");
    SetProperty("DV.SourceMastering", data.sourceMastering ?
        StringUtils::Format("{:.0f} / {:.4f} nits", data.sourceMax, data.sourceMin) : "N/A");
    SetProperty("DV.SourceLight", data.sourceLight ?
        StringUtils::Format("{} / {} nits", data.sourceCLL, data.sourceFALL) : "N/A");
    SetProperty("DV.Profile", data.profile ? StringUtils::Format("{}", data.profile) : "N/A");
    SetProperty("DV.Level", data.profile ? StringUtils::Format("{}", data.level) : "N/A");
    SetProperty("DV.Version", data.profile ? StringUtils::Format("{}.{}", data.major, data.minor) : "N/A");
    SetProperty("DV.CM", metadata ? (data.cm4 ? "CM4" : "CM2.9") : "N/A");
    SetProperty("DV.RPU", data.profile ? yesno(data.rpu) : "N/A");
    SetProperty("DV.BL", data.profile ? yesno(data.bl) : "N/A");
    SetProperty("DV.EL", data.profile ? yesno(data.el) : "N/A");
    SetProperty("DV.FEL", active ? yesno(active & CDVPlaybackInfo::FEL) : "N/A");
    SetProperty("DV.Mode", active & CDVPlaybackInfo::HDR10 ? "HDR10 conversion" :
        active & CDVPlaybackInfo::HDMI ? "Dolby Vision (TV-Led)" : "Kodi default");
    SetProperty("DV.Transport", active & CDVPlaybackInfo::HDMI ? "RGB 8-bit (DV tunnel)" :
        active & CDVPlaybackInfo::HDR10 ? "PQ / BT.2020" : "N/A");
    SetProperty("DV.Precision", metadata ? StringUtils::Format("{}-bit framebuffer", data.framebufferBits) : "N/A");
    SetProperty("DV.L1PQ", metadata && data.l1 ? StringUtils::Format("{} | {} | {}", data.pq[0], data.pq[1], data.pq[2]) : "N/A");
    const auto nits = [](unsigned pq) {
      const double p = std::pow(pq / 4095.0, 32.0 / 2523.0);
      return 10000.0 * std::pow(std::max(p - 3424.0 / 4096.0, 0.0) /
          (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
    };
    SetProperty("DV.L1Nits", metadata && data.l1 ? StringUtils::Format("{:.1f} | {:.1f} | {:.1f} nits",
        nits(data.pq[0]), nits(data.pq[1]), nits(data.pq[2])) : "N/A");
    SetProperty("DV.L5", metadata && data.l5 ? StringUtils::Format("{} | {} | {} | {}",
        data.area[0], data.area[1], data.area[2], data.area[3]) : "N/A");
    SetProperty("DV.Mastering", metadata && data.l6 ? StringUtils::Format("{} | {:.4f} nits",
        data.maxMaster, data.minMaster / 10000.0) : "N/A");
    SetProperty("DV.CLL", metadata && data.l6 ? StringUtils::Format("{} | {} nits", data.maxCLL, data.maxFALL) : "N/A");
  }
#endif
  CGUIDialog::Process(currentTime, dirtyregions);
#ifdef HAVE_DVBRIDGE
  if (refresh)
  {
    // Measure native grouplists after hidden rows have collapsed.
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
}

bool CGUIDialogPlayerProcessInfo::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_PLAYER_PROCESS_INFO)
  {
    Close();
    return true;
  }
  return CGUIDialog::OnAction(action);
}
