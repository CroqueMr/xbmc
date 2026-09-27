/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DVBridgeState.h"
#include "DRMAtomic.h"

using namespace KODI::WINDOWING::GBM;

bool CDVBridgeState::Capture(CDRMAtomic& drm)
{
  if (Captured())
    return false;
  auto* connector = drm.GetConnector();
  auto* crtc = drm.GetCrtc();
  auto* plane = drm.GetGuiPlane() ? drm.GetGuiPlane() : drm.GetVideoPlane();
  if (!connector || !crtc || !plane || (drm.GetGuiPlane() && drm.GetVideoPlane()))
    return false;
  if (connector->GetPropertyValue("DVBRIDGE_DV_STANDARD_LAB") != 0)
    return false;

  std::vector<Property> saved;
  const auto save = [&](CDRMObject* object, const char* name, uint64_t transport,
                        bool required, bool blob, const char* enumName = nullptr) {
    auto value = object->GetPropertyValue(name);
    if (!value)
      return !required;
    if (enumName)
    {
      auto selected = object->GetPropertyEnumValue(name, enumName);
      if (!selected)
        return false;
      transport = *selected;
    }
    Property property{object, name, *value, transport, {}};
    if (blob && *value)
    {
      auto* original = drmModeGetPropertyBlob(drm.GetFileDescriptor(), *value);
      if (!original)
        return false;
      property.blob = CDRMPropertyBlob(drm.GetFileDescriptor(), original->data, original->length);
      drmModeFreePropertyBlob(original);
      if (!property.blob.IsValid())
        return false;
      property.original = property.blob.Get();
    }
    saved.push_back(std::move(property));
    return true;
  };

  if (!save(connector, "DVBRIDGE_DV_STANDARD_LAB", 1, true, false) ||
      !save(connector, "HDR_OUTPUT_METADATA", 0, true, true) ||
      !save(connector, "max bpc", 8, true, false) ||
      !save(connector, "Broadcast RGB", 0, true, false, "Full") ||
      !save(connector, "Colorspace", 0, true, false, "Default") ||
      !save(crtc, "CTM", 0, false, true) ||
      !save(crtc, "GAMMA_LUT", 0, false, true) ||
      !save(crtc, "DEGAMMA_LUT", 0, false, true) ||
      !save(crtc, "VRR_ENABLED", 0, false, false) ||
      !save(plane, "rotation", DRM_MODE_ROTATE_0, false, false) ||
      !save(plane, "alpha", 65535, false, false) ||
      !save(plane, "COLOR_PIPELINE", 0, false, false) ||
      !save(plane, "pixel blend mode", 0, false, false, "None"))
    return false;

  // Do not silently corrupt tunnel bytes with another plane or hardware cursor.
  auto* planes = drmModeGetPlaneResources(drm.GetFileDescriptor());
  if (!planes)
    return false;
  bool safe = true;
  for (uint32_t i = 0; i < planes->count_planes; ++i)
  {
    auto* other = drmModeGetPlane(drm.GetFileDescriptor(), planes->planes[i]);
    if (!other)
    {
      safe = false;
      break;
    }
    safe &= other->plane_id == plane->GetId() || other->crtc_id != crtc->GetCrtcId();
    drmModeFreePlane(other);
  }
  drmModeFreePlaneResources(planes);
  if (!safe)
    return false;
  m_saved = std::move(saved);
  m_owner = &drm;
  for (const auto& property : m_saved)
    CLog::Log(LOGDEBUG, "DVBridge: capture object={} property={} original={} transport={}",
              property.object->GetId(), property.name, property.original, property.transport);
  CLog::Log(LOGDEBUG, "DVBridge: output state captured connector={} crtc={} plane={} properties={}",
            connector->GetId(), crtc->GetId(), plane->GetId(), m_saved.size());
  return true;
}

bool CDVBridgeState::Stage(CDRMAtomic& drm, bool restore)
{
  if (!Captured() || m_owner != &drm || m_saved.front().object != drm.GetConnector())
    return false;
  bool valid = true;
  for (const auto& property : m_saved)
    valid &= drm.AddProperty(property.object, property.name.c_str(),
                             restore ? property.original : property.transport);
  return valid;
}
