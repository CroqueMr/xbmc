/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The reversed GLES blit and framebuffer readback are adapted from
 *  LibreELEC Hades v1.0 (hadesdevls, Kodi patch 9719).
 */

#include "PascalHDR10Scanout.h"

#include "drm/DRMAtomic.h"
#include "utils/EGLImage.h"

#include "system_gl.h"
#include <gbm.h>
#include <unistd.h>

using namespace KODI::WINDOWING::GBM;

namespace
{
struct FramebufferBindings
{
  FramebufferBindings()
  {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  }

  ~FramebufferBindings()
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  }

  GLint read{};
  GLint draw{};
};

bool FlipHDR10Frame(GLuint source, GLuint target, int width, int height)
{
  const FramebufferBindings bindings;
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
  glBlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  const bool copied = glGetError() == GL_NO_ERROR;
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
  return copied;
}

bool Attach(GLuint& framebuffer, GLuint texture)
{
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}
}

std::unique_ptr<CPascalHDR10Scanout> CPascalHDR10Scanout::Create(
    gbm_device* device, CDRMAtomic& drm, int width, int height)
{
  auto scanout = std::make_unique<CPascalHDR10Scanout>();
  scanout->m_width = width;
  scanout->m_height = height;
  if (!scanout->Init(device, drm))
    return {};
  return scanout;
}

CPascalHDR10Scanout::~CPascalHDR10Scanout()
{
  for (const auto& buffer : m_buffers)
  {
    glDeleteFramebuffers(1, &buffer.framebuffer);
    glDeleteTextures(1, &buffer.texture);
  }
  glDeleteFramebuffers(1, &m_source);
  glDeleteTextures(1, &m_texture);
}

bool CPascalHDR10Scanout::Init(gbm_device* device, CDRMAtomic& drm)
{
  const FramebufferBindings bindings;
  auto* plane = drm.GetGuiPlane() ? drm.GetGuiPlane() : drm.GetVideoPlane();
  constexpr uint32_t format = DRM_FORMAT_XRGB2101010;
  if (!plane || !plane->SupportsFormat(format) || !device ||
      m_width < 2 || m_height < 2)
    return false;

  // CEGLImage owns the import until the texture has taken its reference.
  CEGLImage image(eglGetCurrentDisplay());
  GLint previousTexture{};
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  const auto create = [&] {
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB10_A2, m_width, m_height);
    if (!Attach(m_source, m_texture))
      return false;

    for (auto& buffer : m_buffers)
    {
      auto& modifiers = plane->GetModifiersForFormat(format);
      constexpr uint32_t usage = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
      auto* bo = modifiers.empty() ? nullptr : gbm_bo_create_with_modifiers2(
          device, m_width, m_height, format, modifiers.data(), modifiers.size(), usage);
      if (!bo)
        bo = gbm_bo_create(device, m_width, m_height, format, usage);
      if (!bo)
        return false;
      buffer.bo = {bo, gbm_bo_destroy};
      const auto modifier = gbm_bo_get_modifier(bo);
      if (!plane->SupportsFormatAndModifier(format, modifier) || gbm_bo_get_plane_count(bo) != 1)
        return false;
      const int fd = gbm_bo_get_fd(bo);
      if (fd < 0)
        return false;
      CEGLImage::EglAttrs attrs{};
      attrs.width = m_width;
      attrs.height = m_height;
      attrs.format = format;
      attrs.planes[0] = {fd, static_cast<int>(gbm_bo_get_offset(bo, 0)),
                        static_cast<int>(gbm_bo_get_stride(bo)), modifier};
      const bool imported = image.CreateImage(attrs);
      close(fd);
      if (!imported)
        return false;

      glGenTextures(1, &buffer.texture);
      glBindTexture(GL_TEXTURE_2D, buffer.texture);
      image.UploadImage(GL_TEXTURE_2D);
      image.DestroyImage();
      if (!Attach(buffer.framebuffer, buffer.texture) || !drm.TestFramebuffer(bo))
        return false;
    }
    return Probe();
  };
  const bool ready = create();
  glBindTexture(GL_TEXTURE_2D, previousTexture);
  return ready;
}

bool CPascalHDR10Scanout::Probe() const
{
  const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLint box[4], packBuffer{};
  GLboolean mask[4];
  glGetIntegerv(GL_SCISSOR_BOX, box);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, m_source);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const GLfloat black[]{0, 0, 0, 1};
  const GLfloat sample[]{65.f / 1023, 513.f / 1023, 937.f / 1023, 1};
  glDisable(GL_SCISSOR_TEST);
  glClearBufferfv(GL_COLOR, 0, black);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 1, 1);
  glClearBufferfv(GL_COLOR, 0, sample);

  bool valid = true;
  for (const auto& buffer : m_buffers)
  {
    valid &= FlipHDR10Frame(m_source, buffer.framebuffer, m_width, m_height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, buffer.framebuffer);
    GLuint top{}, bottom{};
    glReadPixels(0, m_height - 1, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &top);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &bottom);
    valid &= glGetError() == GL_NO_ERROR &&
        (top & 0x3fffffff) == (65u | (513u << 10) | (937u << 20)) &&
        (bottom & 0x3fffffff) == 0;
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
  glScissor(box[0], box[1], box[2], box[3]);
  if (!scissor)
    glDisable(GL_SCISSOR_TEST);
  glColorMask(mask[0], mask[1], mask[2], mask[3]);
  return valid;
}

std::shared_ptr<gbm_bo> CPascalHDR10Scanout::Blit()
{
  auto& buffer = m_buffers[m_next];
  if (!FlipHDR10Frame(m_source, buffer.framebuffer, m_width, m_height))
    return {};
  glFinish();
  return buffer.bo;
}
