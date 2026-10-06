/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "DRMObject.h"
#include <string>
#include <vector>

namespace KODI::WINDOWING::GBM
{
class CDRMAtomic;

class CDVBridgeState
{
public:
  bool Capture(CDRMAtomic& drm);
  bool Stage(CDRMAtomic& drm, bool restore);
  void Restored() { m_saved.clear(); m_owner = nullptr; }
  bool Captured() const { return !m_saved.empty(); }

private:
  struct Property
  {
    CDRMObject* object;
    std::string name;
    uint64_t original;
    uint64_t transport;
    CDRMPropertyBlob blob;
  };
  std::vector<Property> m_saved;
  CDRMAtomic* m_owner = nullptr;
};
}
