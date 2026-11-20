/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "DVBridgeGLES.h"
#include "VideoShaders/VideoFilterShaderGLES.h"
#include "utils/DVBridgeDiagnosticLog.h"
#include "DVBridgeShaders.h"
#include "DVBridgeScreenshot.h"
#include "rendering/capture/CaptureReadback.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"
#include "utils/log.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/Crc32.h"
#include "utils/SystemInfo.h"
#include "utils/StringUtils.h"
extern "C"
{
#include <libavutil/dovi_meta.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include "dvbridge_placebo.h"
#include "dvbridge_cache.h"
#include "cb1_hdr10plus_dovi.h"
}

#include <EGL/egl.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <GLES3/gl3.h>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(DVBRIDGE_L2_TRIMS_API) || DVBRIDGE_L2_TRIMS_API < 1
#error "Matching CB1 L2 output-trim API required"
#endif

namespace
{
void BackendLog(void*, pl_log_level level, const char* message)
{
  try
  {
  // Error-only callback: no debug shader dumps or GPU timing/readback queries.
  // Shared across renderer recreation so repeated setup errors stay bounded.
  static std::mutex mutex;
  static DVBRIDGE::DiagnosticFailures failures;
  std::lock_guard<std::mutex> lock(mutex);
  if (failures.Record(DVBRIDGE::DiagnosticNowMs(), "graphics", int(level)))
    DVBRIDGE::DiagnosticLog(LOGERROR, "DVBridge graphics: association=backend-unscoped level={} detail={}", int(level),
        std::string_view(message ? message : "unavailable").substr(0, 768));
  }
  catch (...) {} // libplacebo invokes this as a C callback.
}

pl_chroma_location SourceChroma(AVChromaLocation source, unsigned profile)
{
  const auto location = dvbridge_map_chroma(source);
  if (source == AVCHROMA_LOC_UNSPECIFIED && profile == 7) return PL_CHROMA_TOP_LEFT;
  return location == PL_CHROMA_UNKNOWN ? PL_CHROMA_LEFT : location;
}

bool ImportDVLayer(dvbridge_gl_frame* frame, pl_gpu gpu, const CDVBridgeGLES::Layer& layer)
{
  if (!dvbridge_gl_frame_import(frame,gpu,layer.layout,layer.width,layer.height,layer.planes))
    return false;
  dvbridge_gl_frame_set_chroma(frame,SourceChroma(layer.chromaLocation,layer.sourceInfo.profile));
  return true;
}
}


/* Native spatial interpolation only. Colour processing and transport stay in CB1. */
class DVNativeScaler
{
public:
  explicit DVNativeScaler(pl_gpu gpu) : m_gpu(gpu) {}
  ~DVNativeScaler()
  {
    pl_tex_destroy(m_gpu, &m_texture);
    glDeleteSamplers(1, &m_sampler);
    glDeleteBuffers(1, &m_vertices);
    glDeleteVertexArrays(1, &m_vao);
  }

  void SetMethod(ESCALINGMETHOD method, bool exact = true) { m_method = method; m_exact = exact; }

  static bool Fit(void* priv, const pl_frame* image, const pl_frame* target, pl_frame* fitted)
  {
    return static_cast<DVNativeScaler*>(priv)->Fit(*image, *target, *fitted);
  }

private:
  struct State
  {
    GLint program{}, vao{}, buffer{}, draw{}, read{}, viewport[4]{}, active{};
    GLint textures[2]{}, samplers[2]{};
    GLboolean mask[4]{}, enabled[9]{};
    static constexpr GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
        GL_SCISSOR_TEST, GL_DITHER, GL_CULL_FACE, GL_RASTERIZER_DISCARD,
        GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE};
    State()
    {
      glGetIntegerv(GL_CURRENT_PROGRAM, &program);
      glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
      glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
      glGetIntegerv(GL_VIEWPORT, viewport);
      glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
      glGetBooleanv(GL_COLOR_WRITEMASK, mask);
      for (int i = 0; i < 2; ++i)
      {
        glActiveTexture(i ? GL_TEXTURE2 : GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
        glGetIntegerv(GL_SAMPLER_BINDING, &samplers[i]);
        glBindSampler(i ? 2 : 0, 0);
      }
      for (size_t i = 0; i < std::size(caps); ++i)
      {
        enabled[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
      }
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    ~State()
    {
      for (int i = 0; i < 2; ++i)
      {
        glActiveTexture(i ? GL_TEXTURE2 : GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glBindSampler(i ? 2 : 0, samplers[i]);
      }
      glActiveTexture(active);
      glBindVertexArray(vao);
      glBindBuffer(GL_ARRAY_BUFFER, buffer);
      glUseProgram(program);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
      glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
      glColorMask(mask[0], mask[1], mask[2], mask[3]);
      for (size_t i = 0; i < std::size(caps); ++i)
        if (enabled[i]) glEnable(caps[i]); else glDisable(caps[i]);
    }
  };

  bool Fit(const pl_frame& image, const pl_frame& target, pl_frame& fitted)
  {
    if (image.num_planes != 1 || target.num_planes != 1 || image.rotation ||
        !image.planes[0].texture || !target.planes[0].texture ||
        m_method == VS_SCALINGMETHOD_AUTO || glGetError() != GL_NO_ERROR)
      return false;
    State state;
    if (!m_shader || m_compiledMethod != m_method)
    {
      std::unique_ptr<Shaders::GLES::BaseVideoFilterShader> shader;
      if (m_method == VS_SCALINGMETHOD_NEAREST || m_method == VS_SCALINGMETHOD_LINEAR)
        shader = std::make_unique<Shaders::GLES::DefaultFilterShader>();
      else
        shader = std::make_unique<Shaders::GLES::ConvolutionFilterShader>(m_method, false);
      if (!shader->CompileAndLink()) return false;
      m_shader = std::move(shader);
      m_compiledMethod = m_method;
    }
    const int width = target.planes[0].texture->params.w;
    const int height = target.planes[0].texture->params.h;
    const auto format = pl_find_fmt(m_gpu, PL_FMT_FLOAT, 4, m_exact ? 32 : 16, m_exact ? 32 : 16,
        static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE));
    pl_tex_params allocation{};
    allocation.w = width; allocation.h = height; allocation.format = format;
    allocation.sampleable = true; allocation.renderable = true;
    if (!format || !pl_tex_recreate(m_gpu, &m_texture, &allocation)) return false;
    GLenum inputTarget{};
    const auto input = pl_opengl_unwrap(m_gpu, image.planes[0].texture, &inputTarget, nullptr, nullptr);
    GLuint fbo{};
    if (!input || inputTarget != GL_TEXTURE_2D ||
        !pl_opengl_unwrap(m_gpu, m_texture, nullptr, nullptr, &fbo)) return false;
    if (!m_vao) glGenVertexArrays(1, &m_vao);
    if (!m_vertices) glGenBuffers(1, &m_vertices);
    if (!m_sampler) glGenSamplers(1, &m_sampler);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    if (target.crop.x0 || target.crop.y0 || target.crop.x1 != width || target.crop.y1 != height)
    {
      constexpr GLfloat black[4] = {0, 0, 0, 1};
      glClearBufferfv(GL_COLOR, 0, black);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input);
    GLint filter = m_method == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;
    m_shader->GetTextureFilter(filter);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindSampler(0, m_sampler);
    const float sw = image.planes[0].texture->params.w;
    const float sh = image.planes[0].texture->params.h;
    float y0 = image.crop.y0 / sh, y1 = image.crop.y1 / sh;
    if (image.planes[0].flipped) { y0 = 1 - y0; y1 = 1 - y1; }
    const float x0 = 2 * target.crop.x0 / width - 1;
    const float x1 = 2 * target.crop.x1 / width - 1;
    const float top = 2 * target.crop.y0 / height - 1;
    const float bottom = 2 * target.crop.y1 / height - 1;
    const GLfloat vertices[] = {
        x0, top, 0, image.crop.x0 / sw, y0,
        x1, top, 0, image.crop.x1 / sw, y0,
        x0, bottom, 0, image.crop.x0 / sw, y1,
        x1, bottom, 0, image.crop.x1 / sw, y1};
    constexpr GLfloat identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    m_shader->SetWidth(sw); m_shader->SetHeight(sh);
    m_shader->SetSourceTexture(0); m_shader->SetAlpha(1);
    m_shader->SetMatrices(identity, identity);
    if (!m_shader->Enable()) return false;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    const auto position = m_shader->GetVertexLoc(), coord = m_shader->GetcoordLoc();
    glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), nullptr);
    glVertexAttribPointer(coord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
        reinterpret_cast<const void*>(3 * sizeof(GLfloat)));
    glEnableVertexAttribArray(position); glEnableVertexAttribArray(coord);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_shader->Disable();
    if (glGetError() != GL_NO_ERROR) return false;
    fitted = image;
    fitted.planes[0].texture = m_texture;
    fitted.planes[0].flipped = false;
    fitted.crop = target.crop;
    return true;
  }

  bool m_exact{true};
  pl_gpu m_gpu{};
  pl_tex m_texture{};
  GLuint m_vao{}, m_vertices{}, m_sampler{};
  ESCALINGMETHOD m_method{VS_SCALINGMETHOD_LINEAR}, m_compiledMethod{VS_SCALINGMETHOD_MAX};
  std::unique_ptr<Shaders::GLES::BaseVideoFilterShader> m_shader;
};

struct CDVBridgeGLES::Context::State
{
  pl_opengl gl{};
  std::unique_ptr<DVNativeScaler> spatial;
  pl_log log{};
  dvbridge_shader_cache* shaderCache{};
  dvbridge_retirement_owner* owner{};
  struct SourceUse
  {
    GLsync fence{};
    std::vector<std::shared_ptr<void>> leases;
  };
  std::deque<SourceUse> inFlight;
  bool attached{}, quarantined{};
  uint64_t preparationStream{};
  bool asyncPreparation{};
  uint64_t felVppStream{};
  bool felVppEnabled{};
  std::vector<std::shared_ptr<void>> foreign;
#ifdef CB1_HAS_HDR10_AI
  std::unique_ptr<HDR10AISource> ai;
  struct NextHDR10AI
  {
    Layer layer;
    DVBRIDGE::SourceBinding head;
    double headPts;
    DVBRIDGE::OutputPreferences preferences;
  };
  std::optional<NextHDR10AI> aiNext;
  bool aiNextFailed{};
  GLsync aiFence{};
  bool aiStopping{};
  uint64_t aiFencedSubmission{};
#endif
};

CDVBridgeGLES::Context::Context() : m_state(std::make_unique<State>()) {}

CDVBridgeGLES::Context::~Context()
{
  if (!Close())
  {
    // Native window teardown retains the matching EGL/GBM/DRM ownership too.
    // Never reclaim foreign imports or the borrowed GPU after refused retirement.
    // Reachable process-lifetime quarantine, never a replacement allocation loop.
    m_stranded = m_state.release();
  }
}

bool CDVBridgeGLES::Context::Initialize(bool allowSoftware)
{
  if (m_state->gl) return !m_state->quarantined;
  if (NewContextBlocked()) return false;
  pl_opengl_params params{};
  params.get_proc_addr = reinterpret_cast<pl_voidfunc_t (*)(const char*)>(eglGetProcAddress);
  params.egl_display = eglGetCurrentDisplay();
  params.egl_context = eglGetCurrentContext();
  params.allow_software = allowSoftware;
  if (params.egl_context == EGL_NO_CONTEXT) return false;
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    static std::once_flag versions;
    std::call_once(versions, [] {
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge build: Kodi=28ea2eac1eb7af8fdcbd2672d933ce87f594be79 cb1=b852bd0b5e4c1422e0aa5e005520bca765dcb00a FFmpeg={} libplacebo_api={}",
          av_version_info(), PL_API_VER);
    });
    pl_log_params logging{};
    logging.log_cb = BackendLog; logging.log_level = PL_LOG_ERR;
    m_state->log = pl_log_create(PL_API_VER, &logging);
  }
  m_state->gl = pl_opengl_create(m_state->log, &params);
  if (!m_state->gl) return false;
  const auto* vendor = glGetString(GL_VENDOR);
  const auto* renderer = glGetString(GL_RENDERER);
  const auto* version = glGetString(GL_VERSION);
  if (vendor && renderer && version)
  {
    const std::string identity = std::string(reinterpret_cast<const char*>(vendor)) + '\n' +
        reinterpret_cast<const char*>(renderer) + '\n' + reinterpret_cast<const char*>(version) +
        '\n' + CSysInfo::GetOsPrettyNameWithVersion() + '\n' + pl_version();
    const auto folder = CSpecialProtocol::TranslatePath("special://temp/cb1-programs/");
    std::error_code error;
    std::filesystem::create_directories(folder,error);
    const auto path = folder + StringUtils::Format("programs-{:08x}.plcache",Crc32::Compute(identity));
    m_state->shaderCache = dvbridge_shader_cache_open(path.c_str(),32 * 1024 * 1024,m_state->log);
    if (m_state->shaderCache)
    {
      pl_gpu_set_cache(m_state->gl->gpu,dvbridge_shader_cache_get(m_state->shaderCache));
      DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 shader cache: loaded={} api={}",
          dvbridge_shader_cache_loaded(m_state->shaderCache),PL_API_VER);
    }
  }
  m_state->owner = dvbridge_retirement_owner_create(m_state->gl->gpu);
  return m_state->owner != nullptr;
}

dvbridge_renderer* CDVBridgeGLES::Context::Attach()
{
  if (m_state->quarantined || !m_state->inFlight.empty()) return nullptr;
  auto* renderer = dvbridge_renderer_create_with_owner(m_state->owner);
  if (renderer)
  {
    if (!m_state->spatial) m_state->spatial = std::make_unique<DVNativeScaler>(m_state->gl->gpu);
    dvbridge_renderer_set_spatial_fit(renderer, DVNativeScaler::Fit, m_state->spatial.get());
    m_state->attached = true;
  }
  return renderer;
}

void CDVBridgeGLES::Context::Detach(dvbridge_renderer* renderer)
{
#ifdef CB1_HAS_HDR10_AI
  ClearNextHDR10AI();
#endif
  dvbridge_renderer_destroy(renderer);
  if (m_state->shaderCache && dvbridge_shader_cache_save(m_state->shaderCache) < 0)
    DVBRIDGE::DiagnosticLog(LOGWARNING,"CB1 shader cache: save failed; normal compilation remains available");
  m_state->attached = false;
}

dvbridge_dv_policy_status CDVBridgeGLES::Context::Poll(bool allowAhead)
{
  if (m_state->quarantined) return DVBRIDGE_DV_FAILED;
  while (!m_state->inFlight.empty())
  {
    const auto& use = m_state->inFlight.front();
    const auto status = glClientWaitSync(use.fence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED)
      return allowAhead && m_state->attached && m_state->foreign.empty() &&
          m_state->inFlight.size() < 2 ? DVBRIDGE_DV_IDLE : DVBRIDGE_DV_BUSY;
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
    {
      m_state->quarantined = true;
      return DVBRIDGE_DV_FAILED;
    }
    glDeleteSync(use.fence);
    m_state->inFlight.pop_front();
  }
  if (m_state->attached) return DVBRIDGE_DV_IDLE;
#ifdef CB1_HAS_HDR10_AI
  if (m_state->aiFence)
  {
    const auto status = glClientWaitSync(m_state->aiFence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED) return DVBRIDGE_DV_BUSY;
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
    {
      m_state->quarantined = true;
      return DVBRIDGE_DV_FAILED;
    }
    glDeleteSync(m_state->aiFence); m_state->aiFence = nullptr;
  }
#endif
  const auto status = dvbridge_retirement_owner_poll(m_state->owner);
  if (status == DVBRIDGE_DV_FAILED) m_state->quarantined = true;
  if (status == DVBRIDGE_DV_IDLE) SourceReadComplete();
  return status;
}

