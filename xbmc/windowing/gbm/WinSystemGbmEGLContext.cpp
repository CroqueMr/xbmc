/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemGbmEGLContext.h"
#include "windowing/GraphicContext.h"
#include "drm/DRMAtomic.h"

#include "OptionalsReg.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"

using namespace KODI::WINDOWING::GBM;
using namespace KODI::WINDOWING::LINUX;

CWinSystemGbmEGLContext::CWinSystemGbmEGLContext(EGLenum platform,
                                             const std::string& platformExtension)
  : CWinSystemEGL{platform, platformExtension}
{
}

bool CWinSystemGbmEGLContext::InitWindowSystemEGL(EGLint renderableType, EGLint apiType)
{
  if (!m_DRM && !CWinSystemGbm::InitWindowSystem())
  {
    return false;
  }

  if (!m_eglContext.CreatePlatformDisplay(m_GBM->GetDevice().Get(), m_GBM->GetDevice().Get()))
  {
    return false;
  }

  if (!m_eglContext.InitializeDisplay(apiType))
  {
    m_eglContext.Destroy();
    return false;
  }

  m_renderableType = renderableType;
  m_lldvRgb10Visuals.clear();
  // The DV bridge draws on GLES 3 and desktop GL
  const bool dvBridgeApi = renderableType == EGL_OPENGL_ES3_BIT || renderableType == EGL_OPENGL_BIT;
  if (dvBridgeApi)
  {
    for (const auto& format : m_DRM->GetOutputFormats())
      if (format.bpp == 10 &&
          m_eglContext.ChooseConfig(renderableType, format.drm, false, format.alpha, false, true))
        m_lldvRgb10Visuals.push_back(format.drm);
  }
  m_lldvRgb16Visual = dvBridgeApi &&
      m_eglContext.ChooseConfig(renderableType, DRM_FORMAT_ABGR16161616, false, 16, false, true);

  if (!ChooseEGLConfig(renderableType))
  {
    m_eglContext.Destroy();
    return false;
  }

  if (!CreateContext())
  {
    m_eglContext.Destroy();
    return false;
  }

  if (CEGLUtils::HasExtension(m_eglContext.GetEGLDisplay(), "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(m_eglContext.GetEGLDisplay(), "EGL_KHR_fence_sync"))
  {
    if (m_DRM->SupportsFencing())
    {
      m_eglFence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(m_eglContext.GetEGLDisplay());
    }
    else
    {
      CLog::Log(LOGWARNING, "[GBM] EGL_KHR_fence_sync and EGL_ANDROID_native_fence_sync supported"
                            ", but DRM backend doesn't support fencing");
    }
  }
  else
  {
    CLog::Log(LOGWARNING, "[GBM] missing support for EGL_KHR_fence_sync and "
                          "EGL_ANDROID_native_fence_sync - performance may be impacted");
  }

  return true;
}

bool CWinSystemGbmEGLContext::ChooseEGLConfig(EGLint renderableType, int bitDepth)
{
  if (m_dvOutputSurface && bitDepth == 16)
    return HasLldvRgb16Scanout() &&
        m_eglContext.ChooseConfig(renderableType, DRM_FORMAT_ABGR16161616, false, 16);

  // Pick the highest active output format whose bpp is in range for the
  // requested bit depth. Policy: when bitDepth > 8, accept only entries
  // with bpp >= 10 and bpp <= bitDepth (do not silently fall back to 8).
  // When bitDepth <= 8, accept entries with bpp <= 8.
  auto outputformats = m_DRM->GetOutputFormats();

  std::vector<outputformat> candidates;
  std::ranges::copy_if(outputformats, std::back_inserter(candidates),
                       [bitDepth](const outputformat& format)
                       {
                         if (!format.active || format.drm == DRM_FORMAT_ABGR16161616)
                           return false;
                         if (bitDepth > 8)
                           return format.bpp >= 10 && format.bpp <= bitDepth;
                         return format.bpp <= 8;
                       });
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const auto& a, const auto& b) { return a.bpp > b.bpp; });

  for (const auto& format : candidates)
  {
    if (m_eglContext.ChooseConfig(renderableType, format.drm, false, format.alpha, false,
                                  false, UsesNvidiaGbm()))
      return true;
  }

  if (bitDepth > 8 && FloatOutputUsable())
    for (const auto& format : outputformats)
      if (format.active && format.drm == DRM_FORMAT_ABGR16161616F &&
          m_eglContext.ChooseConfig(renderableType, format.drm, false, format.alpha, true))
        return true;
  return false;
}

