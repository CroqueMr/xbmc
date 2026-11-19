/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#ifdef HAVE_DVBRIDGE
#include "utils/DVBridgeLldv.h"
#endif

#include "VideoLayerBridge.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/lib/ISettingCallback.h"
#include "drm/DRMObject.h"
#include "drm/DRMUtils.h"
#include "threads/CriticalSection.h"
#include "threads/SystemClock.h"
#include "utils/DisplayInfo.h"
#include "windowing/WinSystem.h"

#include "platform/linux/input/LibInputHandler.h"

#include <atomic>
#include <optional>
#include <string_view>
#include <utility>

#include <gbm.h>

class IDispResource;

namespace KODI
{
namespace UTILS
{
class CDisplayInfo;
}
namespace WINDOWING
{
namespace GBM
{
class CDVBridgeState;

enum class ColorRange
{
  AUTO = 0, //!< leave it to the graphics driver
  FULL = 1, //!< output full range (0-255) and signal it
  LIMITED = 2, //!< output limited range (16-235) and signal it
};

enum class ColorFormat
{
  AUTO = 0, //!< leave it to the graphics driver
  RGB = 1,
  YUV444 = 2,
  YUV422 = 3,
  YUV420 = 4,
  ADAPTIVE = 5, //!< Auto, with YCbCr for >8 bit video when RGB does not fit the mode
};

class CWinSystemGbm : public CWinSystemBase, public ISettingCallback
{
public:
  CWinSystemGbm();
  ~CWinSystemGbm() override;

  const std::string GetName() override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  void FlipPage(bool rendered, bool videoLayer, bool async);
  bool FlipPage(bool rendered, bool videoLayer, bool async, const std::shared_ptr<gbm_bo>& carrier);
#ifdef HAVE_DVBRIDGE
  bool FlipDVBridgePage(CDVBridgeState& state, bool restore, bool async = false,
                        bool retainSignal = false, struct gbm_bo* bo = nullptr);
  bool CompleteDVBridgePage(int timeoutMs = 0);
  bool SupportsNativeDVOutput(bool requireCurrentMode = true) const;
  bool SupportsDVOutputMode(bool requireCurrentMode = true) const;
  bool RefreshDisplayInfoForVideo();
#endif

  bool CanDoWindowed() override { return false; }
  void UpdateResolutions() override;
  std::optional<uint32_t> GetBluray3DDisplayCapabilities() const override
  {
    return m_bluray3DDisplayCapabilities.load();
  }

  bool UseLimitedColor() override;

  bool Hide() override;
  bool Show(bool raise = true) override;
  void Register(IDispResource* resource) override;
  void Unregister(IDispResource* resource) override;

  bool SetVideoOutput(const VideoPicture* videoPicture) override;

  void SetColorimetry(const VideoPicture* videoPicture, bool sdrToBT709 = false) override;
  KODI::UTILS::Colorimetry GetColorimetry() const override { return m_colorimetry; }
  KODI::UTILS::Eotf GetEotf() const override { return m_eotf; }
  bool SetHDR(const VideoPicture* videoPicture) override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
#ifdef HAVE_DVBRIDGE
  enum class SDRBT2020Support { Available, Display, Driver };
  SDRBT2020Support GetSDRBT2020Support() const;
  DVBRIDGE::LldvConnectorStatus GetLldvConnectorStatus() const;
  bool FlipPlayerLedPage(CDVBridgeState& state, const DVBRIDGE::LldvTransport& packed,
                         bool async = false, gbm_bo* carrier = nullptr,
                         std::shared_ptr<void> lease = {});
#endif

  // holding the returned reference defers video plane teardown until it is released
  std::shared_ptr<CVideoLayerBridge> GetVideoLayerBridge() const { return m_videoLayerBridge; }
  void RegisterVideoLayerBridge(std::shared_ptr<CVideoLayerBridge> bridge)
  {
    m_videoLayerBridge = std::move(bridge);
  };

  CGBMUtils::CGBMDevice& GetGBMDevice() const { return m_GBM->GetDevice(); }
  std::shared_ptr<CDRMUtils> GetDrm() const { return m_DRM; }

  std::vector<std::string> GetConnectedOutputs() override;

protected:
  virtual bool ConnectorPreferencesAllowed() const { return true; }
  void DeferConnectorPreferences();
  void ApplyColorspace();
  bool CanKeepHDR10Signal(const VideoPicture& picture) const;
  bool PresentHDR10Handoff(const VideoPicture& picture);
  virtual std::optional<bool> TransportLimitedColor() const { return {}; }
  bool OrdinaryShaderLimitedColor();
  bool RequestedLimitedColor() const { return GetColorRange() == ColorRange::LIMITED; }
  virtual bool StageTransportPreferences() { return true; }
  virtual bool HasLldvRgb16Scanout() const { return false; }
  virtual bool HasLldvRgb10Scanout() const { return false; }
  virtual std::optional<uint64_t> RestoredConnectorProperty(const char*) const { return {}; }
  std::optional<uint64_t> DefaultMaxBpc() const { return m_defaultMaxBpc; }
  //! The "max bpc" in effect: the choice, or what the connector has. 0 if unknown
  uint64_t GetEffectiveMaxBpc(std::optional<uint64_t> restored = {}) const;
  bool RequeueOutputProperties();
  std::optional<uint64_t> m_queuedColorspace, m_queuedHdrMetadata;
  void OnLostDevice();

#ifdef HAVE_DVBRIDGE
  bool StageOrdinaryHDR10Recovery();
  void CompleteOrdinaryHDR10Recovery();
  const UTILS::CDisplayInfo* GetDisplayInfo() const { return m_info.get(); }
#endif
  std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock* clock) override;

