/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "cores/VideoPlayer/DVDCodecs/Video/DVMetadataBuffer.h"
#include "utils/DVBridgeDiagnosticState.h"
#include "utils/Geometry.h"
#include <optional>
#include <span>
#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"
#ifdef CB1_HAS_HDR10_AI
#include "HDR10AISource.h"
#endif

extern "C"
{
#include "dvbridge_gl_frame.h"
#include "dvbridge_gl_pack.h"
#include "dvbridge_render.h"
#include "dvbridge_dmabuf.h"
}
#if DVBRIDGE_DV_POLICY_API < 5 || DVBRIDGE_HDR10_POLICY_API < 4
#error "DV bridge requires DV policy API 5 and HDR10 policy API 4"
#endif

class CDVBridgeGLES
{
public:
  struct Layer;
  class Context
  {
  public:
    Context();
    ~Context();
    bool Initialize(bool allowSoftware);
    dvbridge_renderer* Attach();
    void Detach(dvbridge_renderer* renderer);
    dvbridge_dv_policy_status Poll(bool allowAhead = false);
    bool Close();
    bool Quarantined() const;
    bool AsyncPreparation(uint64_t stream);
    pl_opengl GL() const;
    pl_log Log() const;
    void Retain(std::shared_ptr<void> base, std::shared_ptr<void> enhancement = {});
    void SourceReadComplete();
    bool FenceLastUse();
#ifdef CB1_HAS_HDR10_AI
    HDR10AISource& HDR10AI();
    bool FenceHDR10AIUse();
    bool StopHDR10AI();
    cb1_ai_status BeginHDR10AI(const char* bundle,
        const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::SourceBinding binding,
        double pts, uint64_t now);
    cb1_ai_status AnalyseHDR10AI(const Layer& base, double headPts);
    HDR10AISource::Decision PrepareHDR10AIQueue(std::span<const Layer> queue,
        const DVBRIDGE::OutputPreferences& preferences, const char* bundle,
        uint64_t now, bool end, cb1_ai_result* result, bool paused = false,
        uint64_t presentationDeadline = 0);
#endif
    static bool NewContextBlocked() { return m_stranded || m_nativeWindowRetained; }
    static void NativeWindowRetained(bool retained) { m_nativeWindowRetained = retained; }
  private:
    struct State;
    inline static State* m_stranded{};
    inline static bool m_nativeWindowRetained{};
    std::unique_ptr<State> m_state;
  };
  struct Layer
  {
    dvbridge_gl_layout layout;
    int width;
    int height;
    dvbridge_gl_plane planes[3];
    double pts;
    DVBRIDGE::SourceBinding binding{};
    std::shared_ptr<void> lease;
    DVBRIDGE::SourceInfo sourceInfo{};
    bool fullRange{};
    pl_hdr_metadata hdr{};
  };
  enum class FrameStatus { Failed, Pending, Busy, Ready, Reused, Unsupported };
#ifdef CB1_HAS_HDR10_AI
  cb1_ai_status BeginHDR10AI(const char* bundle, const DVBRIDGE::OutputPreferences& preferences,
      DVBRIDGE::SourceBinding binding, double pts, uint64_t now);
  cb1_ai_status AnalyseHDR10AI(const Layer& base, double headPts);
  cb1_ai_status CollectHDR10AI();
  cb1_ai_status SealHDR10AI(double pts);
  void EndHDR10AI();
  HDR10AISource::Decision PredictHDR10AI(DVBRIDGE::SourceBinding binding,
      const DVBRIDGE::OutputPreferences& preferences, uint64_t now, cb1_ai_result* result);
  const std::optional<cb1_ai_result>& CommittedHDR10AI() const;
  FrameStatus PrepareHDR10AIFrame(const Layer& base, dvbridge_geometry geometry,
      const DVBRIDGE::OutputPreferences& preferences, const cb1_ai_result& prediction,
      dvbridge_policy policy, bool allowAsync = false);
#endif
  FrameStatus PrepareFrame(const Layer& base, const Layer* enhancement,
                           const CDVMetadataBuffer& metadata, dvbridge_geometry geometry,
                           const DVBRIDGE::OutputPreferences& preferences, bool allowPacked,
                           const dvbridge_policy* selected = nullptr, bool allowAsync = false);
  FrameStatus PrepareHardwareFrame(const Layer& base, std::shared_ptr<AVFrame> enhancement,
      DVBRIDGE::SourceBinding enhancementBinding, const CDVMetadataBuffer& metadata,
      dvbridge_geometry geometry, const DVBRIDGE::OutputPreferences& preferences, bool allowPacked,
      const dvbridge_policy* selected = nullptr, bool allowAsync = false);
  struct Submission
  {
    dvbridge_identity identity{};
    pl_tex target{};
    uint64_t resolveSerial{}, sequence{};
    bool asyncEligible{};
  };
  Submission CaptureSubmission() const { return m_submission; }
  bool AsyncPreparationActive() const { return m_live && m_hasCommitted && m_asyncEligible; }
  bool Presented(bool success, const Submission& submission);
  bool PollReady();
  void Cancel();
  const dvbridge_hdr10_policy_output* HDR10Output() const;
  bool Fallback() const { return m_fallback; }
  bool Advanced() const { return m_advanced; }
  bool SetNativeOverlay(unsigned texture, unsigned degamma, unsigned pq,
                        const CRect& bounds, uint64_t content, bool inputPQ);
  bool NativeOverlayComposed() const { return m_direct && m_nativeOverlayEnabled; }
  bool NativeOverlayPrimed() const { return m_nativeOverlayPrimed; }
  bool SupportsMode(dvbridge_mode mode) const { return dvbridge_renderer_supports_mode(m_renderer, mode); }
  bool SupportsHDR10AI(dvbridge_mode mode) const { return dvbridge_renderer_supports_hdr10_ai(m_renderer, mode); }
  bool HasRenderer() const { return m_renderer != nullptr; }
  dvbridge_mode EffectiveMode() const { return m_effectiveMode; }
  DVBRIDGE::SourceBinding SelectionIdentity(DVBRIDGE::SourceBinding binding,
      const DVBRIDGE::OutputPreferences& preferences, dvbridge_mode mode) const;
  std::shared_ptr<Context> GetContext() const { return m_context; }