bool CWinSystemGbmEGLContext::HasLldvRgb16Scanout() const
{
  if (!m_lldvRgb16Visual || !m_DRM)
    return false;
  const auto* plane = m_DRM->GetVideoPlane() ? m_DRM->GetVideoPlane() : m_DRM->GetGuiPlane();
  return plane && plane->SupportsFormat(DRM_FORMAT_ABGR16161616);
}

bool CWinSystemGbmEGLContext::HasLldvRgb10Scanout() const
{
  if (!m_DRM || m_lldvRgb10Visuals.empty())
    return false;
  const auto* plane = m_DRM->GetVideoPlane() ? m_DRM->GetVideoPlane() : m_DRM->GetGuiPlane();
  return plane && std::ranges::any_of(m_lldvRgb10Visuals, [plane](uint32_t format) {
    return plane->SupportsFormat(format);
  });
}

bool CWinSystemGbmEGLContext::FloatOutputUsable() const
{
  if (m_floatOutputBlocked || m_renderableType == EGL_OPENGL_BIT || !m_DRM) return false;
  auto* plane = m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
  if (!plane) return false;
  const auto none = plane->GetPropertyEnumValue("NV_INPUT_COLORSPACE", "None");
  return none && plane->GetPropertyValue("NV_INPUT_COLORSPACE") == none;
}

bool CWinSystemGbmEGLContext::CreateNewWindow(const std::string& name,
                                              bool fullScreen,
                                              RESOLUTION_INFO& res)
{
  //Notify other subsystems that we change resolution
  OnLostDevice();

  if (!DestroyWindow())
  {
    return false;
  }

  if (!m_DRM->SetMode(res))
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to set DRM mode", __FUNCTION__);
    return false;
  }

  uint32_t format = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  std::vector<uint64_t> modifierList = OutputModifiers(format);
  std::vector<uint64_t>* modifiers = &modifierList;

  if (!m_GBM->GetDevice().CreateSurface(res.iWidth, res.iHeight, format, modifiers->data(),
                                        modifiers->size()))
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to initialize GBM", __FUNCTION__);
    return false;
  }

  // This check + the reinterpret cast is for security reason, if the user has outdated platform header files which often is the case
  static_assert(sizeof(EGLNativeWindowType) == sizeof(gbm_surface*),
                "Declaration specifier differs in size");

  if (!m_eglContext.CreatePlatformSurface(
          m_GBM->GetDevice().GetSurface().Get(),
          reinterpret_cast<khronos_uintptr_t>(m_GBM->GetDevice().GetSurface().Get())))
  {
    return false;
  }

  if (!m_eglContext.BindContext())
  {
    return false;
  }

  if (!m_eglContext.TrySwapBuffers())
  {
    return false;
  }

  struct gbm_bo* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get();
  if (!bo)
  {
    CLog::Log(LOGERROR, "CWinSystemGbmEGLContext::{} - failed to lock front buffer", __FUNCTION__);
    return false;
  }

#if defined(HAS_GBM_MODIFIERS)
  uint64_t modifier = gbm_bo_get_modifier(bo);
#else
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
#endif
  // Offscreen has no CRTC and no scanout planes; FindGuiPlane has nothing to
  // assign, so skip the call rather than violate its post-condition that
  // m_gui_plane is set on success.
  if (m_DRM->GetCrtc() && !m_DRM->FindGuiPlane(gbm_bo_get_format(bo), modifier))
  {
    return false;
  }
  m_bFullScreen = fullScreen;
  m_nWidth = res.iWidth;
  m_nHeight = res.iHeight;
  m_fRefreshRate = res.fRefreshRate;
  CLog::Log(LOGDEBUG, "CWinSystemGbmEGLContext::{} - initialized GBM", __FUNCTION__);
  return true;
}

