/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Snapshot public DRM properties, stage transport, then restore owned values. */
#include "DVBridgeState.h"
#include "DRMAtomic.h"
#include "DRMConnector.h"

using namespace KODI::WINDOWING::GBM;

CDVBridgeState::Backend CDVBridgeState::GetBackend(CDRMConnector& connector)
{
  if (connector.SupportsProperty("DVBRIDGE_DV_STANDARD_LAB"))
    return Backend::KERNEL;
  const bool hdmi = connector.GetType() == "HDMI-A" ||
    (connector.GetType() == "DP" && connector.SupportsProperty("CB1_NATIVE_TMDS") &&
     connector.ReadPropertyValue("CB1_NATIVE_TMDS") == 1);
  const bool vendor = hdmi &&
    connector.SupportsProperty("NV_HDMI_VSIF_METADATA") &&
    connector.SupportsProperty("max bpc") && connector.SupportsProperty("dithering mode");
  return vendor ? Backend::NVIDIA : Backend::NONE;
}

bool CDVBridgeState::Read(CDRMAtomic& drm, std::vector<Change>& changes,
                        CDRMObject* object, const char* name, uint64_t during,
                        bool required, bool blob, const char* enumName)
{
  const auto before = object->GetPropertyValue(name);
  if (!before)
    return required ? Reject(name) : true;
  if (enumName)
  {
    const auto selected = object->GetPropertyEnumValue(name, enumName);
    if (!selected)
      return Reject(name);
    during = *selected;
  }
  Change change{object, name, *before, during, {}, {}};
  if (blob && *before != 0)
  {
    auto* data = drmModeGetPropertyBlob(drm.GetFileDescriptor(), *before);
    if (!data)
      return Reject(name);
    change.beforeBlob = CDRMPropertyBlob(drm.GetFileDescriptor(), data->data, data->length);
    drmModeFreePropertyBlob(data);
    if (!change.beforeBlob.IsValid())
      return Reject(name);
    change.before = change.beforeBlob.Get();
  }
  changes.push_back(std::move(change));
  return true;
}