bool CDVBridgeGLES::Context::Close()
{
#ifdef CB1_HAS_HDR10_AI
  ClearNextHDR10AI();
#endif
#ifdef CB1_HAS_HDR10_AI
  if (m_state->aiFence) return false;
#endif
  if (m_state->quarantined || !m_state->inFlight.empty() || m_state->attached ||
      !dvbridge_retirement_owner_destroy(&m_state->owner)) return false;
  SourceReadComplete();
#ifdef CB1_HAS_HDR10_AI
  if (m_state->ai) m_state->ai->ReleaseRetired();
  m_state->ai.reset();
#endif
  if (m_state->shaderCache) dvbridge_shader_cache_save(m_state->shaderCache);
  m_state->spatial.reset();
  pl_opengl_destroy(&m_state->gl);
  dvbridge_shader_cache_close(m_state->shaderCache); m_state->shaderCache = nullptr;
  pl_log_destroy(&m_state->log);
  return true;
}
bool CDVBridgeGLES::Context::Quarantined() const { return m_state->quarantined; }
bool CDVBridgeGLES::Context::AsyncPreparationConfigured() const
{
  return m_state->preparationStream && m_state->asyncPreparation;
}
bool CDVBridgeGLES::Context::AsyncPreparation(uint64_t stream)
{
  if (stream && stream != m_state->preparationStream)
  {
    m_state->preparationStream = stream;
    m_state->asyncPreparation = DVBRIDGE::AsyncPreparationEnabled();
  }
  return stream && m_state->asyncPreparation;
}
bool CDVBridgeGLES::Context::FELHardwareUpscaling(uint64_t stream)
{
  if (stream && stream != m_state->felVppStream)
  {
    m_state->felVppStream = stream;
    m_state->felVppEnabled = DVBRIDGE::FELHardwareUpscalingEnabled();
  }
  return stream && m_state->felVppEnabled;
}
void CDVBridgeGLES::Context::SetSpatialMethod(ESCALINGMETHOD method, bool exact)
{
  if (m_state->spatial) m_state->spatial->SetMethod(method, exact);
}
pl_opengl CDVBridgeGLES::Context::GL() const { return m_state->gl; }
pl_log CDVBridgeGLES::Context::Log() const { return m_state->log; }
void CDVBridgeGLES::Context::Retain(std::shared_ptr<void> base, std::shared_ptr<void> enhancement)
{
  m_state->foreign = {std::move(base), std::move(enhancement)};
}
void CDVBridgeGLES::Context::SourceReadComplete() { m_state->foreign.clear(); }
bool CDVBridgeGLES::Context::FenceLastUse()
{
  if (m_state->quarantined) return false;
  auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
  glFlush();
  if (!fence || glGetError() != GL_NO_ERROR)
  {
    m_state->quarantined = true;
    return false;
  }
  if (m_state->foreign.empty() && !m_state->inFlight.empty())
  {
    // Ordered GL commands extend the latest use without adding a third batch.
    glDeleteSync(m_state->inFlight.back().fence);
    m_state->inFlight.back().fence = fence;
  }
  else if (m_state->inFlight.size() < 2)
    m_state->inFlight.push_back({fence,std::move(m_state->foreign)});
  else
  {
    glDeleteSync(fence);
    m_state->quarantined = true;
    return false;
  }
  return true;
}

#ifdef CB1_HAS_HDR10_AI
HDR10AISource& CDVBridgeGLES::Context::HDR10AI()
{
  if (!m_state->ai) m_state->ai = std::make_unique<HDR10AISource>();
  return *m_state->ai;
}

bool CDVBridgeGLES::Context::FenceHDR10AIUse()
{
  if (m_state->quarantined) return false;
  const auto submission = HDR10AI().Metrics().gpu_submissions;
  if (!submission || submission == m_state->aiFencedSubmission) return true;
  auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
  glFlush();
  if (!fence || glGetError() != GL_NO_ERROR)
  {
    m_state->quarantined = true;
    return false;
  }
  if (m_state->aiFence) glDeleteSync(m_state->aiFence);
  m_state->aiFence = fence;
  m_state->aiFencedSubmission = submission;
  return true;
}

bool CDVBridgeGLES::Context::StopHDR10AI()
{
  ClearNextHDR10AI();
  m_state->aiNextFailed = false;
  if (!m_state->ai) return true;
  if (!m_state->aiStopping)
  {
    if (!FenceLastUse()) return false;
    m_state->aiStopping=true;
  }
  if (Poll()!=DVBRIDGE_DV_IDLE) return false;
  // The stop fence covers all analysis commands before releasing their lease.
  if (m_state->aiFence) glDeleteSync(m_state->aiFence);
  m_state->aiFence=nullptr;
  m_state->aiFencedSubmission=0;
  m_state->ai->ReleaseRetired();
  m_state->ai.reset();m_state->aiStopping=false;
  return true;
}

namespace
{
// HDR10 ingress reconstructs its own coded grid; imported planes remain unshifted.
void HDR10Color(pl_frame& frame, const CDVBridgeGLES::Layer& layer)
{
  frame.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
  frame.repr.levels = layer.fullRange ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
  frame.color.primaries = PL_COLOR_PRIM_BT_2020;
  frame.color.transfer = PL_COLOR_TRC_PQ;
  frame.color.hdr = layer.hdr;
}
}

cb1_ai_status CDVBridgeGLES::Context::BeginHDR10AI(const char* bundle,
    const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::SourceBinding binding,
    double pts, uint64_t now, bool buffered)
{
  ClearNextHDR10AI();
  m_state->aiNextFailed = false;
  return m_state->gl && !Quarantined() ?
      HDR10AI().Begin(m_state->gl->gpu,bundle,preferences,binding,pts,now,buffered) :
      HDR10AI().RecordStatus("analysis-context",CB1_AI_INCOMPATIBLE);
}

cb1_ai_status CDVBridgeGLES::Context::AnalyseHDR10AI(const Layer& base, double headPts)
{
  if (!m_state->gl || Quarantined())
    return HDR10AI().RecordStatus("analysis-context",CB1_AI_INVALID);
  if (!base.lease) return HDR10AI().RecordStatus("source-lease",CB1_AI_INVALID);
  const auto collected = HDR10AI().Collect();
  if (!FenceHDR10AIUse()) return HDR10AI().RecordStatus("analysis-fence",CB1_AI_INVALID);
  if (collected != CB1_AI_READY) return collected;
  dvbridge_gl_frame imported{};
  if (!dvbridge_gl_frame_import(&imported,m_state->gl->gpu,base.layout,base.width,base.height,base.planes))
    return HDR10AI().RecordStatus("analysis-import",CB1_AI_INCOMPATIBLE);
  HDR10Color(imported.frame,base);
  const auto status = HDR10AI().Submit(&imported.frame,base.binding,base.pts,headPts,base.lease);
  const bool fenced = FenceHDR10AIUse();
  dvbridge_gl_frame_release(&imported);
  return fenced ? status : HDR10AI().RecordStatus("analysis-fence",CB1_AI_INVALID);
}

cb1_ai_status CDVBridgeGLES::BeginHDR10AI(const char* bundle,
    const DVBRIDGE::OutputPreferences& preferences, DVBRIDGE::SourceBinding binding,
    double pts, uint64_t now)
{
  return m_context ? m_context->BeginHDR10AI(bundle,preferences,binding,pts,now) : CB1_AI_INCOMPATIBLE;
}
cb1_ai_status CDVBridgeGLES::AnalyseHDR10AI(const Layer& base, double headPts)
{
  return m_context ? m_context->AnalyseHDR10AI(base,headPts) : CB1_AI_INCOMPATIBLE;
}

cb1_ai_status CDVBridgeGLES::CollectHDR10AI()
{
  if (!m_context) return CB1_AI_INCOMPATIBLE;
  const auto status = m_context->HDR10AI().Collect();
  return m_context->FenceHDR10AIUse() ? status : CB1_AI_INCOMPATIBLE;
}
cb1_ai_status CDVBridgeGLES::SealHDR10AI(double pts)
{
  return m_context ? m_context->HDR10AI().Seal(pts) : CB1_AI_INCOMPATIBLE;
}
void CDVBridgeGLES::EndHDR10AI() { if (m_context) m_context->HDR10AI().End(); }
HDR10AISource::Decision CDVBridgeGLES::PredictHDR10AI(DVBRIDGE::SourceBinding binding,
    const DVBRIDGE::OutputPreferences& preferences, uint64_t now, cb1_ai_result* result)
{
  return m_context ? m_context->HDR10AI().Prediction(binding,preferences,now,result) :
      HDR10AISource::Decision::Native;
}
const std::optional<cb1_ai_result>& CDVBridgeGLES::CommittedHDR10AI() const
{
  return m_context->HDR10AI().Committed();
}

void CDVBridgeGLES::Context::ClearNextHDR10AI()
{
  m_state->aiNext.reset();
}

void CDVBridgeGLES::Context::AdvanceNextHDR10AI(DVBRIDGE::SourceBinding accepted,
    const DVBRIDGE::OutputPreferences& preferences)
{
  // One GUI-owned captured lease, consumed once. Never predict, drain, or wait here.
  auto next = std::move(m_state->aiNext);
  m_state->aiNext.reset();
  auto& source = HDR10AI();
  const auto collected = source.Collect();
  const bool fenced = FenceHDR10AIUse();
  if (!fenced) source.RecordStatus("analysis-fence",CB1_AI_INCOMPATIBLE);
  if (!fenced ||
      (collected != CB1_AI_READY && collected != CB1_AI_PENDING))
  {
    m_state->aiNextFailed = true;
    return;
  }
  if (!next || collected != CB1_AI_READY || m_state->aiStopping ||
      !source.Active() || source.ResetPending() ||
      accepted.stream != next->head.stream || accepted.picture != next->head.picture ||
      accepted.revision != next->head.revision ||
      preferences.revision != next->preferences.revision ||
      !preferences.SameValues(next->preferences) ||
      !source.Current(accepted,preferences) ||
      next->layer.binding.picture <= source.LastSubmitted())
    return;
  const auto status = AnalyseHDR10AI(next->layer,next->headPts);
  // The accepted current output cannot be undone by a future sample failure.
  // Preserve fatal analysis handling at the next queue preparation, without
  // charging a speculative job against the cumulative required-wait budget.
  if (status != CB1_AI_READY && status != CB1_AI_PENDING)
    m_state->aiNextFailed = true;
}

