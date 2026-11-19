/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DRMUtils.h"
#include "utils/log.h"
#include "utils/DVBridgeOutputRetry.h"
#include "utils/DVBridgeDiagnosticState.h"

#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace KODI
{
namespace WINDOWING
{
namespace GBM
{

class CDRMAtomic : public CDRMUtils
{
public:
  CDRMAtomic() = default;
  ~CDRMAtomic() override;
  void FlipPage(struct gbm_bo* bo, bool rendered, bool videoLayer, bool async) override;
  void SetColorRangeTransition() { m_colorRangeTransition = true; }
  bool LastPageAccepted() const { return m_lastPageAccepted; }
  bool IsSDRRestorePending() const { return m_sdrRestorePending; }
  void SetSDRRestorePending(bool pending)
  {
    m_sdrRestorePending = pending;
    if (!pending)
    {
      m_sdrRestoreFailed = false;
      m_sdrRestoreColorspace.reset();
      if (m_req) m_req->sdrRestore = false;
    }
  }
  void StageHDR10Plus(std::optional<std::array<uint8_t, 31>> packet, uint64_t generation);
  bool HDR10PlusActive() const { return m_hdr10plusActive; }
  bool TestFramebuffer(struct gbm_bo* bo);
  bool IsSelectedModeCommitted() const;
  bool SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo* bo) override;
  bool SetActive(bool active) override;
  bool IsActive() const { return m_active; }
  void RequestModeset() override { m_need_modeset = true; }
  bool LastCommitFellBack() const override { return m_commitFellBack; }
  void AllowModeset() { m_need_modeset = true; }
  bool InitDrm() override;
  void DestroyDrm() override;
  bool SupportsFencing() override { return true; }
  bool AddProperty(CDRMObject* object, const char* name, uint64_t value);
  bool FlipPageTransactional(struct gbm_bo* bo, bool restore = false, bool async = false);
  bool HasPendingCB1Page() const { return m_cb1Pending; }
  bool HasPendingRequest() const { return m_need_modeset || (m_req && !m_req->Empty()); }
  bool CompleteCB1Page(int timeoutMs = 0);
  uint64_t CB1PresentationTime() const { return m_cb1CompletedNs; }
  // Kodi presentation timestamp of the picture carried by the next asynchronous commit.
  void SetCB1PresentationPts(double pts) { m_cb1NextPts = pts; }
  double CB1PresentationPts() const { return m_cb1CompletedPts; }
  uint64_t CB1PreviousPresentationTime() const { return m_cb1PreviousCompletedNs; }
  void SetCB1TimingPeriod(uint64_t periodNs);
  std::optional<int64_t> TakeCB1Headroom();
  void ResetDVActivation() { m_dvRetry.Reset(); }
  void SetDVActivationPending(bool pending) { m_dvActivating = pending; }
  bool CanAsyncDVBridge() const
  {
    auto* plane = m_gui_plane ? m_gui_plane : m_video_plane;
    return !m_need_modeset && !m_dvActivating && plane && m_crtc &&
        plane->SupportsProperty("IN_FENCE_FD") && m_crtc->SupportsProperty("OUT_FENCE_PTR");
  }
  void FailDVActivation(int error) { m_dvRetry.Failed(error); }
  bool CanRetryDVActivation(bool restored) const
  { return m_dvRetry.CanRetry(m_dvRetry.Error(), restored); }
  int DVActivationError() const { return m_dvRetry.Error(); }
  unsigned DVActivationFailures() const { return m_dvRetry.Count(); }

  // Probe whether any DRM device supports atomic modesetting, with no
  // connector required. Used to decide atomic-vs-legacy before falling back.
  static bool SupportsAtomicModesetting();

private:
  bool PrepareSDRRestore(int& flags);
  bool m_sdrRestorePending{}, m_sdrRestoreFailed{};
  std::optional<uint64_t> m_sdrRestoreColorspace;
  bool PrepareHDR10Plus(int& flags, bool& frameMetadata);
  bool RemoveRejectedHDR10Plus(int& flags);
  int TestAtomicFrame(int& flags, bool frameMetadata);
  std::optional<std::array<uint8_t, 31>> m_hdr10plusPacket;
  uint64_t m_hdr10plusGeneration{}, m_hdr10plusRejectedGeneration{}, m_hdr10plusRejectedEdid{};
  bool m_hdr10plusActive{};
  bool m_hdr10plusClearRequired{};
  void AcceptHDR10Plus(bool enabled);
  bool DrmAtomicCommit(int fb_id, int flags, bool rendered, bool videoLayer,
                       bool strict = false);
  void FinishRequest(bool accepted);
  DVBRIDGE::OutputRetry m_dvRetry;
  bool m_dvLinkKnown{false};
  uint32_t m_dvLinkConnector{0}, m_dvLinkBlob{0}, m_dvLinkStatus{0};
  void RefreshDVLink();
  void ReportDVFailure(const char* stage, int error) noexcept;
  void ReportDVCommit(const char* stage) noexcept;
  void ReportSignalState() noexcept;
  bool m_signalDirty{false}, m_signalKnown{false};
  std::array<uint64_t, 27> m_signalState{};
  drmModeModeInfo m_committedMode{};
  DVBRIDGE::DiagnosticFailures m_dvFailures;
  bool m_dvCommitReported{}, m_dvLastCommitRestore{};
  bool m_dvSnapshot{false};
  uint64_t m_dvSnapshotBlob{0};
  uint32_t m_dvSnapshotConnector{0};

  bool m_need_modeset{true};
  bool m_commitFellBack{false};
  bool m_colorRangeTransition{false};
  bool m_cb1Pending{}, m_cb1Uncertain{};
  int m_cb1OutFence{-1};
  uint64_t m_cb1CompletedNs{};
  double m_cb1NextPts{}, m_cb1PendingPts{}, m_cb1CompletedPts{};
  uint64_t m_cb1PreviousCompletedNs{};
  int m_cb1TimingInput{-1};
  uint64_t m_cb1TimingPeriodNs{}, m_cb1SubmittedNs{};
  std::optional<int64_t> m_cb1Headroom;
  void ResetCB1Timing(bool discardSample = true);
  void ResetCB1PresentationTime() { m_cb1CompletedNs = m_cb1PreviousCompletedNs = 0; }
  bool m_lastPageAccepted{};
  bool m_active = true;
  bool m_propertyFailure = false;
  bool m_dvRestoring = false;
  bool m_dvActivating = true;

  class CDRMAtomicRequest
  {
  public:
    CDRMAtomicRequest();
    ~CDRMAtomicRequest() = default;
    CDRMAtomicRequest(const CDRMAtomicRequest& right) = delete;

    drmModeAtomicReqPtr Get() const { return m_atomicRequest.get(); }
    bool Empty() const { return m_atomicRequestItems.empty(); }

    bool AddProperty(CDRMObject* object, const char* name, uint64_t value);
    void CacheProperties();
    std::optional<drmModeModeInfo> signalMode;
    std::optional<uint64_t> requestedDV;
    CDRMPropertyBlob hdr10plusBlob;
    std::optional<bool> hdr10plus;
    bool sdrRestore{};
    void LogAtomicRequest();
    void LogDVProperties();

    static void LogAtomicDiff(CDRMAtomicRequest* current, CDRMAtomicRequest* old);

  private:
    static void LogAtomicRequest(
        uint8_t logLevel, std::map<CDRMObject*, std::map<uint32_t, uint64_t>>& atomicRequestItems);

    std::map<CDRMObject*, std::map<uint32_t, uint64_t>> m_atomicRequestItems;

    struct DrmModeAtomicReqDeleter
    {
      void operator()(drmModeAtomicReqPtr p) const;
    };

    std::unique_ptr<drmModeAtomicReq, DrmModeAtomicReqDeleter> m_atomicRequest;
  };

  CDRMAtomicRequest* m_req = nullptr;
  std::deque<std::unique_ptr<CDRMAtomicRequest>> m_atomicRequestQueue;
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