CWinSystemGbmEGLContext::~CWinSystemGbmEGLContext()
{
#ifdef HAVE_DVBRIDGE
  if (m_nativeDVRetirement && m_nativeDVRetirement->GL())
  {
    // RenderSystem and derived GL members have already been destroyed. This
    // final fence covers their commands before the matching context can retire.
    m_nativeDVRetirement->FenceLastUse();
    m_GBM->GetDevice().UseDVOutputLifetime();
    const bool nvidia = UsesNvidiaGbm();
    CDVWindowRetirement::Resources resources;
    resources.gbm = std::shared_ptr<CGBMUtils>(std::move(m_GBM));
    resources.drm = std::move(m_DRM);
    resources.vaapi = std::shared_ptr<CVaapiProxy>(std::move(m_vaapiProxy));
    resources.fence = std::shared_ptr<KODI::UTILS::EGL::CEGLFence>(std::move(m_eglFence));
    resources.properties = std::move(m_nativeDVProperties);
    if (auto drm = std::dynamic_pointer_cast<CDRMAtomic>(
            std::static_pointer_cast<CDRMUtils>(resources.drm));
        drm && drm->HasPendingCB1Page())
    {
      auto* gbm = static_cast<CGBMUtils*>(resources.gbm.get());
      resources.completeDisplay = [drm,gbm] {
        if (!drm->HasPendingCB1Page()) return true;
        if (!drm->CompleteCB1Page()) return false;
        gbm->GetDevice().GetSurface().CompleteTransactionalBuffer(true);
        return true;
      };
    }
    const auto egl = m_eglContext.RetainForRetirement();
    if (nvidia)
    {
      auto* gbm = static_cast<CGBMUtils*>(resources.gbm.get());
      resources.releaseDisplayBuffers = [gbm] {
        gbm->ReleaseSurfaceBuffers(true);
        gbm->ReleaseSurfaceBuffers();
      };
    }
    CDVWindowRetirement::Keep(std::move(m_nativeDVRetirement),egl,std::move(resources));
    return;
  }
#endif
  // The GBM base is destroyed before the EGL base. Finalize while both are alive.
  if (UsesNvidiaGbm() && !DestroyNvidiaWindowSystem())
  {
    CLog::LogF(LOGERROR, "Failed to retire the NVIDIA EGL window");
    // There is no caller to retry during destruction. Try the remaining
    // EGL cleanup here, while the native windows and their device still exist.
    m_GBM->ReleaseSurfaceBuffers(true);
    m_GBM->ReleaseSurfaceBuffers();
    m_eglContext.Destroy();
  }
}

bool CWinSystemGbmEGLContext::UsesNvidiaGbm() const
{
  return m_GBM && m_DRM && m_DRM->HasQuirk(QUIRK_NVIDIA_EGL_GBM);
}

bool CWinSystemGbmEGLContext::DestroyOutputSurface()
{
  if (UsesNvidiaGbm())
    return m_eglContext.DestroySurfaceAfterRelease([this] { m_GBM->ReleaseSurfaceBuffers(); });
  m_eglContext.DestroySurface();
  return true;
}

bool CWinSystemGbmEGLContext::DestroyNvidiaWindowSystem()
{
  return m_eglContext.DestroyAfterRelease([this] {
    m_GBM->ReleaseSurfaceBuffers(true);
    m_GBM->ReleaseSurfaceBuffers();
  });
}

bool CWinSystemGbmEGLContext::DestroyWindow()
{
  if (!DestroyOutputSurface())
    return false;

  CLog::Log(LOGDEBUG, "CWinSystemGbmEGLContext::{} - deinitialized GBM", __FUNCTION__);
  return true;
}

std::vector<uint64_t> CWinSystemGbmEGLContext::OutputModifiers(uint32_t format)
{
  std::vector<uint64_t> modifiers = {DRM_FORMAT_MOD_LINEAR};
  for (const auto& output : m_DRM->GetOutputFormats())
    if (output.drm == format && output.active)
    {
      modifiers = output.modifiers;
      break;
    }
#ifdef AMD_FMT_MOD_DCC_SHIFT
  const auto mode = m_DRM->GetCurrentMode();
  if (mode.dwFlags & (D3DPRESENTFLAG_MODE3DTB | D3DPRESENTFLAG_MODE3DSBS))
  {
    std::erase_if(modifiers, [](uint64_t mod) {
      return IS_AMD_FMT_MOD(mod) && AMD_FMT_MOD_GET(DCC, mod);
    });
    if (modifiers.empty()) modifiers.push_back(DRM_FORMAT_MOD_LINEAR);
  }
#endif
  return modifiers;
}

int CWinSystemGbmEGLContext::VideoOutputBitDepth(const VideoPicture* picture) const
{
  if (!m_nativeSDRDeepSurfaceBlocked && SDRBT2020DeepColorSupported() &&
      WantsNativeSDRBT2020(picture) && NativeSDRBT2020Colorspace())
    return std::max<int>(10, picture->colorBits);
  return picture ? picture->colorBits : 8;
}