HDR10AISource::Decision CDVBridgeGLES::Context::PrepareHDR10AIQueue(
    std::span<const Layer> queue, const DVBRIDGE::OutputPreferences& preferences,
    const char* bundle, uint64_t now, bool end, cb1_ai_result* result, bool paused,
    uint64_t presentationDeadline)
{
  using Decision = HDR10AISource::Decision;
  // Every GUI capture replaces or invalidates the sole unsubmitted boundary.
  ClearNextHDR10AI();
  if (preferences.hdr10Source == DVBRIDGE::HDR10SourceMode::Native)
    return StopHDR10AI() ? Decision::Native : Decision::Wait;
  const bool retiring = m_state->aiStopping;
  if (retiring && !StopHDR10AI()) return Decision::Wait;
  if (queue.empty() || !result) return Decision::Ignore;
  // Retire completed RGB leases even while anticipation is incomplete.
  if (Poll() == DVBRIDGE_DV_FAILED)
  {
    HDR10AI().RecordStatus("rgb-retirement",CB1_AI_INCOMPATIBLE);
    return Decision::Native;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto& head = queue.front();
  const bool buffered = end || queue.back().pts >= head.pts + 1.0 - 1e-9;
  const bool guided=preferences.hdr10Source==DVBRIDGE::HDR10SourceMode::HDR10PlusDV;
  const bool snapshot=guided && DVBRIDGE::HDR10PlusDVSnapshotMatches(head.sourceSideData,
      head.binding,head.binding,head.sourceInfo.informationGeneration,head.sourceSerial,true);
  const std::shared_ptr<const AVDynamicHDRPlus> metadata=snapshot ?
      std::shared_ptr<const AVDynamicHDRPlus>(head.sourceSideData,head.sourceSideData->HDR10Plus()) : nullptr;
  HDR10AI().SetPaused(paused,now);
  if (!HDR10AI().Current(head.binding,preferences) || HDR10AI().ResetPending())
  {
    cb1_hdr10plus_dovi_result description{};
    const bool direct=metadata && cb1_hdr10plus_dovi_describe(&description,metadata.get(),nullptr)==CB1_HDR10PLUS_DOVI_OK;
    // A stop completed at entry already retired prior analysis/leases.
    if(direct && !retiring && !StopHDR10AI())return Decision::Wait;
    const auto status = direct ? (m_state->gl && !Quarantined() ?
        HDR10AI().Begin(m_state->gl->gpu,bundle,preferences,head.binding,head.pts,now,true,true) :
        HDR10AI().RecordStatus("metadata-context",CB1_AI_INCOMPATIBLE)) :
        BeginHDR10AI(bundle,preferences,head.binding,head.pts,now,buffered);
    if (status == CB1_AI_PENDING) { FenceLastUse(); return Decision::Wait; }
    if (status == CB1_AI_INVALID) { StopHDR10AI(); return Decision::Wait; }
    if (status == CB1_AI_INCOMPATIBLE) { FenceLastUse(); return Decision::Native; }
  }
  auto& source=HDR10AI();
  source.SetPaused(paused,now);
  // A fatal future sample belongs to the current activation only. A new
  // binding/policy/reset must first enter Begin's existing GPU retirement path.
  if (m_state->aiNextFailed) return Decision::Native;
  if (buffered) source.InputReady(now);
  // Only drain a complete snapshot; partial queues must let the decoder enqueue.
  source.DiscardBefore(head.binding, now);
  source.SetPresentationDeadline(presentationDeadline);
  if(source.MetadataOnly())
    return source.DescribeHDR10Plus(head.binding,metadata,head.pts,now,result);
  const bool drain = !paused && !source.AwaitingPresentation() && buffered;
  for (;;)
  {
    bool covered = false;
    const Layer* anticipation = nullptr;
    for (const auto& layer : queue)
    {
      if (source.Collect() != CB1_AI_READY) break;
      if (layer.pts >= head.pts + 1.0 - 1e-9)
      {
        anticipation = &layer;
        covered = source.Seal(layer.pts) == CB1_AI_READY;
        break;
      }
      if (layer.binding.picture <= source.LastSubmitted()) continue;
      if (AnalyseHDR10AI(layer,head.pts) != CB1_AI_READY) break;
    }
    if (!covered && end && source.Collect() == CB1_AI_READY &&
        source.LastSubmitted() >= queue.back().binding.picture)
      source.End();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const bool required = drain && now + elapsed >= presentationDeadline;
    const auto decision=source.Prediction(head.binding,preferences,now+elapsed,result,required);
    // Capture anticipation without placing it ahead of the current draw.
    if (decision == Decision::Prepare && !paused && m_state->attached && anticipation &&
        anticipation->pts <= head.pts + 1.0 + 1e-9 &&
        anticipation->binding.picture > source.LastSubmitted() &&
        source.Collect() == CB1_AI_READY)
    {
      m_state->aiNext = State::NextHDR10AI{*anticipation,head.binding,head.pts,preferences};
    }
    // Collect may dispatch follow-up reductions after the initial import fence.
    if (!FenceHDR10AIUse())
    {
      source.RecordStatus("analysis-fence",CB1_AI_INCOMPATIBLE);
      return Decision::Native;
    }
    // Let the current image present while speculative analysis is pending.
    if (decision != Decision::Wait || !required)
      return decision==Decision::Prepare && guided ?
          source.DescribeHDR10Plus(head.binding,metadata,head.pts,now+elapsed,result) : decision;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::PrepareHDR10AIFrame(const Layer& base,
    dvbridge_geometry geometry, const DVBRIDGE::OutputPreferences& preferences,
    const cb1_ai_result& prediction, dvbridge_policy policy, bool allowAsync, bool allowPacked)
{
  if (m_submission.asyncEligible) m_submission = {};
  if (m_hdr10 || !m_renderer || !m_context || m_context->Quarantined() || !base.binding.Valid() ||
      !base.lease || prediction.id.stream != base.binding.stream ||
      prediction.id.picture != base.binding.picture || !std::isfinite(base.pts))
  { m_asyncEligible = false; return FrameStatus::Failed; }
  cb1_ai_result rendition = prediction;
  std::optional<cb1_hdr10plus_dovi_result> guidance;
  if (preferences.hdr10Source == DVBRIDGE::HDR10SourceMode::HDR10PlusDV)
  {
    cb1_hdr10plus_dovi_result described{};
    const auto& analysis = m_context->HDR10AI();
    const bool wholeSource = DVBRIDGE::HDR10PlusDVWholeSource(
        geometry.crop, geometry.source_width, geometry.source_height);
    if (policy.mode != DVBRIDGE_MODE_STANDARD || !(prediction.source_flags&CB1_SOURCE_HDR10PLUS) ||
        !analysis.Current(base.binding, preferences) || !analysis.Describes(base.binding, prediction) ||
        !DVBRIDGE::HDR10PlusDVSnapshotMatches(base.sourceSideData,
            {prediction.id.stream, prediction.id.picture, base.binding.revision}, base.binding,
            base.sourceInfo.informationGeneration, base.sourceSerial, wholeSource))
    {
      Cancel();
      auto& current = m_context->HDR10AI();
      current.RecordStatus("hdr10plus-source", CB1_AI_INCOMPATIBLE);
      current.Preparation(base.binding, CB1_AI_INCOMPATIBLE, DVBRIDGE::DiagnosticNowMs());
      return FrameStatus::Unsupported;
    }
    described.metadata=prediction.raw;
    const auto* source=base.sourceSideData->HDR10Plus();
    const bool peak=source->params[0].maxscl[0].num || source->params[0].maxscl[1].num || source->params[0].maxscl[2].num;
    const bool distribution=prediction.source_flags&CB1_SOURCE_STATISTICS;
    described.source_fields=(peak ? CB1_HDR10PLUS_DOVI_MAX : 0) | (distribution ? CB1_HDR10PLUS_DOVI_AVG : 0);
    described.estimated_fields=CB1_HDR10PLUS_DOVI_MIN | (distribution ? 0 : CB1_HDR10PLUS_DOVI_AVG) |
        (peak ? 0 : CB1_HDR10PLUS_DOVI_MAX);
    described.untranslated_curve=source->params[0].tone_mapping_flag;
    described.untranslated_saturation=source->params[0].color_saturation_mapping_flag;
    guidance = described;
  }
  BeginDiagnosticStream(base.binding.stream);
  dvbridge_renderer_set_l2_trims_disabled(m_renderer, false);
  // Quarantined policy work cannot be bypassed by historical frame reuse.
  if (m_dvPolicyActive && dvbridge_render_dv_policy_poll(m_renderer) == DVBRIDGE_DV_FAILED)
    return FrameStatus::Failed;
  const bool asyncSetting = m_context->AsyncPreparation(base.binding.stream);
  const bool sameGeometry = m_geometryKnown &&
      geometry.source_width == m_sourceGeometry.source_width &&
      geometry.source_height == m_sourceGeometry.source_height &&
      geometry.x == m_sourceGeometry.x && geometry.y == m_sourceGeometry.y &&
      geometry.width == m_sourceGeometry.width && geometry.height == m_sourceGeometry.height &&
      geometry.target_width == m_sourceGeometry.target_width &&
      geometry.target_height == m_sourceGeometry.target_height &&
      std::equal(std::begin(geometry.crop), std::end(geometry.crop), std::begin(m_sourceGeometry.crop)) &&
      std::equal(std::begin(geometry.placement), std::end(geometry.placement), std::begin(m_sourceGeometry.placement)) &&
      base.scalingMethod == m_spatialMethod;
  if (m_aiFrame && m_aiOutputAccepted && !m_pending && m_hasCommitted && base.binding == m_committedBinding &&
      m_pendingBinding == m_committedBinding &&
      prediction.id.revision == m_committedRevision && policy.mode == m_committedMode &&
      preferences.revision == m_requested.revision && m_requested.SameValues(preferences) &&
      m_committedOverlayRevision == m_nativeOverlayRevision &&
      sameGeometry && (!m_direct || allowPacked))
    return FrameStatus::Reused;
  if (m_pending)
  {
    const bool sameAITransaction = base.binding == m_pendingBinding && prediction.id.revision == m_pendingIdentity.revision &&
        preferences.revision == m_requested.revision && m_requested.SameValues(preferences) &&
        sameGeometry && (!m_direct || allowPacked);
    if (sameAITransaction && policy.mode == DVBRIDGE_MODE_ENHANCED_DV && policy.mode == m_effectiveMode &&
        m_dvPolicyActive && !m_direct &&
        (m_pendingOverlayVisible != m_overlayVisible || m_pendingOverlayRevision != m_nativeOverlayRevision))
    {
      if (dvbridge_render_dv_policy_update_overlay(m_renderer,nullptr,m_overlayVisible))
      {
        m_pendingOverlayVisible = m_overlayVisible;
        m_pendingOverlayRevision = m_nativeOverlayRevision;
        return PollReady() ? FrameStatus::Ready : FrameStatus::Pending;
      }
    }
    else if (sameAITransaction && (!m_direct || m_pendingOverlayRevision == m_nativeOverlayRevision) &&
             m_pendingOverlayVisible == m_overlayVisible)
      return PollReady() ? FrameStatus::Ready : FrameStatus::Pending;
    Cancel();
  }
  const bool eligible = allowAsync && asyncSetting && !m_fallback && m_hasCommitted && m_aiOutputAccepted &&
      sameGeometry && m_aiFrame && base.binding.stream == m_committedBinding.stream &&
      base.binding.revision == m_committedBinding.revision &&
      prediction.id.revision == m_committedRevision && policy.mode == m_committedMode &&
      policy.mode == preferences.HDR10Policy().mode;
  if (m_dvPolicyActive)
  {
    const auto status = eligible && allowPacked ?
        dvbridge_render_dv_policy_advance(m_renderer) : dvbridge_render_dv_policy_poll(m_renderer);
    if (status != DVBRIDGE_DV_IDLE)
      return status == DVBRIDGE_DV_FAILED ? FrameStatus::Failed : FrameStatus::Busy;
    m_dvPolicyActive = false;
    m_context->SourceReadComplete();
  }
  const auto retired = m_context->Poll(eligible);
  if (retired != DVBRIDGE_DV_IDLE)
    return retired == DVBRIDGE_DV_FAILED ? FrameStatus::Failed : FrameStatus::Busy;
  dvbridge_gl_frame imported{};
  if (!dvbridge_gl_frame_import(&imported,m_gl->gpu,base.layout,base.width,base.height,base.planes))
    return FrameStatus::Unsupported;
  HDR10Color(imported.frame,base);
  m_spatialMethod = base.scalingMethod;
  m_context->SetSpatialMethod(m_spatialMethod, policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE);
  m_context->Retain(base.lease);
  policy.revision = prediction.id.revision;
  m_live = m_advanced = m_aiFrame = true;
  // Attempt state may replace geometry/settings and touch the accepted output.
  // Only a new accepted presentation can make that state reusable again.
  m_aiOutputAccepted = false;
  m_asyncEligible = eligible;
  m_fallback = policy.mode != preferences.HDR10Policy().mode;
  m_effectiveMode = policy.mode;
  m_requested = preferences;
  dvbridge_renderer_set_optimized_dv(m_renderer,preferences.optimizedDV);
  dvbridge_renderer_set_processing_optimizations(m_renderer,preferences.processingOptimizations);
  dvbridge_renderer_set_overlay_visible(m_renderer,m_overlayVisible);
  m_pendingOverlayRevision = m_nativeOverlayRevision;
  m_pendingOverlayVisible = m_overlayVisible;
  m_sourceGeometry = geometry;m_geometryKnown = true;
  m_pendingBinding = base.binding;m_pendingSourceInfo = base.sourceInfo;
  m_aiGuidance = guidance;
  m_pendingMetadata = {};
  m_pendingIdentity = {prediction.id.stream,prediction.id.picture,prediction.id.revision};
  m_pendingInfo = CDVPlaybackInfo::HDMI;
  m_dvPolicyActive = true;
  m_guiComposed = m_prepared = m_packed = m_pending = false;
  m_direct = false;
  const auto renderStart=std::chrono::steady_clock::now();
  bool rendered = false;
  if (allowPacked && m_directUsable && m_scanout &&
      (policy.mode == DVBRIDGE_MODE_STANDARD ||
       (policy.mode == DVBRIDGE_MODE_ENHANCED_DV && !m_overlayVisible)) &&
      (policy.mode == DVBRIDGE_MODE_ENHANCED_DV ||
       (geometry.x == 0 && geometry.y == 0 &&
        geometry.width == geometry.target_width && geometry.height == geometry.target_height) ||
       (preferences.optimizedDV && geometry.width == geometry.source_width && geometry.height == geometry.source_height)))
  {
    rendered = dvbridge_render_hdr10_ai_packed_overlay(m_renderer,&policy,&rendition,&imported.frame,
        &geometry,m_scanout,m_scanoutFramebuffer,true,m_nativeOverlayEnabled ? &m_nativeOverlay : nullptr);
    m_direct = rendered;
    if (m_direct && m_nativeOverlayEnabled) m_nativeOverlayPrimed = true;
    if (!rendered)
    {
      m_directUsable = m_asyncEligible = false;
      // A false return can follow partial GPU submission. Preserve its source
      // lease in the existing fence owner and drain before drawing RGB again.
      const bool fenced = m_context->FenceLastUse();
      const auto policyRetired = dvbridge_render_dv_policy_poll(m_renderer);
      const auto sourceRetired = m_context->Poll();
      m_dvPolicyActive = policyRetired != DVBRIDGE_DV_IDLE;
      if (!fenced || policyRetired == DVBRIDGE_DV_FAILED || sourceRetired == DVBRIDGE_DV_FAILED ||
          policyRetired != DVBRIDGE_DV_IDLE || sourceRetired != DVBRIDGE_DV_IDLE)
      {
        m_aiRenderSubmitNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-renderStart).count();
        dvbridge_gl_frame_release(&imported);
        return !fenced || policyRetired == DVBRIDGE_DV_FAILED || sourceRetired == DVBRIDGE_DV_FAILED ?
            FrameStatus::Failed : FrameStatus::Busy;
      }
      // Both owners completed. The fallback draw needs its own source lease.
      m_context->Retain(base.lease);
      m_dvPolicyActive = true;
    }
  }
  if (!rendered)
    rendered = dvbridge_render_hdr10_ai_rgb(m_renderer,&policy,&rendition,&imported.frame,&geometry);
  // Kodi composes RGB GUI into the intermediate; do not present an older prepack.
  if (rendered && !m_direct && m_overlayVisible && policy.mode == DVBRIDGE_MODE_ENHANCED_DV)
    rendered = dvbridge_render_dv_policy_update_overlay(m_renderer,nullptr,true);
  m_aiRenderSubmitNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-renderStart).count();
  const bool fenced = m_context->FenceLastUse();
  dvbridge_gl_frame_release(&imported);
  if (!rendered || !fenced) return FrameStatus::Failed;
  m_pending = true;
  if (policy.mode == DVBRIDGE_MODE_ENHANCED_DV)
    m_context->AdvanceNextHDR10AI(base.binding,preferences);
  return PollReady() ? FrameStatus::Ready : FrameStatus::Pending;
}
#endif

void CDVBridgeGLES::BeginDiagnosticStream(uint64_t stream)
{
  if (!stream || stream == m_diagnosticStream) return;
  if (m_diagnosticStream) Summary("stream-end");
  m_diagnosticStream = stream;
  m_failures.Reset();
  m_prepares = m_directFrames = m_presentations = m_failedPresentations = 0;
  m_lastSummary = m_lastReported = m_rawELImports = m_rawELHits = 0;
  m_hdr10plusDiagnostics = {};
  m_hdr10plusAnalyzed = m_hdr10plusGenerated = m_hdr10plusAccepted = {};
  m_hdr10plusSubmits = 0;
  m_hdr10plusLight = {};
  m_firstFrame = true;
}

bool CDVBridgeGLES::Failure(const char* stage)
{
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    const auto now=DVBRIDGE::DiagnosticNowMs();
    const bool changed = m_failures.Record(now, stage);
    DVBRIDGE::DiagnosticFailureLog(stage, changed, LOGERROR,
          "DVBridge renderer: stage={} failures={} mode={} master_ipt={} hdr10={} target={} pending={} prepared={} output_bits={}",
          stage,m_failures.Count(),int(m_effectiveMode),m_requested.masterIPT,m_hdr10,
          m_scanout != nullptr,m_pending,m_prepared,m_outputBits);
    const bool eligible=m_live && m_advanced && m_hasCommitted &&
        m_pendingBinding.stream==m_committedBinding.stream &&
        m_pendingBinding.revision==m_committedBinding.revision;
    if (m_failures.ReferenceDue(now,eligible))
    {
      DVBRIDGE::StreamTelemetry::ReferenceWarning();
      DVBRIDGE::DiagnosticLog(LOGWARNING, "DVBridge sustained render failure: consider Reference; policy unchanged");
    }
  }
  return false;
}

void CDVBridgeGLES::PreparedAudit()
{
  DVBRIDGE::StreamTelemetry::NativeStage({m_pendingIdentity.stream,m_pendingIdentity.picture,
      m_pendingIdentity.revision},"Prepared");
  if (m_auditPreparedSerial!=m_auditSerial && DVBRIDGE::DiagnosticsEnabled() &&
      m_auditPrepared.Record(DVBRIDGE::DiagnosticNowMs(),"prepared",m_auditSerial))
  {
    m_auditPreparedSerial=m_auditSerial;
    DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge policy prepared: source={}/{}/{} policy={} mode={}",
        m_pendingBinding.stream,m_pendingBinding.picture,m_pendingBinding.revision,
        m_pendingIdentity.revision,int(m_effectiveMode));
  }
}

void CDVBridgeGLES::Summary(const char* event)
{
  if (m_advanced)
  {
    if (!DVBRIDGE::DiagnosticsEnabled() ||
        (std::string_view(event) == "summary" && m_lastReported == m_presentations)) return;
    DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge advanced renderer {}: presented={} presentation_failures={} stage_failures={} counter_stream={}",
        event,m_presentations,m_failedPresentations,m_failures.Count(),m_diagnosticStream);
    m_lastReported = m_presentations;
    return;
  }
  if (!DVBRIDGE::DiagnosticsEnabled() ||
      (std::string_view(event) == "summary" && m_lastReported == m_prepares))
    return;
  DVBRIDGE::DiagnosticLog(LOGINFO,
      "DVBridge renderer {}: prepared={} direct={} composed={} presented={} presentation_failures={} stage_failures={} raw_el_imports={} raw_el_hits={} counter_stream={}",
      event, m_prepares, m_directFrames, m_prepares - m_directFrames,
      m_presentations, m_failedPresentations, m_failures.Count(),m_rawELImports,m_rawELHits,m_diagnosticStream);
  m_lastReported = m_prepares;
}

struct CDVBridgeGLES::FELPrefetch
{
  FELSuccessor successor;
  DVBRIDGE::OutputPreferences preferences;
  const void* pool{};
  int64_t pts{};
  int width{}, height{}, format{}, chroma{};
  std::shared_ptr<dvbridge_fel_vpp> vpp;
  const dvbridge_fel_vpp_image* image{};

  FELPrefetch(const FELSuccessor& source, const DVBRIDGE::OutputPreferences& settings)
    : successor(source), preferences(settings), pool(source.frame->hw_frames_ctx->data),
      pts(source.frame->pts), width(source.frame->width), height(source.frame->height),
      format(source.frame->format), chroma(source.frame->chroma_location)
  {}
  ~FELPrefetch()
  {
    if (image) dvbridge_fel_vpp_release(vpp.get(), image);
  }
  bool Matches(const std::shared_ptr<AVFrame>& frame, DVBRIDGE::SourceBinding binding,
      const DVBRIDGE::OutputPreferences& settings) const
  {
    return frame && frame == successor.frame && binding == successor.binding &&
        settings.revision == preferences.revision && settings.SameValues(preferences) &&
        frame->hw_frames_ctx && frame->hw_frames_ctx->data == pool && frame->pts == pts &&
        frame->width == width && frame->height == height && frame->format == format &&
        frame->chroma_location == chroma;
  }
};

void CDVBridgeGLES::ClearFELPrefetch()
{
  m_felPrefetch.reset();
}

