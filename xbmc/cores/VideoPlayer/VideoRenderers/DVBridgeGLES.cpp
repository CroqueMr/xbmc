/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DVBridgeGLES.h"
#include "DVBridgeShaders.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"
extern "C"
{
#include <libavutil/dovi_meta.h>
#include "dvbridge_placebo.h"
}

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <limits>

CDVBridgeGLES::~CDVBridgeGLES()
{
  CDVPlaybackInfo::presented.store(0);
  glDeleteFramebuffers(1, &m_framebuffer);
  dvbridge_gl_packer_destroy(m_packer);
  dvbridge_renderer_destroy(m_renderer);
  if (m_gl)
    pl_tex_destroy(m_gl->gpu, &m_scanout);
  pl_opengl_destroy(&m_gl);
}

bool CDVBridgeGLES::Initialize(bool allowSoftware)
{
  if (m_gl)
    return m_renderer && m_packer;
  pl_opengl_params params{};
  params.get_proc_addr = reinterpret_cast<pl_voidfunc_t (*)(const char*)>(eglGetProcAddress);
  params.egl_display = eglGetCurrentDisplay();
  params.egl_context = eglGetCurrentContext();
  params.allow_software = allowSoftware;
  if (params.egl_context == EGL_NO_CONTEXT)
    return false;
  m_gl = pl_opengl_create(nullptr, &params);
  if (!m_gl)
    return false;
  // GUI alpha blending takes place in the 32F PQ target, before transport packing.
  if (!pl_opengl_has_ext(m_gl, "GL_EXT_float_blend") ||
      !pl_opengl_has_ext(m_gl, "GL_OES_texture_float_linear"))
    return false;
  m_renderer = dvbridge_renderer_create(m_gl->gpu);
  pl_opengl_wrap_params output{};
  output.width = 3840;
  output.height = 2160;
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
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
      red == 8 && green == 8 && blue == 8 && samples <= 1 && encoding == GL_LINEAR)
    m_scanout = pl_opengl_wrap(m_gl->gpu, &output);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  m_packer = dvbridge_gl_packer_create(DVBRIDGE_VERTEX, DVBRIDGE_FRAGMENT);
  return m_renderer && m_packer;
}

void CDVBridgeGLES::Reset()
{
  CDVPlaybackInfo::presented.store(0);
  m_pendingInfo = 0;
  m_prepared = m_packed = false;
  m_direct = false;
  dvbridge_renderer_reset(m_renderer);
}

bool CDVBridgeGLES::Prepare(const Layer& base, const Layer* enhancement,
                          const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked)
{
  m_prepared = m_packed = false;
  m_direct = false;
  if (!m_renderer || !metadata.Data())
    return false;
  dvbridge_gl_frame bl{}, el{};
  if (!dvbridge_gl_frame_import(&bl, m_gl->gpu, base.layout, base.width, base.height, base.planes))
    return false;
  double elPts = std::numeric_limits<double>::quiet_NaN();
  if (enhancement)
  {
    if (!dvbridge_gl_frame_import(&el, m_gl->gpu, enhancement->layout,
                                 enhancement->width, enhancement->height, enhancement->planes))
    {
      dvbridge_gl_frame_release(&bl);
      return false;
    }
    bl.frame.enhancement_layer = &el.frame;
    elPts = enhancement->pts;
  }
  // The fast path produces the exact transport bytes directly. GUI/subtitle
  // frames and non-fullscreen geometry retain the original 32F composition.
  if (allowPacked && m_directUsable && m_scanout && !geometry.x && !geometry.y &&
      geometry.width == 3840 && geometry.height == 2160)
  {
    m_direct = dvbridge_render_packed(m_renderer, &bl.frame, metadata.Data(), metadata.Size(),
                                     base.pts, elPts, geometry, m_scanout, true);
    if (!m_direct)
    {
      m_directUsable = false;
      dvbridge_renderer_reset(m_renderer);
    }
  }
  m_prepared = m_direct || dvbridge_render_rgb(m_renderer, &bl.frame, metadata.Data(), metadata.Size(),
                                               base.pts, elPts, geometry);
  m_pendingInfo = 0;
  if (m_prepared)
  {
    // The renderer has validated all metadata offsets and extension bounds.
    const auto* parsed = reinterpret_cast<const AVDOVIMetadata*>(metadata.Data());
    // An enhancement surface can also belong to MEL. Report FEL only when
    // the same mapping used by the renderer actually reconstructs residuals.
    dvbridge_color color{};
    const bool fel = enhancement &&
                     dvbridge_map_color(&color, metadata.Data(), metadata.Size(), true) &&
                     color.dovi.nlq_active;
    m_pendingInfo = CDVPlaybackInfo::HDMI | (fel ? CDVPlaybackInfo::FEL : 0u);
    for (int i = 0; i < parsed->num_ext_blocks; ++i)
    {
      const auto level = av_dovi_get_ext(parsed, i)->level;
      if (level == 3 || level == 8 || level == 9 || level == 10 || level == 11 || level == 254)
        m_pendingInfo |= CDVPlaybackInfo::CM4;
    }
  }
  dvbridge_gl_frame_release(&el);
  dvbridge_gl_frame_release(&bl);
  return m_prepared;
}

unsigned int CDVBridgeGLES::GetPQTexture() const
{
  const auto texture = m_prepared ? dvbridge_render_texture(m_renderer) : nullptr;
  return texture ? pl_opengl_unwrap(m_gl->gpu, texture, nullptr, nullptr, nullptr) : 0;
}

bool CDVBridgeGLES::PrepareHardware(const Layer& base, const AVFrame* enhancement,
                                  const CDVMetadataBuffer& metadata, dvbridge_geometry geometry, bool allowPacked)
{
  m_prepared = m_packed = false;
  if (!enhancement)
    return Prepare(base, nullptr, metadata, geometry, allowPacked);
  dvbridge_dmabuf imported{};
  if (!dvbridge_dmabuf_import(&imported, enhancement))
    return false;
  Layer layer{};
  layer.layout = imported.layout;
  layer.width = enhancement->width;
  layer.height = enhancement->height;
  layer.pts = static_cast<double>(enhancement->pts);
  for (int i = 0; i < 3; ++i)
    layer.planes[i] = imported.planes[i];
  bool result = Prepare(base, &layer, metadata, geometry, allowPacked);
  dvbridge_dmabuf_release(&imported);
  return result;
}

bool CDVBridgeGLES::Pack(unsigned int framebuffer, int width, int height, bool flipY)
{
  m_packed = false;
  if (!m_prepared)
    return false;
  if (m_direct)
    return m_packed = framebuffer == 0 && width == 3840 && height == 2160 && flipY;
  unsigned int count;
  const auto* packets = dvbridge_packets(dvbridge_render_candidate(m_renderer), &count);
  m_packed = dvbridge_gl_pack(m_packer, GetPQTexture(), framebuffer, width, height,
                             packets, count, flipY);
  return m_packed;
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
  return valid ? m_framebuffer : 0;
}

bool CDVBridgeGLES::Presented(bool success)
{
  const bool committed = success && m_prepared && m_packed && dvbridge_render_commit(m_renderer);
  CDVPlaybackInfo::presented.store(committed ? m_pendingInfo : 0);
  if (!committed)
    dvbridge_renderer_reset(m_renderer);
  m_prepared = m_packed = false;
  m_direct = false;
  return committed;
}