bool CWinSystemGbmEGLContext::SetVideoOutput(const VideoPicture* videoPicture)
{
  if (!m_dvOutputSurface && !videoPicture)
  {
    m_floatOutputBlocked = false;
    m_nativeSDRDeepSurfaceBlocked = false;
    m_floatOutputPicture.reset();
  }
  else if (!m_dvOutputSurface && videoPicture && FloatOutputUsable() &&
           videoPicture != m_floatOutputPicture.get())
  {
    m_floatOutputPicture = std::make_unique<VideoPicture>();
    m_floatOutputPicture->SetParams(*videoPicture);
  }
  if (m_dvOutputSurface)
  {
    if (m_dvSurfacePending || !m_eglContext.BeginOutputSurface()) return false;
    m_dvSurfacePending = true;
    m_priorOutputVideo = m_DRM->GetVideoPlane() != nullptr;
    m_priorOutputPlane = m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane();
    if (m_priorOutputPlane)
    {
      m_priorOutputFormat = m_priorOutputPlane->GetFormat();
      m_priorOutputModifier = m_priorOutputPlane->GetModifier();
    }
  }
  const bool nativePromotion = !m_dvOutputSurface && videoPicture &&
      videoPicture->colorBits < 10 && !m_nativeSDRDeepSurfaceBlocked &&
      SDRBT2020DeepColorSupported() && WantsNativeSDRBT2020(videoPicture) &&
      NativeSDRBT2020Colorspace();
  const auto reject = [this,videoPicture,nativePromotion]() {
    if (m_dvOutputSurface) RejectDVOutputSurface();
    else if (nativePromotion && !m_nativeSDRDeepSurfaceBlocked)
    {
      m_nativeSDRDeepSurfaceBlocked = true;
      return SetVideoOutput(videoPicture);
    }
    else if (HasFloatOutput() && !m_floatOutputBlocked)
    {
      m_floatOutputBlocked = true;
      return SetVideoOutput(videoPicture);
    }
    return false;
  };
  // Override: the base only flips the gui/video plane role. On EGL
  // backends we additionally rebuild the GBM and EGL output surface
  // to a wider format (e.g. AR24 -> AR30) when the video needs more
  // than 8-bit through the single output plane, then chain to the
  // base so plane-role tracking sees the new format. EGL context survives
  // across the rebuild, no shader-rebuild. The next flip forces a modeset
  // when needed (amdgpu), on other drivers it is a fast flip."
  int bitDepth = !m_dvOutputSurface && m_floatOutputBlocked ?
      8 : VideoOutputBitDepth(videoPicture);

  // Pick an EGL config matching the target bit depth, then rebuild
  // the surface only if that pick changed the native visual ID.
  const auto currentConfig = m_eglContext.GetEGLConfig();
  uint32_t currentFormat = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);

  bool separateScanout = false;
  bool chosen = ChooseEGLConfig(m_renderableType, bitDepth);
  if (!chosen && nativePromotion)
  {
    m_nativeSDRDeepSurfaceBlocked = true;
    bitDepth = videoPicture->colorBits;
    chosen = ChooseEGLConfig(m_renderableType, bitDepth);
  }
  if (!chosen)
  {
    separateScanout = bitDepth >= 10 && !(m_dvOutputSurface && bitDepth == 16) &&
        PrepareHDR10Scanout(videoPicture);
    if (!separateScanout)
    {
      CLog::LogF(LOGERROR, "failed to choose EGL config for {}-bit", bitDepth);
      return reject();
    }
  }

  uint32_t format = m_eglContext.GetConfigAttrib(EGL_NATIVE_VISUAL_ID);

  if (format != currentFormat)
  {
    if (!m_dvOutputSurface && !DestroyOutputSurface())
    {
      m_eglContext.RestoreEGLConfig(currentConfig);
      return false;
    }

    auto modifierList = OutputModifiers(format);
    auto* modifiers = &modifierList;

    if (!m_GBM->GetDevice().CreateSurface(m_nWidth, m_nHeight, format, modifiers->data(),
                                          modifiers->size(),m_dvOutputSurface))
    {
      CLog::LogF(LOGERROR, "failed to create GBM surface");
      return reject();
    }

    auto* nativeSurface = m_GBM->GetDevice().GetSurface().Get();
    const bool created = m_dvOutputSurface ?
        m_eglContext.StageOutputSurface(nativeSurface,reinterpret_cast<khronos_uintptr_t>(nativeSurface)) :
        m_eglContext.CreatePlatformSurface(nativeSurface,reinterpret_cast<khronos_uintptr_t>(nativeSurface));
    if (!created)
    {
      return reject();
    }

    if (!m_eglContext.BindContext())
    {
      return reject();
    }

    if (!m_eglContext.TrySwapBuffers())
    {
      return reject();
    }

    CLog::LogF(LOGINFO, "Output surface recreated at {}-bit", bitDepth);

    // amdgpu disables the CRTC when RMFB removes a still-bound FB during
    // surface destroy (primary-plane invariant). Force a full modeset on
    // the next commit to re-enable the CRTC. No-op on drivers without the
    // quirk (Intel, RPi, etc.) so they keep the fast-flip path.
    if (m_DRM->HasQuirk(KODI::WINDOWING::GBM::QUIRK_NEEDSPRIMARY))
      m_DRM->SetActive(true);

    // The chained base reads the active plane's cached format and
    // modifier to decide which DRM plane satisfies the new role.
    // After a surface rebuild that cache is stale, so refresh it from
    // the new GBM front buffer (made available by TrySwapBuffers above)
    // before chaining.
    if (auto* plane = m_DRM->GetGuiPlane() ? m_DRM->GetGuiPlane() : m_DRM->GetVideoPlane())
    {
      if (struct gbm_bo* bo = m_GBM->GetDevice().GetSurface().LockFrontBuffer().Get())
      {
        plane->SetFormat(gbm_bo_get_format(bo));
#if defined(HAS_GBM_MODIFIERS)
        plane->SetModifier(gbm_bo_get_modifier(bo));
#else
        plane->SetModifier(DRM_FORMAT_MOD_LINEAR);
#endif
      }
    }
  }

  // Chain to base for the fine-plane part: routes through FindVideoPlane
  // if videoPicture set or FindGuiPlane if videoPicture was nullptr, using
  // the now-current cached format/modifier on the active plane.
  const bool routed = CWinSystemGbm::SetVideoOutput(videoPicture);
  if (!routed) return reject();
  if (!m_dvOutputSurface && HasFloatOutput())
  {
    auto drm = std::dynamic_pointer_cast<CDRMAtomic>(m_DRM);
    if (!drm || !drm->TestFramebuffer(m_GBM->GetDevice().GetSurface().GetLockedFrontBuffer()))
      return reject();
  }
  // No new drawable exists when only a same-format policy value changes.
  if (m_dvOutputSurface && format == currentFormat && !separateScanout &&
      !RetainHDR10ScanoutOnTransition())
    return CompleteDVOutputSurface(true);
  return true;
}