void CDVBridgeGLES::PrefetchHardwareFEL(const FELSuccessor& successor,
    const DVBRIDGE::OutputPreferences& preferences)
{
  const auto current = m_hardwareFrame.lock();
  const auto& frame = successor.frame;
  dvbridge_color color{};
  if (!m_pending || !m_dvPolicyActive || !m_asyncEligible ||
      m_effectiveMode != DVBRIDGE_MODE_ENHANCED_DV ||
      preferences.felUpscaling != 0 ||
      preferences.revision != m_requested.revision || !preferences.SameValues(m_requested) ||
      !m_hardwareFELVpp || !m_felVppEnabled || !m_felVpp || !current || !frame ||
      !(m_hardwareBinding == m_pendingBinding) || !successor.binding.Valid() ||
      !(successor.binding == successor.metadata.Binding()) ||
      successor.binding.stream != m_pendingBinding.stream ||
      successor.binding.revision != m_pendingBinding.revision ||
      successor.binding.picture == m_pendingBinding.picture ||
      !std::isfinite(double(frame->pts)) || frame->pts <= current->pts ||
      double(frame->pts) - double(current->pts) >= 250000 ||
      !frame->hw_frames_ctx || !current->hw_frames_ctx ||
      frame->hw_frames_ctx->data != current->hw_frames_ctx->data ||
      frame->format != current->format || frame->format != AV_PIX_FMT_VAAPI ||
      frame->width != current->width || frame->height != current->height ||
      !dvbridge_map_color(&color,successor.metadata.Data(),successor.metadata.Size(),true) ||
      !color.dovi.nlq_active)
  {
    ClearFELPrefetch();
    return;
  }
  if (m_felPrefetch && m_felPrefetch->Matches(frame,successor.binding,preferences)) return;
  ClearFELPrefetch();
  auto pending = std::make_shared<FELPrefetch>(successor,preferences);
  pending->vpp = m_felVpp;
  if (dvbridge_fel_vpp_prefetch_chroma(pending->vpp.get(),frame.get(),SourceChroma(frame->chroma_location,7),
      &pending->image) == DVBRIDGE_VPP_PENDING)
    m_felPrefetch = std::move(pending);
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::TakeFELPrefetch(const std::shared_ptr<AVFrame>& frame,
    DVBRIDGE::SourceBinding binding, const DVBRIDGE::OutputPreferences& preferences,
    std::shared_ptr<dvbridge_fel_vpp>& vpp, const dvbridge_fel_vpp_image*& image)
{
  if (!m_felPrefetch) return FrameStatus::Unsupported;
  if (!m_felPrefetch->Matches(frame,binding,preferences))
  {
    ClearFELPrefetch();
    return FrameStatus::Unsupported;
  }
  if (dvbridge_fel_vpp_finish(m_felPrefetch->vpp.get(),m_felPrefetch->image) != DVBRIDGE_VPP_OK)
  {
    ClearFELPrefetch();
    return FrameStatus::Failed;
  }
  vpp = m_felPrefetch->vpp;
  image = m_felPrefetch->image;
  m_felPrefetch->image = nullptr;
  ClearFELPrefetch();
  return FrameStatus::Ready;
}

CDVBridgeGLES::~CDVBridgeGLES()
{
  cb1_hdr10plus_analysis_destroy(&m_hdr10plusAnalysis);
  ClearFELPrefetch();
  Summary("closed");
  if (!m_live) DVBRIDGE::StreamTelemetry::ClearOutput();
  glDeleteFramebuffers(1, &m_framebuffer);
  dvbridge_gl_packer_destroy(m_packer);
  if (m_context) m_context->Detach(m_renderer);
  if (m_gl)
  {
    pl_tex_destroy(m_gl->gpu, &m_nativeOverlay.texture);
    pl_tex_destroy(m_gl->gpu, &m_nativeOverlay.degamma);
    pl_tex_destroy(m_gl->gpu, &m_nativeOverlay.pq);
    pl_tex_destroy(m_gl->gpu, &m_scanout);
  }
}

void CDVBridgeGLES::SetOverlayVisible(bool visible)
{
  // Outgoing L5 differs, so a paused or packed picture must be prepared again.
  if (visible != m_overlayVisible)
    ++m_nativeOverlayRevision;
  m_overlayVisible = visible;
}

bool CDVBridgeGLES::SetNativeOverlay(unsigned texture, unsigned degamma, unsigned pq,
                                    const CRect& bounds, uint64_t content, bool inputPQ)
{
  const int width = m_scanout ? m_scanout->params.w : 0;
  const int height = m_scanout ? m_scanout->params.h : 0;
  const bool sizeChanged = m_nativeOverlay.texture &&
      (m_nativeOverlay.texture->params.w != width || m_nativeOverlay.texture->params.h != height);
  float nextRect[4]{};
  if (!bounds.IsEmpty())
  {
    nextRect[0] = std::clamp(std::floor(bounds.x1 / 2) * 2, 0.0f, float(width));
    nextRect[1] = std::clamp(std::floor(bounds.y1), 0.0f, float(height));
    nextRect[2] = std::clamp(std::ceil(bounds.x2 / 2) * 2, 0.0f, float(width));
    nextRect[3] = std::clamp(std::ceil(bounds.y2), 0.0f, float(height));
  }
  const bool formatChanged = inputPQ != m_nativeOverlay.input_pq;
  const bool enabled = m_gl && width > 0 && height > 0 && texture && degamma && pq &&
      (bounds.IsEmpty() || (nextRect[0] < nextRect[2] && nextRect[1] < nextRect[3]));
  const bool layoutChanged = m_nativeOverlayEnabled != enabled || (enabled &&
      (texture != m_nativeOverlayTextures[0] || degamma != m_nativeOverlayTextures[1] ||
       pq != m_nativeOverlayTextures[2] || formatChanged || sizeChanged ||
       !std::equal(std::begin(nextRect),std::end(nextRect),std::begin(m_nativeOverlay.rect))));
  if (layoutChanged || (enabled && content != m_nativeOverlayContent))
    ++m_nativeOverlayRevision;
  m_nativeOverlayContent = content;
  m_nativeOverlayEnabled = false;
  if (!enabled)
    return false;
  m_nativeOverlay.input_pq = inputPQ;
  std::copy(std::begin(nextRect),std::end(nextRect),std::begin(m_nativeOverlay.rect));
  const unsigned ids[]{texture, degamma, pq};
  pl_tex* wrapped[]{&m_nativeOverlay.texture, &m_nativeOverlay.degamma, &m_nativeOverlay.pq};
  for (unsigned i = 0; i < 3; ++i)
  {
    if (ids[i] == m_nativeOverlayTextures[i] && *wrapped[i] && !(i == 0 && (formatChanged || sizeChanged))) continue;
    pl_tex_destroy(m_gl->gpu, wrapped[i]);
    pl_opengl_wrap_params params{};
    params.texture = ids[i];
    params.width = i ? 256 : width;
    params.height = i ? 1 : height;
    params.iformat = i ? GL_R16F : inputPQ ? GL_RGBA16F : GL_RGBA8;
    *wrapped[i] = pl_opengl_wrap(m_gl->gpu, &params);
    m_nativeOverlayTextures[i] = ids[i];
    if (!*wrapped[i]) return false;
  }
  m_nativeOverlayEnabled = true;
  return true;
}

bool CDVBridgeGLES::Initialize(bool allowSoftware, bool hdr10, std::shared_ptr<Context> context)
{
  if (m_gl)
    return m_renderer && m_packer;
  m_hdr10 = hdr10;
  m_context = context ? std::move(context) : std::make_shared<Context>();
  if (!m_context->Initialize(allowSoftware))
    return Failure("opengl-create");
  m_gl = m_context->GL(); m_log = m_context->Log();
  // GUI alpha blending takes place in the 32F PQ target, before transport packing.
  if (!pl_opengl_has_ext(m_gl, "GL_EXT_float_blend") ||
      !pl_opengl_has_ext(m_gl, "GL_OES_texture_float_linear"))
    return Failure("float-blend-or-linear-unavailable");
  m_renderer = m_context->Attach();
  m_packer = dvbridge_gl_packer_create(DVBRIDGE_VERTEX, DVBRIDGE_FRAGMENT);
  return m_renderer && m_packer && RebindOutputSurface(hdr10);
}

bool CDVBridgeGLES::RebindOutputSurface(bool hdr10)
{
  if (!m_gl || m_pending || m_prepared) return false;
  pl_opengl_wrap_params output{};
  const auto display = eglGetCurrentDisplay();
  const auto surface = eglGetCurrentSurface(EGL_DRAW);
  if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE ||
      !eglQuerySurface(display, surface, EGL_WIDTH, &output.width) ||
      !eglQuerySurface(display, surface, EGL_HEIGHT, &output.height) ||
      output.width <= 0 || (output.width & 1) || output.height <= 0)
    return false;
  GLint draw, read, red, green, blue, samples, encoding;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glGetIntegerv(GL_RED_BITS, &red);
  glGetIntegerv(GL_GREEN_BITS, &green);
  glGetIntegerv(GL_BLUE_BITS, &blue);
  glGetIntegerv(GL_SAMPLES, &samples);
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK,
      GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
  pl_tex candidate{};
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
      ((hdr10 && red >= 10 && green >= 10 && blue >= 10) ||
       (!hdr10 && red == 8 && green == 8 && blue == 8)) &&
      samples <= 1 && encoding == GL_LINEAR)
    candidate = pl_opengl_wrap(m_gl->gpu, &output);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  if (!candidate && (hdr10 || m_live)) return false;
  // The caller has retired previous GPU use before replacing this borrowed wrapper.
  pl_tex_destroy(m_gl->gpu,&m_scanout);
  m_scanout = candidate;
  m_scanoutFramebuffer = output.framebuffer;
#ifdef CB1_HAS_HDR10_AI
  m_aiOutputAccepted = false;
  m_aiGuidance.reset();
#endif
  m_hdr10 = hdr10;
  m_outputBits = std::min(red, std::min(green, blue));
  m_failedIdentity = {};
  if (DVBRIDGE::DiagnosticsEnabled())
    DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge renderer init: rgb={}/{}/{} samples={} linear={} direct_target={} renderer={} packer={}",
        red, green, blue, samples, encoding == GL_LINEAR, m_scanout != nullptr,
        m_renderer != nullptr, m_packer != nullptr);
  return true;
}

bool CDVBridgeGLES::SetHDR10Session(const dvbridge_hdr10_session& session, uint64_t stream,
                                  bool limited)
{
  if (m_prepared || m_pending)
    return false;
  m_hdr10Limited = limited;
  if (m_hdr10Session && stream && stream == m_hdr10SessionStream)
    return true;
  if (m_hdr10Session && !stream)
    return false;
  m_hdr10Session = session;
  m_hdr10SessionStream = stream;
  return true;
}

void CDVBridgeGLES::Reset()
{
  Summary("reset");
  Cancel();
  m_rawELCache.clear();
  m_firstFrame = true;
  DVBRIDGE::StreamTelemetry::ClearOutput();
  m_pendingInfo = 0;
  m_direct = false;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrame = false;
#endif
  m_pendingHDR10Fallback = 0;
  dvbridge_renderer_reset(m_renderer);
}

bool CDVBridgeGLES::Prepare(const Layer& base, const Layer* enhancement,
                          const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked)
{
  m_prepared = m_packed = false;
  m_direct = false;
  if (!m_renderer)
    return Failure("renderer-unavailable");
  if (m_hdr10 && !m_hdr10Session)
    return Failure("hdr10-session-unavailable");
  if (!metadata.Data())
    return Failure("metadata-unavailable");
  dvbridge_renderer_set_optimized_dv(m_renderer,m_requested.optimizedDV);
  dvbridge_renderer_set_processing_optimizations(m_renderer,m_requested.processingOptimizations);
  dvbridge_renderer_set_overlay_visible(m_renderer,m_overlayVisible);
  dvbridge_gl_frame bl{}, el{};
  if (!ImportDVLayer(&bl,m_gl->gpu,base))
    return Failure("base-import");
  double elPts = std::numeric_limits<double>::quiet_NaN();
  if (enhancement)
  {
    if (!ImportDVLayer(&el,m_gl->gpu,*enhancement))
    {
      dvbridge_gl_frame_release(&bl);
      return Failure("enhancement-import");
    }
    bl.frame.enhancement_layer = &el.frame;
    elPts = enhancement->pts;
  }
  // Subtitle-only frames compose before packing. Other GUI retains 32F composition.
  const bool hdr10plus = m_effectiveMode == DVBRIDGE_MODE_HDR10PLUS;
  if (m_hdr10 && !hdr10plus && allowPacked && m_directUsable && m_scanout && !geometry.x && !geometry.y &&
      geometry.width == geometry.target_width && geometry.height == geometry.target_height)
  {
    m_direct = dvbridge_render_hdr10_direct(m_renderer, &*m_hdr10Session, &bl.frame,
        metadata.Data(), metadata.Size(), base.pts, elPts, geometry, m_scanout,
        true, m_hdr10Limited, m_outputBits);
    if (!m_direct)
    {
      Failure("hdr10-direct-unavailable-using-composition");
      m_directUsable = false;
      m_asyncEligible = false;
      dvbridge_renderer_reset(m_renderer);
    }
  }
  if (!m_hdr10 && allowPacked && (m_directUsable || m_effectiveMode == DVBRIDGE_MODE_MASTER_REFERENCE) &&
      m_scanout)
  {
    if(m_effectiveMode == DVBRIDGE_MODE_MASTER_REFERENCE)
    {
      auto renderMaster=m_requested.masterIPT ? dvbridge_render_master_reference_ipt : dvbridge_render_master_reference;
      const auto status=renderMaster(m_renderer,&bl.frame,metadata.Data(),metadata.Size(),
          base.pts,elPts,geometry,m_scanout,true,m_nativeOverlayEnabled ? &m_nativeOverlay : nullptr);
      m_direct=status==CB1_MASTER_READY;
      if(!m_direct) Failure(status==CB1_MASTER_INVALID ? "master-reference-invalid-metadata" :
          status==CB1_MASTER_UNSUPPORTED ? "master-reference-unsupported-contract" :
          status==CB1_MASTER_GPU_UNAVAILABLE ? "master-reference-gpu-unavailable" : "master-reference-unavailable");
    }
    else m_direct = m_nativeOverlayEnabled ?
        dvbridge_render_packed_overlay(m_renderer, &bl.frame, metadata.Data(), metadata.Size(),
            base.pts, elPts, geometry, m_scanout, true, &m_nativeOverlay) :
        dvbridge_render_packed(m_renderer, &bl.frame, metadata.Data(), metadata.Size(),
            base.pts, elPts, geometry, m_scanout, true);
    if (m_direct && m_nativeOverlayEnabled)
      m_nativeOverlayPrimed = true;
    if (!m_direct)
    {
      if(m_effectiveMode != DVBRIDGE_MODE_MASTER_REFERENCE)
      {
        Failure("direct-path-unavailable-using-composition");
        m_directUsable = false;
      }
      m_asyncEligible = false;
      dvbridge_renderer_reset(m_renderer);
    }
  }
  m_prepared = m_direct || (m_effectiveMode != DVBRIDGE_MODE_MASTER_REFERENCE && (m_hdr10 && !hdr10plus ?
      dvbridge_render_hdr10_rgb(m_renderer, &*m_hdr10Session, &bl.frame,
          metadata.Data(), metadata.Size(), base.pts, elPts, geometry) :
      dvbridge_render_rgb(m_renderer, &bl.frame, metadata.Data(), metadata.Size(),
          base.pts, elPts, geometry)));
  m_pendingInfo = 0;
  if (m_prepared)
  {
    // The renderer has validated all metadata offsets and extension bounds.
    const auto* parsed = reinterpret_cast<const AVDOVIMetadata*>(metadata.Data());
    if (m_hdr10 && !hdr10plus)
    {
      const auto* l1 = av_dovi_find_level(parsed, 1);
      const bool usable = l1 && l1->l1.min_pq <= l1->l1.avg_pq &&
          l1->l1.avg_pq <= l1->l1.max_pq && l1->l1.max_pq <= 4095 && l1->l1.max_pq;
      m_pendingHDR10Fallback = usable ? 0u : l1 ? 2u : 1u;
      if (m_pendingHDR10Fallback) m_asyncEligible = false;
    }
    // An enhancement surface can also belong to MEL. Report FEL only when
    // the same mapping used by the renderer actually reconstructs residuals.
    dvbridge_color color{};
    const bool fel = enhancement &&
                     dvbridge_map_color(&color, metadata.Data(), metadata.Size(), true) &&
                     color.dovi.nlq_active;
    if (!m_live) DVBRIDGE::StreamTelemetry::Metadata(parsed, fel, m_outputBits);
    else m_pendingMetadata = metadata;
    m_pendingInfo = (m_hdr10 ? CDVPlaybackInfo::HDR10 : CDVPlaybackInfo::HDMI) |
                   (fel ? CDVPlaybackInfo::FEL : 0u);
    for (int i = 0; i < parsed->num_ext_blocks; ++i)
    {
      const auto level = av_dovi_get_ext(parsed, i)->level;
      if (level == 3 || level == 8 || level == 9 || level == 10 || level == 11 || level == 254)
        m_pendingInfo |= CDVPlaybackInfo::CM4;
    }
  }
  dvbridge_gl_frame_release(&el);
  dvbridge_gl_frame_release(&bl);
  if (!m_prepared)
    return Failure("render-prepare");
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    ++m_prepares;
    m_directFrames += m_direct;
    if (m_firstFrame)
    {
      m_firstFrame = false;
      m_lastSummary = DVBRIDGE::DiagnosticNowMs();
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge first frame: source={}x{} output={}x{} offset={},{} enhancement={} fel={} cm4={} direct={} bl_layout={} bl_texture={}x{} el={}x{} el_layout={} el_texture={}x{} bl_chroma={} el_chroma={} bl_uv_texture={}x{} el_uv_texture={}x{} bl_texture_ids={},{} el_texture_ids={},{}",
          base.width, base.height, geometry.width, geometry.height, geometry.x, geometry.y,
          enhancement != nullptr, bool(m_pendingInfo & CDVPlaybackInfo::FEL),
          bool(m_pendingInfo & CDVPlaybackInfo::CM4), m_direct,
          int(base.layout), base.planes[0].width, base.planes[0].height,
          enhancement ? enhancement->width : 0, enhancement ? enhancement->height : 0,
          enhancement ? int(enhancement->layout) : -1,
          enhancement ? enhancement->planes[0].width : 0, enhancement ? enhancement->planes[0].height : 0,
          int(base.chromaLocation), enhancement ? int(enhancement->chromaLocation) : -1,
          base.planes[1].width,base.planes[1].height,
          enhancement ? enhancement->planes[1].width : 0,enhancement ? enhancement->planes[1].height : 0,
          base.planes[0].texture,base.planes[1].texture,
          enhancement ? enhancement->planes[0].texture : 0,enhancement ? enhancement->planes[1].texture : 0);
    }
  }
  return m_prepared;
}