bool CDVBridgeState::Capture(CDRMAtomic& drm, uint64_t ownVsif, bool legacyVsif)
{
  m_failure = "none";
  if (Captured())
    return Reject("already-captured");
  auto* connector = drm.GetConnector();
  auto* crtc = drm.GetCrtc();
  auto* plane = drm.GetGuiPlane() ? drm.GetGuiPlane() : drm.GetVideoPlane();
  if (!connector || !crtc || !plane || (drm.GetGuiPlane() && drm.GetVideoPlane()))
    return Reject("output-objects");
  const Backend backend = GetBackend(*connector);
  if (backend == Backend::NONE ||
      (backend == Backend::KERNEL && connector->GetPropertyValue("DVBRIDGE_DV_STANDARD_LAB") != 0))
    return Reject("DVBRIDGE_DV_STANDARD_LAB");
  const auto vsif = connector->GetPropertyValue("NV_HDMI_VSIF_METADATA");
  const bool owned = ownVsif != 0 && vsif == ownVsif;
  if (backend == Backend::NVIDIA && vsif != 0 && !owned)
    return Reject("NV_HDMI_VSIF_METADATA");

  std::vector<Change> changes;
  struct Setting
  {
    CDRMObject* object;
    const char* name;
    uint64_t value;
    bool required, blob;
    const char* enumeration = nullptr;
  };
  if (backend == Backend::KERNEL)
  {
    const Setting native[] = {
      {connector,"DVBRIDGE_DV_STANDARD_LAB",1,true,false},
      {connector,"DVBRIDGE_HDR10_DEEP_COLOR",0,false,false},
      {connector,"HDR_OUTPUT_METADATA",0,false,true},
      {connector,"max bpc",8,true,false},
      {connector,"Broadcast RGB",0,true,false,"Full"},
      {connector,"Colorspace",0,true,false,"Default"}};
    for (const auto& setting : native)
      if (!Read(drm,changes,setting.object,setting.name,setting.value,
                setting.required,setting.blob,setting.enumeration))
        return false;
  }
  else
  {
    // Vendor VSIF payload and S31.32 neutral factors from the output contract.
    const uint8_t modern[27] = {0x46,0xd0,0,2};
    const uint8_t legacy[24] = {3,12,0};
    // The driver fills legacy timing fields from its validated active mode.
    CDRMPropertyBlob transport(drm.GetFileDescriptor(),
        legacyVsif ? legacy : modern, legacyVsif ? sizeof(legacy) : sizeof(modern));
    if (!transport.IsValid())
      return Reject("NV_HDMI_VSIF_METADATA");
    if (!Read(drm,changes,connector,"NV_HDMI_VSIF_METADATA",transport.Get(),true,true,nullptr))
      return false;
    changes.back().duringBlob = std::move(transport);
    if (owned)
    {
      changes.back().beforeBlob = CDRMPropertyBlob();
      changes.back().before = 0;
    }
    const uint64_t one = uint64_t{1} << 32;
    const Setting vendor[] = {
      {connector,"HDR_OUTPUT_METADATA",0,true,true},
      {connector,"DVBRIDGE_HDR10_DEEP_COLOR",0,false,false},
      {connector,"max bpc",8,true,false},
      {connector,"dithering mode",0,true,false,"off"},
      {connector,"Colorspace",0,true,false,"Default"},
      {crtc,"NV_CRTC_REGAMMA_TF",0,false,false,"Default"},
      {crtc,"NV_CRTC_REGAMMA_LUT",0,false,true},
      {crtc,"NV_CRTC_REGAMMA_DIVISOR",one,false,false},
      {crtc,"NV_CRTC_COLOR_PASSTHROUGH",0,false,false},
      {plane,"NV_INPUT_COLORSPACE",0,false,false,"None"},
      {plane,"NV_HDR_STATIC_METADATA",0,false,true},
      {plane,"NV_PLANE_LMS_CTM",0,false,true},
      {plane,"NV_PLANE_LMS_TO_ITP_CTM",0,false,true},
      {plane,"NV_PLANE_ITP_TO_LMS_CTM",0,false,true},
      {plane,"NV_PLANE_BLEND_CTM",0,false,true},
      {plane,"NV_PLANE_DEGAMMA_TF",0,false,false,"Default"},
      {plane,"NV_PLANE_DEGAMMA_LUT",0,false,true},
      {plane,"NV_PLANE_DEGAMMA_MULTIPLIER",one,false,false},
      {plane,"NV_PLANE_TMO_LUT",0,false,true},
      {plane,"NV_PLANE_COLOR_PASSTHROUGH",0,false,false}};
    for (const auto& setting : vendor)
      if (!Read(drm,changes,setting.object,setting.name,setting.value,
                setting.required,setting.blob,setting.enumeration))
        return false;
  }
  if (!Read(drm,changes,connector,"color format",0,false,false,"RGB"))
    return false;
  const Setting neutral[] = {
    {crtc,"CTM",0,false,true},{crtc,"GAMMA_LUT",0,false,true},
    {crtc,"DEGAMMA_LUT",0,false,true},{crtc,"VRR_ENABLED",0,false,false},
    {plane,"rotation",DRM_MODE_ROTATE_0,false,false},{plane,"alpha",65535,false,false},
    {plane,"COLOR_PIPELINE",0,false,false},{plane,"pixel blend mode",0,false,false,"None"}};
  for (const auto& setting : neutral)
    if (!Read(drm,changes,setting.object,setting.name,setting.value,
              setting.required,setting.blob,setting.enumeration))
      return false;

  auto* resources = drmModeGetPlaneResources(drm.GetFileDescriptor());
  if (!resources)
    return Reject("plane-resources");
  bool safe = true;
  for (uint32_t i=0;i<resources->count_planes;i++)
  {
    auto* item = drmModeGetPlane(drm.GetFileDescriptor(),resources->planes[i]);
    if (!item)
    {
      Reject("plane-query");safe = false;break;
    }
    if (item->plane_id != plane->GetId() && item->crtc_id == crtc->GetCrtcId())
    {
      Reject("active-plane");safe = false;
    }
    drmModeFreePlane(item);
  }
  drmModeFreePlaneResources(resources);
  if (!safe)
    return false;
  m_changes = std::move(changes);m_owner = &drm;m_backend = backend;
  return true;
}

bool CDVBridgeState::CaptureColorPreferences(CDRMAtomic& drm,
                                              std::optional<uint64_t> defaultMaxBpc)
{
  if (Captured()) return m_owner == &drm;
  auto* connector = drm.GetConnector();
  if (!connector) return Reject("connector");
  std::vector<Change> changes;
  if (!Read(drm,changes,connector,"Broadcast RGB",0,false,false,"Full") ||
      !Read(drm,changes,connector,"color format",0,false,false,"RGB") ||
      !Read(drm,changes,connector,"max bpc",
            defaultMaxBpc.value_or(connector->GetPropertyValue("max bpc").value_or(0)),
            false,false,nullptr))
    return false;
  m_changes = std::move(changes);
  m_owner = &drm;
  return true;
}

std::optional<uint64_t> CDVBridgeState::Before(const char* name) const
{
  for (const auto& change : m_changes)
    if (change.name == name) return change.before;
  return std::nullopt;
}

bool CDVBridgeState::Stage(CDRMAtomic& drm, bool restore)
{
  if (!Captured() || m_owner != &drm || m_changes.front().object != drm.GetConnector())
    return false;
  bool valid = true;
  for (const auto& change : m_changes)
    if (!drm.AddProperty(change.object,change.name.c_str(),restore ? change.before : change.during))
      valid = false;
  return valid;
}