  CDVBridgeGLES() = default;
  ~CDVBridgeGLES();
  CDVBridgeGLES(const CDVBridgeGLES&) = delete;
  CDVBridgeGLES& operator=(const CDVBridgeGLES&) = delete;

  bool Initialize(bool allowSoftware = false, bool hdr10 = false,
                  std::shared_ptr<Context> context = {});
  bool SetHDR10Session(const dvbridge_hdr10_session& session, uint64_t stream = 0);
  bool RebindOutputSurface(bool hdr10);
  void Reset();
  bool Prepare(const Layer& base, const Layer* enhancement,
               const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked = false);
  bool PrepareHardware(const Layer& base, const AVFrame* enhancement,
                       const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked = false);
  unsigned int GetPQTexture() const;
  unsigned int GetPQFramebuffer();
  bool Pack(unsigned int framebuffer, int width, int height, bool flipY);
  bool ResolveHDR10(bool limited);
  bool Presented(bool success);

private:
  void CaptureMetadata(const CDVMetadataBuffer& metadata, bool fel);
  std::shared_ptr<Context> m_context;
  CDVMetadataBuffer m_pendingMetadata;
  DVBRIDGE::SourceInfo m_pendingSourceInfo{};
  std::shared_ptr<void> m_hardwareLease;
  DVBRIDGE::SourceBinding m_hardwareBinding{};
  DVBRIDGE::SourceBinding m_pendingBinding{}, m_committedBinding{};
  dvbridge_identity m_pendingIdentity{};
  dvbridge_identity m_failedIdentity{};
  DVBRIDGE::OutputPreferences m_requested;
  dvbridge_mode m_effectiveMode{DVBRIDGE_MODE_STANDARD};
  dvbridge_mode m_committedMode{DVBRIDGE_MODE_DISABLED};
  Submission m_submission{};
  uint64_t m_submissionSequence{};
  bool m_live{}, m_advanced{}, m_dvPolicyActive{}, m_pending{}, m_fallback{};
  bool m_asyncEligible{};
#ifdef CB1_HAS_HDR10_AI
  bool m_aiFrame{};
  uint64_t m_aiRenderSubmitNs{};
  DVBRIDGE::DiagnosticInterval m_aiSummary;
#endif
  bool m_hasCommitted{}, m_committedPacked{};
  bool m_guiComposed{};
  uint64_t m_committedRevision{};
  dvbridge_geometry m_sourceGeometry{};
  uint64_t m_geometryRevision{1};
  bool m_geometryKnown{};
  bool Failure(const char* stage);
  void PreparedAudit();
  void Summary(const char* event);
  DVBRIDGE::DiagnosticFailures m_failures;
  DVBRIDGE::DiagnosticFailures m_auditRequested,m_auditPrepared,m_auditCommitted;
  DVBRIDGE::DiagnosticFailures m_auditCreative;
  uint64_t m_auditStream{},m_auditSource{},m_auditPolicy{};
  int m_auditSerial{},m_auditRequestedSerial{},m_auditPreparedSerial{},m_auditCommittedSerial{};
  uint64_t m_prepares{0}, m_directFrames{0}, m_presentations{0}, m_failedPresentations{0};
  uint64_t m_lastSummary{0}, m_lastReported{0};
  bool m_firstFrame{true};
  bool m_hdr10{false};
  std::optional<dvbridge_hdr10_session> m_hdr10Session;
  uint64_t m_hdr10SessionStream{};
  DVBRIDGE::DiagnosticFailures m_hdr10Fallbacks;
  unsigned m_pendingHDR10Fallback{0}, m_hdr10FallbackReason{0};
  unsigned m_outputBits{8};
  // All methods, including destruction, require the owning GL context/thread.
  pl_opengl m_gl = nullptr;
  pl_log m_log = nullptr;
  dvbridge_renderer* m_renderer = nullptr;
  dvbridge_gl_packer* m_packer = nullptr;
  pl_tex m_scanout = nullptr;
  unsigned m_scanoutFramebuffer{};
  bool m_direct = false;
  bool m_directUsable = true;
  dvbridge_native_overlay m_nativeOverlay{};
  unsigned m_nativeOverlayTextures[3]{};
  bool m_nativeOverlayEnabled{}, m_nativeOverlayPrimed{};
  uint64_t m_nativeOverlayContent{};
  uint64_t m_nativeOverlayRevision{}, m_pendingOverlayRevision{}, m_committedOverlayRevision{};
  bool m_prepared = false;
  bool m_packed = false;
  unsigned m_pendingInfo = 0;
  unsigned int m_framebuffer = 0;
};