DVBRIDGE::SourceBinding CDVBridgeGLES::SelectionIdentity(DVBRIDGE::SourceBinding binding,
    const DVBRIDGE::OutputPreferences& preferences, dvbridge_mode mode) const
{
  // Availability observes an existing processing transaction only with exact
  // original source, saved policy and effective-mode proof, never key ordering.
  if (!binding.Valid() || binding != m_pendingBinding || mode == DVBRIDGE_MODE_DISABLED ||
      !m_requested.SameValues(preferences) || m_requested.revision != preferences.revision ||
      mode != m_effectiveMode || dvbridge_identity_equal(&m_failedIdentity, &m_pendingIdentity))
    return {};
  if (!m_pending && (!m_hasCommitted || binding != m_committedBinding ||
      m_pendingIdentity.revision != m_committedRevision || mode != m_committedMode))
    return {};
  return {m_pendingIdentity.stream,m_pendingIdentity.picture,m_pendingIdentity.revision};
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::PrepareFrame(const Layer& base, const Layer* enhancement,
    const CDVMetadataBuffer& metadata, dvbridge_geometry geometry,
    const DVBRIDGE::OutputPreferences& preferences, bool allowPacked, const dvbridge_policy* selected,
    bool allowAsync)
{
  if (base.binding.Valid()) BeginDiagnosticStream(base.binding.stream);
  DVBRIDGE::ObserveDiagnostic(base.binding.stream,base.binding.picture,base.binding.revision,
      preferences.revision,"prepare-entry",preferences.output != DVBRIDGE::OutputMode::Disabled,false);
  if (m_submission.asyncEligible) m_submission = {};
  if (!base.binding.Valid() || base.binding != metadata.Binding() ||
      !std::isfinite(base.pts) ||
      (enhancement && (enhancement->binding != base.binding ||
          !std::isfinite(enhancement->pts) || enhancement->pts != base.pts)))
  { Failure("source-association"); m_asyncEligible = false; return FrameStatus::Failed; }
  if (!m_renderer || !m_context || m_context->Quarantined())
  { Failure("renderer-unavailable"); m_asyncEligible = false; return FrameStatus::Failed; }
  const bool asyncSetting = m_context->AsyncPreparation(base.binding.stream);
  auto policy = selected ? *selected : preferences.Policy();
  if (policy.mode == DVBRIDGE_MODE_HDR10PLUS && !m_hdr10plusSubmits)
    DVBRIDGE::DiagnosticReserveProgress();
  if (policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE && !allowPacked)
  {
    // The next GUI pass can create its composition target. No render was attempted.
    DVBRIDGE::DiagnosticStage("master-await-overlay");
    return FrameStatus::Busy;
  }
  // A selected policy already passed output admission, including compatibility routes.
  const bool fallback = !selected &&
      (policy.mode == DVBRIDGE_MODE_HDR10_EXPERT || policy.mode == DVBRIDGE_MODE_ENHANCED_DV) && !preferences.ProfileValid();
  if (fallback)
  {
    policy.mode = preferences.output == DVBRIDGE::OutputMode::HDR10 ?
        DVBRIDGE_MODE_HDR10_BASIC : DVBRIDGE_MODE_STANDARD;
    policy.tv = {};
    policy.adaptive = DVBRIDGE_ADAPTIVE_A;
  }
  // Capture the proven observation key before geometry or refresh rekeys work.
  dvbridge_renderer_set_l2_trims_disabled(m_renderer, preferences.L2TrimsDisabled(policy.mode));
  const auto cmv40 = preferences.CMv40Options(fallback ? DVBRIDGE_MODE_DISABLED : policy.mode);
  dvbridge_renderer_set_cmv40(m_renderer, &cmv40);
  const auto admissionIdentity = SelectionIdentity(base.binding, preferences,
      policy.mode);
  const bool sameGeometry = geometry.source_width == m_sourceGeometry.source_width &&
      geometry.source_height == m_sourceGeometry.source_height &&
      geometry.x == m_sourceGeometry.x && geometry.y == m_sourceGeometry.y &&
      geometry.width == m_sourceGeometry.width && geometry.height == m_sourceGeometry.height &&
      geometry.target_width == m_sourceGeometry.target_width &&
      geometry.target_height == m_sourceGeometry.target_height &&
      std::equal(std::begin(geometry.crop), std::end(geometry.crop), std::begin(m_sourceGeometry.crop)) &&
      std::equal(std::begin(geometry.placement), std::end(geometry.placement), std::begin(m_sourceGeometry.placement)) &&
      base.scalingMethod == m_spatialMethod;
  if (m_geometryKnown && !sameGeometry)
    m_geometryRevision = DVBRIDGE::NextRevisionAfter(std::max(preferences.revision,base.binding.revision));
  m_sourceGeometry = geometry;
  m_geometryKnown = true;
  uint64_t revision = std::max({preferences.revision,base.binding.revision,m_geometryRevision});
  bool masterOverlayRefresh = false;
  if (!m_pending && m_hasCommitted && base.binding == m_committedBinding &&
      revision == m_committedRevision && (!allowPacked || !m_committedPacked ||
          m_nativeOverlayRevision != m_committedOverlayRevision))
  {
    masterOverlayRefresh = policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE &&
        sameGeometry && allowPacked && m_committedPacked;
    // Explicit refresh of an equal-PTS source is a new processing transaction.
    revision = m_geometryRevision = DVBRIDGE::NextRevisionAfter(revision);
    if (!revision) return FrameStatus::Failed;
  }
  if (m_failedIdentity.stream == base.binding.stream &&
      m_failedIdentity.picture == base.binding.picture && m_failedIdentity.revision == revision)
    return FrameStatus::Failed;
  if (m_pending)
  {
    const bool sameTransaction = base.binding == m_pendingBinding &&
        revision == m_pendingIdentity.revision && policy.mode == m_effectiveMode &&
        (allowPacked || !m_direct);
    // GUI composition follows analysis; retain source work across GUI changes.
    if (sameTransaction && m_direct && m_dvPolicyActive &&
        m_effectiveMode == DVBRIDGE_MODE_ENHANCED_DV && m_pendingMetadata.Data() &&
        (m_pendingOverlayRevision != m_nativeOverlayRevision || m_pendingOverlayVisible != m_overlayVisible) &&
        dvbridge_render_dv_policy_update_overlay(m_renderer,
            m_nativeOverlayEnabled ? &m_nativeOverlay : nullptr,m_overlayVisible))
    {
      m_pendingOverlayRevision = m_nativeOverlayRevision;
      m_pendingOverlayVisible = m_overlayVisible;
    }
    if (sameTransaction &&
        m_pendingOverlayVisible == m_overlayVisible &&
        (!m_direct || m_pendingOverlayRevision == m_nativeOverlayRevision))
      return PollReady() ? FrameStatus::Ready : FrameStatus::Pending;
    Cancel();
  }
  const bool eligible = allowAsync && asyncSetting && !fallback && !m_fallback && !m_pendingHDR10Fallback &&
      policy.mode != DVBRIDGE_MODE_DISABLED && m_hasCommitted && sameGeometry &&
      base.lease && (!enhancement || enhancement->lease) &&
      (m_directUsable || policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE) &&
      m_committedMode == policy.mode && base.binding.stream == m_committedBinding.stream &&
      base.binding.revision == m_committedBinding.revision &&
      (revision == m_committedRevision || masterOverlayRefresh);
  if (m_dvPolicyActive)
  {
    const auto status = eligible && allowPacked ?
        dvbridge_render_dv_policy_advance(m_renderer) : dvbridge_render_dv_policy_poll(m_renderer);
    if (status != DVBRIDGE_DV_IDLE)
      return status == DVBRIDGE_DV_FAILED ? FrameStatus::Failed : FrameStatus::Busy;
    m_dvPolicyActive = false;
    m_context->SourceReadComplete();
  }
  const auto retired = m_context->Poll(eligible);
  if (retired != DVBRIDGE_DV_IDLE)
    return retired == DVBRIDGE_DV_FAILED ? FrameStatus::Failed : FrameStatus::Busy;
  if (m_hasCommitted && allowPacked && m_committedPacked &&
      base.binding == m_committedBinding && revision == m_committedRevision &&
      policy.mode == m_committedMode && policy.mode == m_effectiveMode)
    return FrameStatus::Reused;
  m_live = true;
#ifdef CB1_HAS_HDR10_AI
  m_aiFrame = m_aiOutputAccepted = false;
#endif
  m_guiComposed = false;
  m_pendingOverlayRevision = m_nativeOverlayRevision;
  m_pendingOverlayVisible = m_overlayVisible;
  m_requested = preferences;
  m_spatialMethod = base.scalingMethod;
  m_context->SetSpatialMethod(m_spatialMethod, policy.mode == DVBRIDGE_MODE_MASTER_REFERENCE);
  policy.revision = revision;
  dvbridge_renderer_set_optimized_dv(m_renderer,preferences.optimizedDV);
  dvbridge_renderer_set_fel_filter(m_renderer, preferences.felUpscaling == 2 ? DVBRIDGE_FEL_REFERENCE : preferences.felUpscaling == 1 || (enhancement && enhancement->layout != DVBRIDGE_Y416) ? DVBRIDGE_FEL_BILINEAR : DVBRIDGE_FEL_DEFAULT);
  dvbridge_renderer_set_processing_optimizations(m_renderer,preferences.processingOptimizations);
  dvbridge_renderer_set_overlay_visible(m_renderer,m_overlayVisible);
  m_fallback = fallback;
  m_asyncEligible = eligible;
  const auto previousMode=m_effectiveMode;
  m_effectiveMode = policy.mode;
  m_hdr10plusMetadata.reset();
  if (policy.mode == DVBRIDGE_MODE_HDR10PLUS) m_pendingHDR10Fallback = 0;
  if (policy.mode == DVBRIDGE_MODE_DISABLED) return FrameStatus::Unsupported;
  m_pendingBinding = base.binding;
  m_pendingSourceInfo = base.sourceInfo;
  m_pendingIdentity = {base.binding.stream,base.binding.picture,revision};
  DVBRIDGE::StreamTelemetry::NativeRequest({base.binding.stream,base.binding.picture,revision},
      DVBRIDGE::PolicyCode(preferences.Policy()),DVBRIDGE::PolicyCode(policy),
      admissionIdentity.Valid() ? admissionIdentity :
          DVBRIDGE::SourceBinding{base.binding.stream,base.binding.picture,preferences.revision});
  if (m_auditStream!=base.binding.stream || m_auditSource!=base.binding.revision ||
      m_auditPolicy!=preferences.revision || previousMode!=m_effectiveMode)
  {
    m_auditStream=base.binding.stream; m_auditSource=base.binding.revision;
    m_auditPolicy=preferences.revision;
    if (++m_auditSerial==std::numeric_limits<int>::max())
    {
      m_auditSerial=1;
      m_auditRequestedSerial=m_auditPreparedSerial=m_auditCommittedSerial=0;
    }
  }
  // A phase remains pending across failed/superseded pictures and exhausted
  // log budgets. Only its emitted report consumes this source/settings epoch.
  if (m_auditRequestedSerial!=m_auditSerial && DVBRIDGE::DiagnosticsEnabled() &&
      m_auditRequested.Record(DVBRIDGE::DiagnosticNowMs(),"requested",m_auditSerial))
  {
    m_auditRequestedSerial=m_auditSerial;
    DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge policy requested: source={}/{}/{} policy={} requested={} effective={} eligible={} fallback={} l2_trims_disabled={} method={} tv_peak={} tv_panel={} tv_gamut={} tv_profile_valid={}",
        base.binding.stream,base.binding.picture,base.binding.revision,revision,
        int(preferences.Policy().mode),int(policy.mode),!m_fallback,m_fallback,preferences.L2TrimsDisabled(policy.mode),
        preferences.method,preferences.peak,preferences.panel,preferences.gamut,preferences.ProfileValid());
    const auto admission=DVBRIDGE::StreamTelemetry::Read().transaction;
    DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge output admission: generation={} source_known={} gpu_known={} native_dv={} hdr10={} requested={} effective={} reason={}",
        admission.capabilities.generation,admission.capabilities.sourceKnown,admission.capabilities.processingKnown,
        admission.capabilities.nativeDV,admission.capabilities.hdr10,
        CDVPlaybackInfo::RequestedPolicy(admission.requested),CDVPlaybackInfo::RequestedPolicy(admission.effective),
        CDVPlaybackInfo::Reason(admission.reason));
  }
  m_advanced = policy.mode == DVBRIDGE_MODE_HDR10_EXPERT || policy.mode == DVBRIDGE_MODE_ENHANCED_DV;
  m_context->Retain(base.lease, enhancement ? enhancement->lease : nullptr);
  if (!m_advanced)
  {
    const bool prepared = Prepare(base,enhancement,metadata,geometry,allowPacked);
    const auto hdr10plus = prepared && policy.mode == DVBRIDGE_MODE_HDR10PLUS ?
        BeginHDR10Plus() : FrameStatus::Ready;
    const bool fenced = m_context->FenceLastUse();
    m_pending = prepared && fenced &&
        (hdr10plus == FrameStatus::Ready || hdr10plus == FrameStatus::Pending);
    if (m_pending && m_prepared) PreparedAudit();
    if (!fenced || !prepared) return FrameStatus::Failed;
    return hdr10plus;
  }
  if (!base.lease || (enhancement && !enhancement->lease))
  { Failure("policy-source-lease"); return FrameStatus::Failed; }
  dvbridge_gl_frame bl{}, el{};
  if (!ImportDVLayer(&bl,m_gl->gpu,base))
  { Failure("policy-base-import"); return FrameStatus::Failed; }
  if (enhancement)
  {
    if (!ImportDVLayer(&el,m_gl->gpu,*enhancement))
    {
      dvbridge_gl_frame_release(&bl);
      Failure("policy-enhancement-import");
      return FrameStatus::Failed;
    }
    bl.frame.enhancement_layer = &el.frame;
  }
  const double elPts = enhancement ? enhancement->pts : base.pts;
  FrameStatus result = FrameStatus::Failed;
  m_prepared = m_packed = m_direct = false;
  if (policy.mode == DVBRIDGE_MODE_ENHANCED_DV)
  {
    auto status = DVBRIDGE_DV_UNSUPPORTED;
    if (allowPacked && m_directUsable && m_scanout && !geometry.x && !geometry.y &&
        geometry.width == geometry.target_width && geometry.height == geometry.target_height)
    {
      status = dvbridge_render_dv_policy_prepare_packed(m_renderer,&policy,&m_pendingIdentity,
          &bl.frame,metadata.Data(),metadata.Size(),base.pts,elPts,geometry,m_scanout,true,
          m_nativeOverlayEnabled ? &m_nativeOverlay : nullptr);
      m_direct = status == DVBRIDGE_DV_READY || status == DVBRIDGE_DV_PENDING;
      if (m_direct && m_nativeOverlayEnabled) m_nativeOverlayPrimed = true;
      if (status == DVBRIDGE_DV_UNSUPPORTED)
      {
        m_directUsable = false;
        m_asyncEligible = false;
        // Drain admitted packed work before reusing RGB fallback resources.
        const auto policyRetired = dvbridge_render_dv_policy_poll(m_renderer);
        m_dvPolicyActive = policyRetired != DVBRIDGE_DV_IDLE;
        const auto sourceRetired = m_context->Poll();
        if (policyRetired != DVBRIDGE_DV_IDLE || sourceRetired != DVBRIDGE_DV_IDLE)
        {
          dvbridge_gl_frame_release(&el);
          dvbridge_gl_frame_release(&bl);
          m_context->SourceReadComplete();
          return policyRetired == DVBRIDGE_DV_FAILED || sourceRetired == DVBRIDGE_DV_FAILED ?
              FrameStatus::Failed : FrameStatus::Busy;
        }
      }
    }
    if (status == DVBRIDGE_DV_UNSUPPORTED)
      status = dvbridge_render_dv_policy_prepare(m_renderer,&policy,&m_pendingIdentity,
          &bl.frame,metadata.Data(),metadata.Size(),base.pts,elPts,geometry);
    m_dvPolicyActive = status == DVBRIDGE_DV_PENDING || status == DVBRIDGE_DV_READY ||
        status == DVBRIDGE_DV_BUSY || status == DVBRIDGE_DV_FAILED;
    m_pending = status == DVBRIDGE_DV_PENDING || status == DVBRIDGE_DV_READY;
    // Ready output can be resolved now; retain decoder surfaces through GPU use.
    const bool fenced = m_context->FenceLastUse();
    m_prepared = status == DVBRIDGE_DV_READY && fenced;
    result = status == DVBRIDGE_DV_UNSUPPORTED ? FrameStatus::Unsupported :
        status == DVBRIDGE_DV_BUSY ? FrameStatus::Busy :
        m_prepared ? FrameStatus::Ready : m_pending ? FrameStatus::Pending : FrameStatus::Failed;
    if (!fenced) { Cancel(); result = FrameStatus::Failed; }
  }
  else
  {
    if (allowPacked && m_directUsable && m_scanout && !geometry.x && !geometry.y &&
        geometry.width == geometry.target_width && geometry.height == geometry.target_height)
    {
      m_direct = dvbridge_render_hdr10_policy_direct(m_renderer,&policy,&m_pendingIdentity,
          &bl.frame,metadata.Data(),metadata.Size(),base.pts,elPts,geometry,m_scanout,
          true,m_hdr10Limited,m_outputBits);
      if (!m_direct)
      {
        Failure("hdr10-policy-direct-unavailable-using-composition");
        m_directUsable = false;
        m_asyncEligible = false;
        dvbridge_render_policy_cancel(m_renderer);
      }
    }
    m_prepared = m_direct || dvbridge_render_hdr10_policy_rgb(m_renderer,&policy,&m_pendingIdentity,
        &bl.frame,metadata.Data(),metadata.Size(),base.pts,elPts,geometry);
    const bool fenced = m_context->FenceLastUse();
    m_pending = m_prepared && fenced;
    result = m_pending ? FrameStatus::Ready : FrameStatus::Failed;
  }
  dvbridge_gl_frame_release(&el); dvbridge_gl_frame_release(&bl);
  if (m_pending)
  {
    m_pendingMetadata = metadata;
    dvbridge_color color{};
    const bool fel = enhancement && dvbridge_map_color(&color,metadata.Data(),metadata.Size(),true) &&
        color.dovi.nlq_active;
    CaptureMetadata(metadata,fel);
  }
  if (result == FrameStatus::Ready) PreparedAudit();
  if (result == FrameStatus::Failed) { m_failedIdentity = m_pendingIdentity; Failure("policy-prepare"); }
  return result;
}