  std::shared_ptr<CDRMUtils> m_DRM;
  std::unique_ptr<CGBMUtils> m_GBM;
  std::shared_ptr<CVideoLayerBridge> m_videoLayerBridge;

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;

  bool m_dispReset = false;
  XbmcThreads::EndTime<> m_dispResetTimer;
  std::unique_ptr<CLibInputHandler> m_libinput;

private:
  static void ApplyColorRangeSetting(void* userptr);
  static void ApplyColorFormatSetting(void* userptr);
  static void ApplyMaxBpcSetting(void* userptr);
  KODI::MESSAGING::ThreadMessageCallback m_colorRangeCallback{ApplyColorRangeSetting, this};
  KODI::MESSAGING::ThreadMessageCallback m_colorFormatCallback{ApplyColorFormatSetting, this};
  KODI::MESSAGING::ThreadMessageCallback m_maxBpcCallback{ApplyMaxBpcSetting, this};

  struct PropertyRequest
  {
    const char* property{nullptr};
    bool queued{false};
    uint64_t target{0};
  };

  void ApplyConnectorProperties(bool retry = false);
  void RetryConnectorProperties();
  void RequeueConnectorProperties();
  void ApplyBroadcastRange();
  void ApplyColorFormat();
  void ApplyMaxBpc();

  bool QueueConnectorProperty(PropertyRequest& request,
                              const char* property,
                              const char* valueName);
  bool QueueConnectorProperty(PropertyRequest& request,
                              const char* property,
                              uint64_t target,
                              std::string_view label);
  //! True if the request's commit was refused. Clears its queued flag
  bool IsRequestRejected(PropertyRequest& request);

  //! True if the connector offers the format, or if that is not known
  bool IsColorFormatOffered(ColorFormat format) const;

  ColorRange GetColorRange() const;
  ColorFormat GetColorFormat() const;
  //! The format to ask the driver for: the choice or Adaptive's pick, if listed and not refused
  ColorFormat GetRequestedColorFormat() const;
  //! True if the HDMI link can be driven with FRL, which carries 4K 60 Hz 10 bit RGB
  bool IsHdmiFrlLink() const;
  //! True if >8 bit video in the active HDMI mode does not fit as RGB, but does as YCbCr
  bool ContentNeedsSubsampling() const;

  bool IsYuvRequested() const;
  bool IsYuvFormatCommitted(const CDRMConnector& connector) const;
  bool IsYuvFormat(const CDRMConnector& connector, std::optional<uint64_t> current) const;

  void MigrateLimitedRangeSetting();

  void UpdateDriverLimitedRange();

  std::atomic<int> m_colorRange{static_cast<int>(ColorRange::AUTO)};
  std::atomic<int> m_shaderColorRange{static_cast<int>(ColorRange::AUTO)};

  //! True while "Broadcast RGB" is committed as "Limited 16:235"
  std::atomic<bool> m_driverLimitedRange{false};

  bool m_migratingColorRange{false};

  PropertyRequest m_rangeRequest;
  PropertyRequest m_formatRequest;
  PropertyRequest m_bpcRequest;
  int m_connectorRetries{0};
  bool m_connectorPreferencesDeferred{false};

  std::atomic<int> m_colorFormat{static_cast<int>(ColorFormat::AUTO)};

  //! Index into the "Maximum bit depth" options, 0 is Auto
  std::atomic<int> m_maxBpc{0};
  //! The driver refused the chosen "max bpc", so the default is used
  bool m_maxBpcFallback{false};
  //! The mode that refused the chosen depth, the fallback only holds there
  std::string m_maxBpcFailedMode;
  //! The default depth is being tried, to see whether the chosen one was refused
  bool m_bpcProbe{false};
  //! The "max bpc" the connector had before Kodi wrote to it
  std::optional<uint64_t> m_defaultMaxBpc;

  //! Bits per component of the video that is playing, 0 when there is none
  std::atomic<int> m_contentBits{0};

  //! How many formats the driver refused for m_colorFormatFailedMode, they are skipped there
  int m_colorFormatSkip{0};
  std::string m_colorFormatFailedMode;
  std::string m_currentModeId;

  //! Whether the "Colorspace" tag was last set for a YCbCr output
  bool m_scanoutYuv{false};
  // Claim no 3D display formats until an entire DRM mode enumeration proves them.
  std::atomic<uint32_t> m_bluray3DDisplayCapabilities{0};
  CDRMPropertyBlob m_hdrBlob;
  KODI::UTILS::Eotf m_eotf = KODI::UTILS::Eotf::TRADITIONAL_SDR;
  KODI::UTILS::Colorimetry m_colorimetry = KODI::UTILS::Colorimetry::DEFAULT;

  std::unique_ptr<UTILS::CDisplayInfo> m_info;
  std::vector<uint8_t> m_displayEdid;
#ifdef HAVE_DVBRIDGE
  mutable std::mutex m_lldvEdidMutex;
  std::vector<uint8_t> m_lldvEdid;
  DVBRIDGE::LldvDisplay m_lldvDisplay;
  uint32_t m_lldvEdidConnector{};
  uint64_t m_lldvEdidGeneration{};
  DVBRIDGE::LldvTransport m_lldvRejected;
#endif
};

} // namespace GBM
} // namespace WINDOWING
} // namespace KODI
