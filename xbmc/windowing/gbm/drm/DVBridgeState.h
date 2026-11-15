/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "DRMObject.h"
#include <string>
#include <vector>

namespace KODI::WINDOWING::GBM
{
class CDRMAtomic;
class CDRMConnector;

class CDVBridgeState
{
public:
  enum class Backend { NONE, KERNEL, NVIDIA };
  static Backend GetBackend(CDRMConnector&);
  bool Capture(CDRMAtomic&, uint64_t ownVsif = 0, bool legacyVsif = false);
  bool CaptureColorPreferences(CDRMAtomic&, std::optional<uint64_t> defaultMaxBpc);
  std::optional<uint64_t> Before(const char* name) const;
  bool Stage(CDRMAtomic&, bool restore, std::optional<uint64_t> broadcastRange = {});
  bool CapturePlayerLed(CDRMAtomic&);
  bool StagePlayerLed(CDRMAtomic&, uint64_t format, uint64_t bpc);
  void Restored() { m_changes.clear(); m_owner = nullptr; m_backend = Backend::NONE; }
  bool Captured() const { return !m_changes.empty(); }
  Backend CapturedBackend() const { return m_backend; }
  const char* CaptureFailure() const { return m_failure; }

private:
  struct Change
  {
    CDRMObject* object;
    std::string name;
    uint64_t before, during;
    CDRMPropertyBlob beforeBlob, duringBlob;
  };
  bool Reject(const char* reason) { m_failure = reason; return false; }
  bool Read(CDRMAtomic&, std::vector<Change>&, CDRMObject*, const char*, uint64_t,
            bool required, bool blob, const char* enumName);
  bool CaptureTransport(CDRMAtomic&, uint64_t ownVsif, bool legacyVsif, bool playerLed);
  bool StageTransport(CDRMAtomic&, bool restore, uint64_t playerLedFormat,
                      std::optional<uint64_t> broadcastRange = {}, uint64_t playerLedBpc = 8);
  std::vector<Change> m_changes;
  CDRMAtomic* m_owner = nullptr;
  Backend m_backend = Backend::NONE;
  const char* m_failure = "none";
};
}