void CDVBridgeGLES::Cancel()
{
  cb1_hdr10plus_analysis_reset(m_hdr10plusAnalysis);
  m_hdr10plusMetadata.reset();
  ClearFELPrefetch();
#ifdef CB1_HAS_HDR10_AI
  if (m_context) m_context->ClearNextHDR10AI();
  m_aiOutputAccepted = false;
  m_aiGuidance.reset();
#endif
  if (m_dvPolicyActive) dvbridge_render_dv_policy_cancel(m_renderer);
  else dvbridge_render_policy_cancel(m_renderer);
  m_prepared = m_packed = m_pending = false;
  m_submission = {};
  m_asyncEligible = false;
}

bool CDVBridgeGLES::PollReady()
{
  return PollStatus(0) == FrameStatus::Ready;
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::PollStatus(uint64_t timeoutNs)
{
  if (!m_pending) return FrameStatus::Failed;
  DVBRIDGE::ObserveDiagnostic(m_pendingBinding.stream,m_pendingBinding.picture,
      m_pendingBinding.revision,m_requested.revision,"gpu-pending",true,false);
  if (m_effectiveMode == DVBRIDGE_MODE_HDR10PLUS && !m_prepared)
    return PollHDR10Plus();
  if (!m_dvPolicyActive) return m_prepared ? FrameStatus::Ready : FrameStatus::Pending;
  const auto status = dvbridge_render_dv_policy_wait(m_renderer,timeoutNs);
  if (status == DVBRIDGE_DV_READY)
  {
    m_context->SourceReadComplete();
    m_prepared = true;
    PreparedAudit();
    return FrameStatus::Ready;
  }
  if (status == DVBRIDGE_DV_FAILED)
  {
    m_failedIdentity = m_pendingIdentity;
    Failure("policy-analysis");
    Cancel();
    return FrameStatus::Failed;
  }
  return FrameStatus::Pending;
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::BeginHDR10Plus()
{
  m_prepared = false;
  m_hdr10plusMetadata.reset();
  if (!m_hdr10plusAnalysis)
    m_hdr10plusAnalysis = cb1_hdr10plus_analysis_create(m_log, m_gl->gpu);
  const auto rgb = dvbridge_render_texture(m_renderer);
  unsigned margins[4];
  if (m_hdr10plusAnalysis && rgb && dvbridge_frame_area(margins,
      m_pendingMetadata.Data(), m_pendingMetadata.Size(), m_sourceGeometry))
  {
    const pl_rect2d area{static_cast<int>(margins[0]), static_cast<int>(margins[2]),
        rgb->params.w - static_cast<int>(margins[1]), rgb->params.h - static_cast<int>(margins[3])};
    const auto status = cb1_hdr10plus_analysis_submit(m_hdr10plusAnalysis, rgb, area, m_pendingIdentity);
    if (status == CB1_HDR10PLUS_ANALYSIS_PENDING)
    {
      if (DVBRIDGE::DiagnosticsEnabled()) ++m_hdr10plusSubmits;
      return FrameStatus::Pending;
    }
    if (status == CB1_HDR10PLUS_ANALYSIS_BUSY) return FrameStatus::Busy;
  }
  m_failedIdentity = m_pendingIdentity;
  Failure("hdr10plus-analysis-submit");
  Cancel();
  return FrameStatus::Failed;
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::PollHDR10Plus()
{
  cb1_hdr10plus_statistics statistics{};
  const auto status = cb1_hdr10plus_analysis_poll(m_hdr10plusAnalysis, m_pendingIdentity, &statistics);
  if (status == CB1_HDR10PLUS_ANALYSIS_PENDING)
  {
    ReportHDR10Plus();
    return FrameStatus::Pending;
  }
  if (status == CB1_HDR10PLUS_ANALYSIS_READY && DVBRIDGE::DiagnosticsEnabled())
  {
    Crc32 fingerprint;
    fingerprint.Compute(reinterpret_cast<const char*>(statistics.histogram),sizeof(statistics.histogram));
    fingerprint.Compute(reinterpret_cast<const char*>(&statistics.average_maxrgb),sizeof(statistics.average_maxrgb));
    fingerprint.Compute(reinterpret_cast<const char*>(statistics.maxscl),sizeof(statistics.maxscl));
    m_hdr10plusAnalyzed.Observe({m_pendingIdentity.stream,m_pendingIdentity.picture,m_pendingIdentity.revision},fingerprint);
    m_hdr10plusLight={statistics.average_maxrgb,statistics.maxscl[0],statistics.maxscl[1],statistics.maxscl[2]};
  }
  AVDynamicHDRPlus metadata{};
  if (status != CB1_HDR10PLUS_ANALYSIS_READY || !cb1_dv_hdr10plus_metadata_for_version(&metadata,
      &m_hdr10plusReport, m_pendingMetadata.Data(), m_pendingMetadata.Size(), &statistics,
      m_hdr10plusVersion))
  {
    m_failedIdentity = m_pendingIdentity;
    Failure("hdr10plus-analysis");
    Cancel();
    return FrameStatus::Failed;
  }
  m_hdr10plusMetadata = metadata;
  ReportHDR10Plus();
  m_prepared = true;
  PreparedAudit();
  return FrameStatus::Ready;
}

void CDVBridgeGLES::ObserveHDR10PlusSubmission(const Submission& submission) const
{
  if (!DVBRIDGE::DiagnosticsEnabled() || !submission.hdr10plus) return;
  Crc32 fingerprint;
  fingerprint.Compute(reinterpret_cast<const char*>(submission.hdr10plus->data()),submission.hdr10plus->size());
  m_hdr10plusGenerated.Observe({submission.identity.stream,submission.identity.picture,submission.identity.revision},fingerprint);
}

void CDVBridgeGLES::ReportHDR10Plus()
{
  const auto now=DVBRIDGE::DiagnosticNowMs();
  if (!DVBRIDGE::DiagnosticsEnabled() || !m_hdr10plusDiagnostics.Due(now)) return;
  const auto& a=m_hdr10plusAnalyzed;
  const auto& g=m_hdr10plusGenerated;
  const auto& p=m_hdr10plusAccepted;
  if (DVBRIDGE::DiagnosticProgressLog(LOGINFO,
      "CB1 HDR10+ progress: submitted={} analyzed={} stats_changes={} generated={} packet_changes={} accepted={} accepted_changes={} analysis={}/{}/{} packet={}/{}/{} accepted_source={}/{}/{} stats_crc={:#x} packet_crc={:#x} accepted_crc={:#x} avg_maxrgb={} maxscl={},{},{} version={} coverage={} reason={}",
      m_hdr10plusSubmits,a.count,a.changes,g.count,g.changes,p.count,p.changes,
      a.identity[0],a.identity[1],a.identity[2],g.identity[0],g.identity[1],g.identity[2],
      p.identity[0],p.identity[1],p.identity[2],a.crc,g.crc,p.crc,
      m_hdr10plusLight[0],m_hdr10plusLight[1],m_hdr10plusLight[2],m_hdr10plusLight[3],
      m_hdr10plusVersion,int(m_hdr10plusReport.coverage),int(m_hdr10plusReport.reason)))
    m_hdr10plusDiagnostics.Emitted(now);
}

const dvbridge_hdr10_policy_output* CDVBridgeGLES::HDR10Output() const
{
  return m_advanced && !m_dvPolicyActive ? dvbridge_render_policy_output(m_renderer) : nullptr;
}

void CDVBridgeGLES::CaptureMetadata(const CDVMetadataBuffer& metadata, bool fel)
{
  const auto* parsed = reinterpret_cast<const AVDOVIMetadata*>(metadata.Data());
  m_pendingMetadata = metadata;
  m_pendingInfo = (m_hdr10 ? CDVPlaybackInfo::HDR10 : CDVPlaybackInfo::HDMI) |
      (fel ? CDVPlaybackInfo::FEL : 0u);
  for (int i=0;i<parsed->num_ext_blocks;i++)
  {
    const auto level = av_dovi_get_ext(parsed,i)->level;
    if (level == 3 || level == 8 || level == 9 || level == 10 || level == 11 || level == 254)
      m_pendingInfo |= CDVPlaybackInfo::CM4;
  }
}

unsigned int CDVBridgeGLES::GetPQTexture() const
{
  const auto* dv = m_advanced && m_dvPolicyActive ?
      dvbridge_render_dv_policy_output(m_renderer) : nullptr;
  const auto texture = m_prepared ? (dv ? dvbridge_render_dv_policy_rgb(m_renderer) : dvbridge_render_texture(m_renderer)) : nullptr;
  return texture ? pl_opengl_unwrap(m_gl->gpu, texture, nullptr, nullptr, nullptr) : 0;
}

bool CDVBridgeGLES::ReadScreenshot(KODI::RENDERING::CAPTURE::ReadbackBuffer& out)
{
  using namespace KODI::RENDERING::CAPTURE;
  const bool master=m_effectiveMode==DVBRIDGE_MODE_MASTER_REFERENCE;
  if (!m_prepared || !m_scanout || (master && !m_direct)) return false;
  const auto framebuffer=master ? m_scanoutFramebuffer : GetPQFramebuffer();
  if (!master && !framebuffer) return false;
  GLint previous=0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&previous);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);
  ReadbackBuffer pixels;
  const bool read=ReadFramebufferRegion(0,0,m_scanout->params.w,m_scanout->params.h,master ? 8 : 16,master,pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,previous);
  if (!read) return false;
  if (master)
  {
    dvbridge_color mapped{};
    if (pixels.format!=AV_PIX_FMT_RGBA || !dvbridge_map_color(&mapped,
        m_pendingMetadata.Data(),m_pendingMetadata.Size(),(m_pendingInfo&CDVPlaybackInfo::FEL)!=0))
      return false;
    pl_color_repr_normalize(&mapped.repr);
    const auto tr=pl_color_repr_decode(&mapped.repr,nullptr);
    pl_matrix3x3 linear={{{3.06441879f,-2.16597676f,.10155818f},
        {-.65612108f,1.78554118f,-.12943749f},{.01736321f,-.04725154f,1.03004253f}}};
    pl_matrix3x3_mul(&linear,&mapped.dovi.linear);
    float decode[12];
    for (unsigned i=0;i<3;i++)
    {
      for (unsigned j=0;j<3;j++) decode[4*i+j]=tr.mat.m[i][j];
      decode[4*i+3]=tr.c[i];
    }
    auto rgb=std::make_unique<uint8_t[]>(size_t(pixels.width)*pixels.height*8);
    if (!DVBRIDGE::DecodeReferenceScreenshot(pixels.pixels.get(),pixels.width,pixels.height,
        decode,&linear.m[0][0],reinterpret_cast<uint16_t*>(rgb.get()))) return false;
    pixels.pixels=std::move(rgb);
    pixels.stride=-int(pixels.width*8);
    pixels.format=AV_PIX_FMT_RGBA64LE;
  }
  out=std::move(pixels);
  return true;
}

bool CDVBridgeGLES::PrepareHardware(const Layer& base, const AVFrame* enhancement,
                                  const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked)
{
  m_prepared = m_packed = false;
  if (!enhancement)
    return Prepare(base, nullptr, metadata, geometry, allowPacked);
  dvbridge_dmabuf imported{};
  if (!dvbridge_dmabuf_import(&imported, enhancement))
    return Failure("enhancement-dmabuf-import");
  Layer layer{};
  layer.layout = imported.layout;
  layer.width = enhancement->width;
  layer.height = enhancement->height;
  layer.pts = static_cast<double>(enhancement->pts);
  layer.chromaLocation = enhancement->chroma_location == AVCHROMA_LOC_UNSPECIFIED ?
      AVCHROMA_LOC_TOPLEFT : enhancement->chroma_location;
  for (int i = 0; i < 3; ++i)
    layer.planes[i] = imported.planes[i];
  bool result = Prepare(base, &layer, metadata, geometry, allowPacked);
  dvbridge_dmabuf_release(&imported);
  return result;
}

CDVBridgeGLES::FrameStatus CDVBridgeGLES::PrepareHardwareFrame(const Layer& base,
    std::shared_ptr<AVFrame> enhancement, DVBRIDGE::SourceBinding enhancementBinding,
    const CDVMetadataBuffer& metadata, dvbridge_geometry geometry,
    const DVBRIDGE::OutputPreferences& preferences, bool allowPacked, const dvbridge_policy* selected,
    bool allowAsync)
{
  if (!enhancement)
  {
    ClearFELPrefetch();
    return PrepareFrame(base,nullptr,metadata,geometry,preferences,allowPacked,selected,allowAsync);
  }
  if (!enhancementBinding.Valid() || enhancementBinding != base.binding ||
      !std::isfinite(base.pts) || static_cast<double>(enhancement->pts) != base.pts)
  {
    if (m_submission.asyncEligible) m_submission = {};
    m_asyncEligible = false;
    return FrameStatus::Failed;
  }
  BeginDiagnosticStream(base.binding.stream);
  struct Import
  {
    struct Raw
    {
      dvbridge_dmabuf_key key{};
      dvbridge_dmabuf image{};
      AVBufferRef* pool{};
      ~Raw() { dvbridge_dmabuf_release(&image); av_buffer_unref(&pool); }
    };
    dvbridge_dmabuf image{};
    std::shared_ptr<Raw> raw;
    std::shared_ptr<AVFrame> frame;
    std::shared_ptr<dvbridge_fel_vpp> vpp;
    const dvbridge_fel_vpp_image* scaled{};
    ~Import()
    {
      if (scaled) dvbridge_fel_vpp_release(vpp.get(),scaled);
      else dvbridge_dmabuf_release(&image);
    }
  };
  std::shared_ptr<Import> imported;
  const auto mode = (selected ? *selected : preferences.Policy()).mode;
  const bool master = mode == DVBRIDGE_MODE_MASTER_REFERENCE;
  const bool prefetchEnabled = allowAsync && allowPacked && m_context &&
      m_context->AsyncPreparation(base.binding.stream) &&
      mode == DVBRIDGE_MODE_ENHANCED_DV &&
      preferences.felUpscaling == 0;
  if (!prefetchEnabled) ClearFELPrefetch();
  const int effectiveFilter = master ? 2 : preferences.felUpscaling;
  const auto chroma = SourceChroma(enhancement->chroma_location,7);
  if (m_hardwareBinding == enhancementBinding && m_hardwareLease &&
      m_hardwareFELFilter == effectiveFilter && m_hardwareFELChroma == chroma)
    imported = std::static_pointer_cast<Import>(m_hardwareLease);
  else
  {
    imported = std::make_shared<Import>();
    imported->frame = enhancement;
    if (prefetchEnabled && TakeFELPrefetch(enhancement,enhancementBinding,preferences,
        imported->vpp,imported->scaled) == FrameStatus::Failed)
      return FrameStatus::Failed;
    if (m_hardwareFELFilter != effectiveFilter)
      DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 FEL filter selection: requested={} backend={}",
          master || preferences.felUpscaling == 2 ? "reference" : preferences.felUpscaling == 1 ? "bilinear" : "vaapi",
          master || preferences.felUpscaling == 2 ? "reference-gpu" : preferences.felUpscaling == 1 ? "shader" : "vaapi-vpp");
    if (m_felVppStream != base.binding.stream)
    {
      m_rawELCache.clear();
      m_felVppStream = base.binding.stream;
      m_felVppEnabled = m_context && m_context->FELHardwareUpscaling(base.binding.stream);
      m_felVppReported = false;
      m_felVpp.reset();
    }
    dvbridge_color color{};
    if (!imported->scaled && !master && preferences.felUpscaling == 0 && m_felVppEnabled && m_gl && enhancement->hw_frames_ctx &&
        dvbridge_map_color(&color,metadata.Data(),metadata.Size(),true) && color.dovi.nlq_active &&
        !(base.width & 1) && !(base.height & 1) &&
        enhancement->width == base.width / 2 && enhancement->height == base.height / 2)
    {
      // Existing source-read fences retain leases until either image is reusable.
      if (m_context) m_context->Poll(true);
      m_hardwareLease.reset();
      if (!m_felVpp)
      {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(enhancement->hw_frames_ctx->data);
        m_felVpp = std::shared_ptr<dvbridge_fel_vpp>(
            dvbridge_fel_vpp_create(frames->device_ref,base.width,base.height),
            [](dvbridge_fel_vpp* owner) { dvbridge_fel_vpp_destroy(&owner); });
      }
      auto status = m_felVpp ? dvbridge_fel_vpp_acquire_chroma(m_felVpp.get(),enhancement.get(),chroma,
          &imported->scaled) : DVBRIDGE_VPP_FAILED;
      if (status == DVBRIDGE_VPP_BUSY && prefetchEnabled) return FrameStatus::Busy;
      if (status == DVBRIDGE_VPP_OK && !m_felVppReported)
      {
        dvbridge_gl_frame probe{};
        const dvbridge_gl_plane planes[3]{imported->scaled->plane};
        if (!dvbridge_gl_frame_import(&probe,m_gl->gpu,DVBRIDGE_Y416,
            base.width,base.height,planes))
        {
          dvbridge_fel_vpp_release(m_felVpp.get(),imported->scaled);
          imported->scaled = nullptr;
          status = DVBRIDGE_VPP_FAILED;
        }
        dvbridge_gl_frame_release(&probe);
      }
      if (status == DVBRIDGE_VPP_OK)
      {
        imported->vpp = m_felVpp;
        if (!m_felVppReported)
        {
          m_felVppReported = true;
          DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 FEL upscaling: VAAPI {}x{} to {}x{} format=Y416, two leased surfaces, zero CPU pixel copies",
              enhancement->width,enhancement->height,base.width,base.height);
        }
      }
      else if (status == DVBRIDGE_VPP_FAILED)
      {
        m_felVppEnabled = false;
        DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 FEL upscaling: VAAPI unavailable, using bilinear GPU reconstruction");
      }
    }
    if (!imported->scaled)
    {
      if (!dvbridge_dmabuf_map(&imported->image,enhancement.get()))
      {
        if (m_submission.asyncEligible) m_submission = {};
        m_asyncEligible = false;
        return FrameStatus::Failed;
      }
      dvbridge_dmabuf_key key{};
      const bool cacheable = dvbridge_dmabuf_make_key(&key,
          reinterpret_cast<const AVDRMFrameDescriptor*>(imported->image.mapping->data[0]),
          imported->image.width,imported->image.height,enhancement->hw_frames_ctx->data,
          imported->image.display,eglGetCurrentContext());
      if (cacheable && !m_rawELImports)
        DVBRIDGE::DiagnosticLog(LOGINFO,
            "CB1 FEL DMA: source={}/{}/{} objects={} format={:#x}/{:#x} pitch={}/{} offset={}/{} modifier={:#x}/{:#x}",
            base.binding.stream,base.binding.picture,base.binding.revision,key.objects_count,
            key.layers[0][0],key.layers[1][0],key.layers[0][4],key.layers[1][4],key.layers[0][3],key.layers[1][3],
            key.objects[key.layers[0][2]][3],key.objects[key.layers[1][2]][3]);
      if (cacheable && !m_rawELCache.empty() &&
          std::static_pointer_cast<Import::Raw>(m_rawELCache.front())->key.pool != key.pool)
        m_rawELCache.clear();
      if (cacheable)
        for (const auto& entry : m_rawELCache)
        {
          auto raw = std::static_pointer_cast<Import::Raw>(entry);
          if (dvbridge_dmabuf_key_equal(&key,&raw->key))
          { imported->raw = std::move(raw); ++m_rawELHits; break; }
        }
      if (!imported->raw)
      {
        if (!dvbridge_dmabuf_import_mapped(&imported->image))
        {
          if (m_submission.asyncEligible) m_submission = {};
          m_asyncEligible = false;
          return FrameStatus::Failed;
        }
        auto raw = std::make_shared<Import::Raw>();
        raw->key = key;
        raw->image = imported->image;
        raw->image.mapping = nullptr;
        raw->pool = av_buffer_ref(enhancement->hw_frames_ctx);
        std::fill(std::begin(imported->image.images),std::end(imported->image.images),nullptr);
        std::fill(std::begin(imported->image.planes),std::end(imported->image.planes),dvbridge_gl_plane{});
        imported->raw = raw;
        ++m_rawELImports;
        if (cacheable && raw->pool)
        {
          if (m_rawELCache.size() == 32) m_rawELCache.erase(m_rawELCache.begin());
          m_rawELCache.push_back(std::move(raw));
        }
      }
    }
    m_hardwareLease = imported;
    m_hardwareFrame = enhancement;
    m_hardwareBinding = enhancementBinding;
    m_hardwareFELFilter = effectiveFilter;
    m_hardwareFELChroma = chroma;
    m_hardwareFELVpp = imported->scaled != nullptr;
  }
  Layer layer{};
  layer.layout = imported->scaled ? DVBRIDGE_Y416 : imported->raw->image.layout;
  layer.width = imported->scaled ? imported->scaled->width : enhancement->width;
  layer.height = imported->scaled ? imported->scaled->height : enhancement->height;
  layer.pts = static_cast<double>(enhancement->pts);
  layer.binding = enhancementBinding;
  layer.lease = imported;
  layer.chromaLocation = enhancement->chroma_location == AVCHROMA_LOC_UNSPECIFIED ?
      AVCHROMA_LOC_TOPLEFT : enhancement->chroma_location;
  if (imported->scaled)
  {
    layer.planes[0] = imported->scaled->plane;
  }
  else std::copy(std::begin(imported->raw->image.planes),std::end(imported->raw->image.planes),layer.planes);
  return PrepareFrame(base,&layer,metadata,geometry,preferences,allowPacked,selected,allowAsync);
}