void CWinSystemGbmEGLContext::CheckFloatOutput(bool accepted)
{
  if (accepted || !HasFloatOutput()) return;
  m_floatOutputBlocked = true;
  if (m_floatOutputPicture && SetVideoOutput(m_floatOutputPicture.get()))
    RequeueOutputProperties();
}

bool CWinSystemGbmEGLContext::CompleteDVOutputSurface(bool submitted)
{
  if (!m_dvSurfacePending) return true;
  // NVIDIA's GBM release callback still uses the EGL drawable's stream.
  // Rebind first on rollback, then release the discarded drawable's buffers
  // before EGL destroys it. GBM keeps the native window alive until after EGL.
  std::function<void()> releaseBuffers;
  if (UsesNvidiaGbm())
    releaseBuffers = [this, submitted] { m_GBM->ReleaseSurfaceBuffers(submitted); };
  if (!m_eglContext.CompleteOutputSurface(submitted, releaseBuffers))
    return false;
  m_GBM->GetDevice().CompleteOutputSurface(submitted);
  if (!submitted && m_priorOutputPlane)
  {
    m_priorOutputPlane->SetFormat(m_priorOutputFormat);
    m_priorOutputPlane->SetModifier(m_priorOutputModifier);
    const bool restored = m_priorOutputVideo ?
        m_DRM->FindVideoPlane(m_priorOutputFormat,m_priorOutputModifier) :
        m_DRM->FindGuiPlane(m_priorOutputFormat,m_priorOutputModifier);
    if (!restored) return false;
  }
  m_dvSurfacePending = false;
  m_priorOutputPlane = nullptr;
  return true;
}

bool CWinSystemGbmEGLContext::DestroyWindowSystem()
{
  CDVDFactoryCodec::ClearHWAccels();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  if (UsesNvidiaGbm())
  {
    if (!DestroyNvidiaWindowSystem())
      return false;
  }
  else
    m_eglContext.Destroy();

  return CWinSystemGbm::DestroyWindowSystem();
}

void CWinSystemGbmEGLContext::delete_CVaapiProxy::operator()(CVaapiProxy* p) const
{
  VaapiProxyDelete(p);
}

bool CWinSystemGbmEGLContext::BindTextureUploadContext()
{
  return m_eglContext.BindTextureUploadContext();
}

bool CWinSystemGbmEGLContext::UnbindTextureUploadContext()
{
  return m_eglContext.UnbindTextureUploadContext();
}

bool CWinSystemGbmEGLContext::HasContext()
{
  return m_eglContext.HasContext();
}
