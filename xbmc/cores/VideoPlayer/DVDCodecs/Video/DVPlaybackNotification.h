/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef HAVE_DVBRIDGE
#include "DVOutputSettings.h"
#include "DVPlaybackInfo.h"
#include "DVMetadataBuffer.h"
#include <atomic>
#include <mutex>
#include <string>

namespace DVBRIDGE
{
// File lifecycle arms once. Only accepted presentation consumes the message.
class PlaybackNotification
{
public:
  static bool Pending() { return m_pending.load(std::memory_order_relaxed); }
  static uint64_t Generation() { return Pending() ? m_generation.load(std::memory_order_relaxed) : 0; }
  static void StartVideo()
  {
    const auto component=CServiceBroker::GetSettingsComponent();
    const auto settings=component ? component->GetSettings() : nullptr;
    const bool enabled=settings && settings->GetBool("dvbridge.startupmode");
    std::lock_guard lock(m_mutex);
    m_generation.fetch_add(1,std::memory_order_relaxed);
    m_nativeFloor=nativeRevision.load(std::memory_order_relaxed);
    m_ordinaryFrame=false;
    m_nativeFrame={};
    m_lldvFallbackRate = 0.0;
    m_pending.store(enabled,std::memory_order_relaxed);
  }
  static void StopVideo()
  {
    std::lock_guard lock(m_mutex);
    m_ordinaryFrame=false;
    m_nativeFrame={};
    m_lldvFallbackRate = 0.0;
    m_pending.store(false,std::memory_order_relaxed);
  }
  static void ArmLldvRateFallback(double fps)
  {
    std::lock_guard lock(m_mutex);
    m_lldvFallbackRate = fps;
    m_pending.store(true, std::memory_order_relaxed);
  }
  static void RenderedOrdinary(uint64_t generation)
  {
    if (!generation || !Pending()) return;
    std::lock_guard lock(m_mutex);
    if (!Pending() || generation!=m_generation.load(std::memory_order_relaxed)) return;
    m_nativeFrame={};
    m_ordinaryFrame=true;
  }
  static void RenderedNative(uint64_t generation,const SourceBinding& binding)
  {
    if (!Pending()) return;
    std::lock_guard lock(m_mutex);
    m_nativeFrame={};
    m_ordinaryFrame=false;
    if (generation && generation==m_generation.load(std::memory_order_relaxed) && binding.Valid())
      m_nativeFrame=binding;
  }
  static void OrdinaryOutputAccepted(bool accepted)
  {
    if (!Pending()) return;
    {
      std::lock_guard lock(m_mutex);
      const bool notify=accepted && m_ordinaryFrame && m_pending.exchange(false);
      m_ordinaryFrame=false;
      if (!notify) return;
    }
    Emit("Kodi : Default");
  }
  template<class Output> static void NativeOutputAccepted(const Output& output)
  {
    if (!Pending()) return;
    if (!(output.flags & (CDVPlaybackInfo::HDMI | CDVPlaybackInfo::HDR10 | CDVPlaybackInfo::SDR))) return;
    double lldvFallbackRate;
    {
      std::lock_guard lock(m_mutex);
      if (!output.picture || output.stream<=m_nativeFloor) return;
      if (output.stream!=m_nativeFrame.stream || output.picture!=m_nativeFrame.picture) return;
      if (!m_pending.exchange(false)) return;
      lldvFallbackRate = m_lldvFallbackRate;
      m_ordinaryFrame=false;
      m_nativeFrame={};
    }
    if (lldvFallbackRate > 0.0 && output.requested == CDVPlaybackInfo::PlayerLedMode &&
        (output.flags & CDVPlaybackInfo::HDR10))
    {
      EmitLldvRateFallback(lldvFallbackRate);
      return;
    }
#ifdef CB1_HAS_HDR10_AI
    if constexpr (requires { output.hdr10AI; })
      if (output.hdr10AI)
      {
        if (CDVPlaybackInfo::IsHDR10PlusDV(output))
        {
          Emit(CDVPlaybackInfo::Mode(output));
          return;
        }
        EmitAI(output.effective);
        return;
      }
#endif
    Emit(CDVPlaybackInfo::Mode(output));
  }
  static void Emit(const std::string& text);
  static void EmitAI(int mode);
  static void EmitLldvRateFallback(double fps);
private:
  inline static std::atomic<bool> m_pending{false};
  inline static std::atomic<uint64_t> m_generation{0};
  inline static std::mutex m_mutex;
  inline static bool m_ordinaryFrame{};
  inline static double m_lldvFallbackRate{};
  inline static uint64_t m_nativeFloor{};
  inline static SourceBinding m_nativeFrame{};
};
}
#endif