bool CDVBridgeGLES::Pack(unsigned int framebuffer, int width, int height, bool flipY)
{
  m_packed = false;
  if (!m_prepared)
    return Failure("pack-without-prepared-frame");
  if (m_advanced && m_dvPolicyActive)
  {
    if (!m_scanout || framebuffer != m_scanoutFramebuffer ||
        width != m_scanout->params.w || height != m_scanout->params.h)
      return Failure("enhanced-pack-target");
    m_packed = dvbridge_render_dv_policy_resolve(m_renderer,m_scanout,m_scanoutFramebuffer,flipY);
    if (m_packed)
    {
      const auto* output = dvbridge_render_dv_policy_output(m_renderer);
      if (!output) return m_packed = false;
      m_submission = {m_pendingIdentity,m_scanout,output->resolve_serial,++m_submissionSequence,m_asyncEligible};
    }
    return m_packed;
  }
  if (m_direct)
  {
    m_packed = m_scanout && framebuffer == m_scanoutFramebuffer &&
        width == m_scanout->params.w && height == m_scanout->params.h && flipY;
    if (m_packed && m_live)
      m_submission = {m_pendingIdentity,m_scanout,0,++m_submissionSequence,
          m_asyncEligible && (m_directUsable || m_effectiveMode == DVBRIDGE_MODE_MASTER_REFERENCE)};
    return m_packed || Failure("direct-pack-target");
  }
  unsigned int count;
  const auto* packets = dvbridge_packets(dvbridge_render_candidate(m_renderer), &count);
  m_packed = dvbridge_gl_pack(m_packer, GetPQTexture(), framebuffer, width, height,
                             packets, count, flipY);
  if (m_packed && m_live)
    m_submission = {m_pendingIdentity,m_scanout,0,++m_submissionSequence,m_asyncEligible && m_directUsable};
  return m_packed || Failure("composition-pack");
}

bool CDVBridgeGLES::ResolveHDR10(bool limited)
{
  if (m_hdr10 && m_prepared && m_direct)
  {
    if (limited != m_hdr10Limited) return Failure("hdr10-output-range-changed");
    m_packed = true;
    if (m_live)
      m_submission = {m_pendingIdentity,m_scanout,0,++m_submissionSequence,
          m_asyncEligible && !m_pendingHDR10Fallback};
    return true;
  }
  if (m_advanced && !m_dvPolicyActive)
  {
    m_packed = m_hdr10 && m_prepared && dvbridge_render_hdr10_policy_resolve(
        m_renderer,m_scanout,true,limited,m_outputBits) && m_context->FenceLastUse();
    if (m_packed)
      m_submission = {m_pendingIdentity,m_scanout,0,++m_submissionSequence,m_asyncEligible};
    return m_packed;
  }
  m_packed = m_hdr10 && m_prepared &&
      dvbridge_render_hdr10(m_renderer, m_scanout, true, limited, m_outputBits);
  if (m_packed && m_live)
    m_submission = {m_pendingIdentity,m_scanout,0,++m_submissionSequence,m_asyncEligible && !m_pendingHDR10Fallback};
  return m_packed || Failure("hdr10-resolve");
}

unsigned int CDVBridgeGLES::GetPQFramebuffer()
{
  const auto texture = GetPQTexture();
  if (!texture)
    return 0;
  GLint previous;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
  if (!m_framebuffer)
    glGenFramebuffers(1, &m_framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  const bool valid = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous);
  if (valid && m_live) m_guiComposed = true;
  return valid ? m_framebuffer : 0;
}

void CDVBridgeGLES::CountPresentation(bool committed)
{
  if (committed) DVBRIDGE::DiagnosticPresented(m_pendingBinding.stream);
  if (!DVBRIDGE::DiagnosticsEnabled() || !m_prepared) return;
  m_presentations += committed;
  m_failedPresentations += !committed;
  if ((m_presentations + m_failedPresentations) % 120 == 0)
  {
    const auto now = DVBRIDGE::DiagnosticNowMs();
    if (now - m_lastSummary >= 30000)
    {
      m_lastSummary = now;
      Summary("summary");
    }
  }
}

bool CDVBridgeGLES::Presented(bool success)
{
  if (m_live) return Presented(success,m_submission);
  const bool committed = success && m_prepared && m_packed && dvbridge_render_commit(m_renderer);
  CountPresentation(committed);
  if (!committed && m_prepared) Failure("presentation");
  if (committed)
  {
    const float target = m_hdr10 && m_hdr10Session ? m_hdr10Session->target.hdr.max_luma : 0.0f;
    const bool level6 = m_hdr10 && m_hdr10Session && m_hdr10Session->output.level6;
    DVBRIDGE::StreamTelemetry::Presented(m_pendingInfo, m_outputBits, target, level6);
    // Immutable session policy is announced only on its first accepted frame.
    // Picture reset and changing CM/FEL metadata do not start a new session.
    if (m_hdr10 && DVBRIDGE::DiagnosticsEnabled() && m_presentations == 1)
      DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge presented: policy=Basic target={} nits origin={} creative_trims=not-applied framebuffer_bits={}",
          target, level6 ? "RPU-L6" : "RPU-source-PQ", m_outputBits);
    if (m_hdr10 && m_pendingHDR10Fallback != m_hdr10FallbackReason)
    {
      m_hdr10FallbackReason = m_pendingHDR10Fallback;
      if (DVBRIDGE::DiagnosticsEnabled())
      {
        const auto now = DVBRIDGE::DiagnosticNowMs();
        const char* reason = m_hdr10FallbackReason == 1 ? "missing-L1" : "unusable-L1";
        if (m_hdr10FallbackReason && m_hdr10Fallbacks.Record(now, reason))
          DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge Basic HDR10: presented static fallback reason={}", reason);
        else if (!m_hdr10FallbackReason && m_hdr10Fallbacks.Recovered(now))
          DVBRIDGE::DiagnosticLog(LOGINFO, "DVBridge Basic HDR10: presented L1 mapping recovered");
      }
    }
  }
  if (!committed)
    dvbridge_renderer_reset(m_renderer);
  m_prepared = m_packed = false;
  m_direct = false;
  return committed;
}

