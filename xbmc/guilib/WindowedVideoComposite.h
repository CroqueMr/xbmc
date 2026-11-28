/*
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Geometry.h"

#include <cstdint>
#include <optional>
#include <utility>

// Only a control that participates in the GUI draw can own the deferred video pass.
class CWindowedVideoComposite
{
public:
  struct Control
  {
    CRect view;
    CRect clip;
    float depth;
    uint8_t opacity;
  };

  void BeginFrame(bool active)
  {
    m_active = active;
    m_control.reset();
    m_owner.reset();
  }
  bool Active() const { return m_active; }
  void BeginControl(const CRect& view, const CRect& clip, float depth, uint8_t opacity)
  {
    if (m_active)
      m_control = Control{view, clip, depth, opacity};
  }
  void EndControl() { m_control.reset(); }
  const Control* CurrentControl() const { return m_control ? &*m_control : nullptr; }
  std::optional<CRect> Queue(const CRect& destination)
  {
    if (!m_control || !m_control->opacity || m_control->view.IsEmpty())
      return {};
    CRect aperture = destination;
    aperture.Intersect(m_control->clip);
    if (aperture.IsEmpty())
      return {};
    // A single late presentation must cover every punched aperture. An
    // opaque foreground control may replace its fully covered predecessor
    // (e.g. a video screensaver over the Home background). A partial or
    // translucent replacement would leave an orphan or compound alpha.
    if (m_owner &&
        (m_control->opacity != 255 || aperture.x1 > m_ownerAperture.x1 ||
         aperture.y1 > m_ownerAperture.y1 || aperture.x2 < m_ownerAperture.x2 ||
         aperture.y2 < m_ownerAperture.y2))
      return {};
    m_owner = m_control;
    m_ownerAperture = aperture;
    return aperture;
  }
  bool HasOwner() const { return m_owner.has_value(); }
  std::optional<Control> TakeOwner() { return std::exchange(m_owner, std::nullopt); }

private:
  bool m_active{false};
  std::optional<Control> m_control;
  std::optional<Control> m_owner;
  CRect m_ownerAperture;
};