bool CDVBridgeGLES::Presented(bool success, const Submission& submission)
{
  if (!m_live) return Presented(success);
  // A delayed callback must not consume or cancel a newer resolve.
  if (!submission.sequence || submission.sequence != m_submission.sequence ||
      submission.target != m_submission.target ||
      submission.resolveSerial != m_submission.resolveSerial ||
      submission.asyncEligible != m_submission.asyncEligible ||
      submission.identity.stream != m_pendingIdentity.stream ||
      submission.identity.picture != m_pendingIdentity.picture ||
      submission.identity.revision != m_pendingIdentity.revision)
    return false;
  const auto* hdrOutput = HDR10Output();
  const float peak = m_effectiveMode == DVBRIDGE_MODE_HDR10PLUS ? 992.0f :
      hdrOutput ? hdrOutput->nominal_target.hdr.max_luma :
      m_advanced && m_effectiveMode == DVBRIDGE_MODE_ENHANCED_DV ? static_cast<float>(m_requested.peak) :
      m_hdr10 && m_hdr10Session ? m_hdr10Session->target.hdr.max_luma : 0.0f;
  const bool committed = success && m_prepared && m_packed &&
      (m_advanced ? (m_dvPolicyActive ?
          dvbridge_render_dv_policy_commit(m_renderer,&submission.identity,
              submission.target,submission.resolveSerial) :
          dvbridge_render_policy_commit(m_renderer,&submission.identity,submission.target)) :
          dvbridge_render_commit(m_renderer));
  CountPresentation(committed);
  if (!committed)
  {
#ifdef CB1_HAS_HDR10_AI
    if (m_aiFrame) m_context->HDR10AI().Presented(m_pendingBinding,false);
#endif
    m_failedIdentity = submission.identity;
    DVBRIDGE::StreamTelemetry::NativeStage({submission.identity.stream,submission.identity.picture,
        submission.identity.revision},"Failed commit");
    DVBRIDGE::StreamTelemetry::PublishNativePending(submission.informationGeneration);
    Failure("presentation-commit");
    Cancel();
    return false;
  }
  m_hasCommitted = true;
  const int felKey=submission.felBackend*4+submission.felFilter;
  if ((m_reportedFELStream != submission.identity.stream || m_reportedFELKey != felKey) &&
      DVBRIDGE::DiagnosticLog(LOGINFO,
          "CB1 FEL presented: applicable={} requested={} executed={} backend={}",
          submission.felBackend != 0,
          submission.felFilter < 0 ? "not-applicable" : submission.felFilter == 2 ? "reference" : submission.felFilter == 1 ? "bilinear" : "vaapi",
          submission.felBackend == 3 ? "reference" : submission.felBackend == 0 ? "not-applicable" : submission.felBackend == 2 ? "vaapi" : "bilinear",
          submission.felBackend == 3 ? "reference-gpu" : submission.felBackend == 2 ? "vaapi-vpp" : submission.felBackend == 1 ? "shader" : "not-applicable"))
  {
    m_reportedFELStream=submission.identity.stream;
    m_reportedFELKey=felKey;
  }
#ifdef CB1_HAS_HDR10_AI
  if (m_aiFrame && !m_context->HDR10AI().Presented(m_pendingBinding,true))
  {
    m_context->ClearNextHDR10AI();
    return false;
  }
#endif
  m_committedBinding = m_pendingBinding;
  m_committedMode = m_effectiveMode;
  m_committedRevision = m_pendingIdentity.revision;
  m_committedOverlayRevision = m_pendingOverlayRevision;
  m_committedPacked = !m_guiComposed;
#ifdef CB1_HAS_HDR10_AI
  m_aiOutputAccepted = m_aiFrame;
#endif
  DVBRIDGE::StreamSnapshot::Output output{m_pendingInfo,m_outputBits,peak,
      m_hdr10 && !m_advanced && m_hdr10Session && m_hdr10Session->output.level6};
  output.requested = DVBRIDGE::PolicyCode(m_requested.Policy());
  output.felUpscaling = submission.felBackend == 3 ? 2 : submission.felBackend == 2 ? 0 : submission.felBackend == 1 ? 1 : -1;
#ifdef CB1_HAS_HDR10_AI
  if (m_aiFrame)
  {
    output.requested=static_cast<int>(m_requested.HDR10Policy().mode);
    const auto& prediction=m_context->HDR10AI().Committed();
    if (prediction)
    {
      output.hdr10AI=true;
      const auto& raw=prediction->raw;
      output.aiL1L3={raw.l1_min,raw.l1_max,raw.l1_avg,raw.l3_min,raw.l3_max,raw.l3_avg};
      if (m_aiGuidance)
      {
        output.hdr10PlusDV = true;
        output.guidedSourceFields = m_aiGuidance->source_fields;
        output.guidedEstimatedFields = m_aiGuidance->estimated_fields;
        output.untranslatedCurve = m_aiGuidance->untranslated_curve;
        output.untranslatedSaturation = m_aiGuidance->untranslated_saturation;
      }
    }
  }
#endif
  output.iptTransport = (output.flags & CDVPlaybackInfo::HDMI) &&
      (m_pendingSourceInfo.profile==5 || (m_effectiveMode==DVBRIDGE_MODE_MASTER_REFERENCE && m_requested.masterIPT));
  output.effective = static_cast<int>(m_effectiveMode);
  if (m_effectiveMode == DVBRIDGE_MODE_HDR10PLUS) output.effective = 7;
  if(m_effectiveMode==DVBRIDGE_MODE_ENHANCED_DV &&
#ifdef CB1_HAS_HDR10_AI
      !output.hdr10AI &&
#endif
      m_requested.output==DVBRIDGE::OutputMode::AdaptiveB)output.effective=6;
  output.method = m_requested.method;
  output.cmv40Appended = submission.cmv40Status == CB1_CMV40_APPEND;
  if (output.cmv40Appended) output.flags |= CDVPlaybackInfo::CM4;
  if (m_cmv40LogStream != submission.identity.stream)
  {
    m_auditCMv40 = {};
    m_cmv40LogStream = submission.identity.stream;
  }
  if (m_auditCMv40.Record(DVBRIDGE::DiagnosticNowMs(), "cmv40", int(submission.cmv40Status)))
    DVBRIDGE::DiagnosticLog(LOGINFO,
        "CB1 CM4 output: mode={} result={} source_cm4={} appended={} l2_removed={} tv_peak={} trigger={} headroom={} source={}/{}",
        m_requested.cmv40Mode, int(submission.cmv40Status), bool(m_pendingInfo & CDVPlaybackInfo::CM4),
        output.cmv40Appended, m_requested.L2TrimsDisabled(m_effectiveMode),
        m_requested.peak, m_requested.cmv40Trigger, m_requested.cmv40Headroom,
        submission.identity.stream, submission.identity.picture);
  output.l2TrimsDisabled = m_requested.L2TrimsDisabled(m_effectiveMode
#ifdef CB1_HAS_HDR10_AI
      , m_aiFrame
#endif
  );
  output.panel = m_requested.panel; output.gamut = m_requested.gamut;
  output.manualPeak = m_requested.peak; output.revision = m_pendingIdentity.revision;
  output.fallback = m_fallback;
  if (m_effectiveMode == DVBRIDGE_MODE_HDR10PLUS)
  {
    if (submission.hdr10plusAccepted)
    {
      if (DVBRIDGE::DiagnosticsEnabled() && submission.hdr10plus)
      {
        Crc32 fingerprint;
        fingerprint.Compute(reinterpret_cast<const char*>(submission.hdr10plus->data()),submission.hdr10plus->size());
        m_hdr10plusAccepted.Observe({submission.identity.stream,submission.identity.picture,submission.identity.revision},fingerprint);
      }
      output.flags |= CDVPlaybackInfo::HDR10PLUS;
      if (m_hdr10plusReport.coverage >= CB1_DV_HDR10PLUS_L1) output.creativeEdited |= 1u<<1;
      if (m_hdr10plusReport.coverage == CB1_DV_HDR10PLUS_L2) output.creativeEdited |= 1u<<2;
      if (m_hdr10plusReport.coverage == CB1_DV_HDR10PLUS_L8) output.creativeEdited |= 1u<<8;
      if (m_hdr10plusReport.l3_used && output.creativeEdited) output.creativeEdited |= 1u<<3;
    }
    else
    {
      output.fallback = true;
      output.reason = DVBRIDGE::OutputReason::NoHDR10PlusTransport;
    }
    if (m_auditCreative.Record(DVBRIDGE::DiagnosticNowMs(), "hdr10plus",
        int(m_hdr10plusReport.coverage)*32 + int(m_hdr10plusReport.reason)*2 + submission.hdr10plusAccepted))
    {
      Crc32 packet;
      if (submission.hdr10plus)
        packet.Compute(reinterpret_cast<const char*>(submission.hdr10plus->data()),submission.hdr10plus->size());
      DVBRIDGE::DiagnosticLog(LOGINFO,
          "CB1 HDR10+ output: active={} coverage={} reason={} l3={} curve_error_pq={} color_error_pq={} packet_present={} packet_crc={:#x} source={}/{}/{}",
          submission.hdr10plusAccepted, int(m_hdr10plusReport.coverage), int(m_hdr10plusReport.reason),
          m_hdr10plusReport.l3_used, m_hdr10plusReport.maximum_curve_pq_error,
          m_hdr10plusReport.maximum_color_pq_error,bool(submission.hdr10plus),uint32_t(packet),
          submission.identity.stream,submission.identity.picture,submission.identity.revision);
    }
    ReportHDR10Plus();
  }
  output.stream=m_pendingIdentity.stream; output.picture=m_pendingIdentity.picture;
  const char* pixelAlgorithm=nullptr;
  const char* analysisModel=nullptr;
  unsigned expansionReason=0;
  if (m_dvPolicyActive)
  {
    dvbridge_dv_policy_snapshot snapshot{};
    // Only the committed boundary can attest authored output and reduction data.
    if (dvbridge_render_dv_policy_committed(m_renderer,&snapshot) &&
        dvbridge_identity_equal(&snapshot.identity,&submission.identity))
    {
      output.authoredLegacy=snapshot.output_generation==DVBRIDGE_ETSI_LEGACY_LITERAL;
#ifdef CB1_HAS_HDR10_AI
      if (output.hdr10AI)
      {
        output.emittedL1Known=true;
        std::copy(std::begin(snapshot.l1),std::end(snapshot.l1),output.emittedL1.begin());
      }
#endif
      if (snapshot.output_generation==DVBRIDGE_RECONSTRUCTED_ENHANCED)
      {
        output.creativeKnown=true;
        output.creativeApplied=(1u<<1) | (snapshot.guidance_family==8 ? 1u<<8 :
            snapshot.guidance_family==2 ? 1u<<2 : 0);
        output.creativeStatus=DVBRIDGE_CREATIVE_READY;
#ifdef CB1_HAS_HDR10_AI
        output.emittedL1Known=true;
        std::copy(std::begin(snapshot.l1),std::end(snapshot.l1),output.emittedL1.begin());
#endif
      }
      else if (!output.authoredLegacy)
      {
        output.creativeKnown=true;
        output.creativeEdited=(snapshot.creative_edit.l2_coverage.edited ? 1u<<2 : 0) |
            (snapshot.creative_edit.l8_coverage.edited ? 1u<<8 : 0);
        output.creativeStatus=snapshot.creative_edit.status;
        output.creativeReason=snapshot.creative_edit.reason;
      }
      std::copy(std::begin(snapshot.raw_statistics),std::end(snapshot.raw_statistics),
          output.rawStatistics.begin());
      output.rawSum=snapshot.raw_sum; output.sampleCount=snapshot.sample_count;
      pixelAlgorithm=snapshot.pixel_algorithm;analysisModel=snapshot.analysis_model;
      expansionReason=snapshot.expansion_reason;
    }
  }
  if (m_advanced && m_effectiveMode==DVBRIDGE_MODE_HDR10_EXPERT)
  {
    dvbridge_hdr10_policy_output snapshot{};
    if (dvbridge_render_policy_committed(m_renderer,&snapshot) &&
        dvbridge_identity_equal(&snapshot.identity,&submission.identity))
    {
      output.creativeKnown=true; output.creativeApplied=snapshot.creative.applied_levels;
      output.creativeStatus=snapshot.creative.status; output.creativeReason=snapshot.creative.reason;
    }
  }
  DVBRIDGE::StreamTelemetry::NativePresented(m_pendingSourceInfo,m_pendingMetadata,output);
  DVBRIDGE::StreamTelemetry::PublishNativeInfo(submission.informationGeneration);
  const auto acceptedOutput=DVBRIDGE::StreamTelemetry::Read().output;
#ifdef CB1_HAS_HDR10_AI
  if (m_aiFrame && DVBRIDGE::DiagnosticsEnabled() && m_aiSummary.Due(DVBRIDGE::DiagnosticNowMs()))
  {
    m_aiSummary.Emitted(DVBRIDGE::DiagnosticNowMs());
    const auto metrics=m_context->HDR10AI().Metrics();
    DVBRIDGE::DiagnosticLog(LOGINFO,
        "CB1 AI summary: engine={} circuit={} bundle={} source={}/{}/{} pictures={} readback_bytes={} inferences={} analysis_submit_us={} readback_latency_us={} inference_us={} render_submit_us={}",
        cb1_engine_version(),acceptedOutput.Circuit(),cb1_l1l3_bundle_id(),
        m_committedBinding.stream,m_committedBinding.picture,m_committedBinding.revision,
        metrics.pictures,metrics.readback_bytes,metrics.inferences,
        metrics.analysis_submit_ns/1000,metrics.readback_latency_ns/1000,metrics.inference_ns/1000,
        m_aiRenderSubmitNs/1000);
    if (acceptedOutput.hdr10PlusDV)
      DVBRIDGE::DiagnosticLog(LOGINFO,
          "CB1 HDR10+ guidance: source_fields={} estimated_fields={} untranslated_curve={} untranslated_saturation={} source={}/{}/{}",
          acceptedOutput.guidedSourceFields, acceptedOutput.guidedEstimatedFields,
          acceptedOutput.untranslatedCurve, acceptedOutput.untranslatedSaturation,
          m_committedBinding.stream, m_committedBinding.picture, m_committedBinding.revision);
  }
#endif
  if (DVBRIDGE::DiagnosticsEnabled())
  {
    const auto now=DVBRIDGE::DiagnosticNowMs();
    const int creativeKey=acceptedOutput.effective*1024+
        acceptedOutput.creativeStatus*32+acceptedOutput.creativeReason;
    if (acceptedOutput.creativeKnown && m_auditCreative.Record(now,"creative",creativeKey))
      DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 treatment: mode={} source={}/{} status={} reason={} applied={} edited={}",
          acceptedOutput.effective,m_committedBinding.stream,m_committedBinding.picture,
          acceptedOutput.creativeStatus,acceptedOutput.creativeReason,
          acceptedOutput.creativeApplied,acceptedOutput.creativeEdited);
    if (m_auditCommittedSerial!=m_auditSerial && m_auditCommitted.Record(now,"committed",m_auditSerial))
    {
      m_auditCommittedSerial=m_auditSerial;
      if (pixelAlgorithm)
        DVBRIDGE::DiagnosticLog(LOGINFO,"CB1 Enhanced: algorithm={} analysis={} generation={} reason={}",
            pixelAlgorithm,analysisModel ? analysisModel : "source",acceptedOutput.sampleCount ? 3 : 2,expansionReason);
      DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge policy committed: source={}/{}/{} policy={} mode={} bits={} authored_legacy={} samples={} creative_status={} creative_reason={} applied={} edited={}",
          m_committedBinding.stream,m_committedBinding.picture,m_committedBinding.revision,
          m_committedRevision,acceptedOutput.effective,acceptedOutput.bits,acceptedOutput.authoredLegacy,acceptedOutput.sampleCount,
          acceptedOutput.creativeStatus,acceptedOutput.creativeReason,acceptedOutput.creativeApplied,acceptedOutput.creativeEdited);
      DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge output selection: requested={} effective={} reason={} minimum_link_bits={} profile={} bl={} el={} rpu={} hdr_origin={}",
          CDVPlaybackInfo::RequestedPolicy(acceptedOutput.requested,CDVPlaybackInfo::IsHDR10AI(acceptedOutput),CDVPlaybackInfo::IsHDR10PlusDV(acceptedOutput)),
          CDVPlaybackInfo::RequestedPolicy(acceptedOutput.effective,CDVPlaybackInfo::IsHDR10AI(acceptedOutput),CDVPlaybackInfo::IsHDR10PlusDV(acceptedOutput)),
          CDVPlaybackInfo::Reason(acceptedOutput.reason),acceptedOutput.minimumLinkBits,
          m_pendingSourceInfo.profile,m_pendingSourceInfo.bl,m_pendingSourceInfo.el,m_pendingSourceInfo.rpu,
          CDVPlaybackInfo::IsHDR10PlusDV(acceptedOutput) ?
              (acceptedOutput.guidedSourceFields & CB1_HDR10PLUS_DOVI_AVG ?
                  "source-HDR10+-statistics" : "source-HDR10+-and-AI-estimates") :
          acceptedOutput.effective == 7 ? "reconstructed-picture-statistics" :
          acceptedOutput.flags & CDVPlaybackInfo::HDR10 ? (acceptedOutput.effective>=3 ? "manual-tv-profile" :
              acceptedOutput.level6 ? "source-L6" : "reference-default") : "source-RPU");
    }
    if (m_failures.Recovered(now)) DVBRIDGE::DiagnosticLog(LOGINFO,"DVBridge renderer recovered: policy unchanged");
  }
  m_prepared = m_packed = m_pending = false;
  m_submission = {};
#ifdef CB1_HAS_HDR10_AI
  // The accepted page owns its immutable current fence; future commands follow it.
  if (m_aiFrame) m_context->AdvanceNextHDR10AI(m_committedBinding,
      DVBRIDGE::FrameOutputSettings(m_requested.hdr10Source == DVBRIDGE::HDR10SourceMode::HDR10PlusDV));
#endif
  return true;
}
