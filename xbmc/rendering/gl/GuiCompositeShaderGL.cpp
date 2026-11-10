/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GuiCompositeShaderGL.h"

#include "utils/log.h"

extern "C"
{
#include <libavutil/pixfmt.h>
}

#include <cmath>

namespace
{
// ST2084 (PQ) constants
constexpr float ST2084_m1 = 0.1593017578125f; // 2610/16384
constexpr float ST2084_m2 = 78.84375f; // 2523/4096 * 128
constexpr float ST2084_c1 = 0.8359375f; // 3424/4096
constexpr float ST2084_c2 = 18.8515625f; // 2413/4096 * 32
constexpr float ST2084_c3 = 18.6875f; // 2392/4096 * 32

float ForwardPQ(float L)
{
  float Lm1 = std::pow(L, ST2084_m1);
  return std::pow((ST2084_c1 + ST2084_c2 * Lm1) / (1.0f + ST2084_c3 * Lm1), ST2084_m2);
}

// IEC 61966-2-1 sRGB EOTF.
float SRGBToLinear(float v)
{
  return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

} // namespace

CGuiCompositeShaderGL::CGuiCompositeShaderGL(const std::string& prefix)
{
  VertexShader()->LoadSource("gl_gui_composite.vert", prefix);
  PixelShader()->LoadSource("gl_gui_composite.frag", prefix);
}

CGuiCompositeShaderGL::~CGuiCompositeShaderGL()
{
  if (m_lutDegammaTexId)
    glDeleteTextures(1, &m_lutDegammaTexId);
  if (m_lutTFTexId)
    glDeleteTextures(1, &m_lutTFTexId);
}

void CGuiCompositeShaderGL::OnCompiledAndLinked()
{
  m_hPos = glGetAttribLocation(ProgramHandle(), "a_pos");
  m_hTex = glGetAttribLocation(ProgramHandle(), "a_tex");
  m_hSamp = glGetUniformLocation(ProgramHandle(), "u_samp");
  m_hLutDegamma = glGetUniformLocation(ProgramHandle(), "u_lutDegamma");
  m_hLutTF = glGetUniformLocation(ProgramHandle(), "u_lutTF");
  m_hProj = glGetUniformLocation(ProgramHandle(), "u_proj");
  m_hOotfGamma = glGetUniformLocation(ProgramHandle(), "u_ootfGamma");
  m_hInputPQ = glGetUniformLocation(ProgramHandle(), "u_inputPQ");
  m_hSDRBT2020 = glGetUniformLocation(ProgramHandle(), "u_sdrBT2020");
  m_hUnpremultiplyGui = glGetUniformLocation(ProgramHandle(), "u_unpremultiplyGui");
  m_hOutputRange = glGetUniformLocation(ProgramHandle(), "u_outputRange");
  m_hPlayerLedScale = glGetUniformLocation(ProgramHandle(), "u_playerLedScale");
  m_hPlayerLedGui = glGetUniformLocation(ProgramHandle(), "u_playerLedGui");
  m_hPlayerLedSubtitles = glGetUniformLocation(ProgramHandle(), "u_playerLedSubtitles");
  glUseProgram(ProgramHandle());
  glUniform1i(m_hSamp, 0);
  glUniform1i(m_hLutDegamma, 1);
  glUniform1i(m_hLutTF, 2);
  glUseProgram(0);
}

bool CGuiCompositeShaderGL::OnEnabled()
{
  if (m_proj)
    glUniformMatrix4fv(m_hProj, 1, GL_FALSE, m_proj);

  glUniform1f(m_hOotfGamma, m_ootfGamma);
  glUniform1i(m_hInputPQ, m_inputPQ);
  glUniform1i(m_hSDRBT2020, m_sdrBT2020);
  glUniform1i(m_hUnpremultiplyGui, m_unpremultiplyGui);
  glUniform2f(m_hOutputRange, m_outputScale, m_outputOffset);
  glUniform1f(m_hPlayerLedScale, m_playerLedScale);
  if (m_playerLedScale > 0)
  {
    glUniformMatrix3fv(m_hPlayerLedGui, 1, GL_FALSE, m_playerLedGui.data());
    glUniformMatrix3fv(m_hPlayerLedSubtitles, 1, GL_FALSE, m_playerLedSubtitles.data());
  }

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_lutDegammaTexId);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, m_lutTFTexId);
  glActiveTexture(GL_TEXTURE0);

  return true;
}

GLuint CGuiCompositeShaderGL::CreateLUTTexture(const std::vector<float>& data)
{
  while (glGetError() != GL_NO_ERROR)
  {
  }

  GLuint texId;
  glGenTextures(1, &texId);
  glBindTexture(GL_TEXTURE_2D, texId);

  // GL_R16F is core since OpenGL 3.0.
  const bool uploaded = [&]
  {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, data.size(), 1, 0, GL_RED, GL_FLOAT, data.data());
    if (glGetError() == GL_NO_ERROR)
      return true;
    CLog::Log(LOGERROR, "CGuiCompositeShaderGL::CreateLUTTexture - failed to create {} entry LUT texture",
              data.size());
    return false;
  }();

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!uploaded)
  {
    glDeleteTextures(1, &texId);
    return 0;
  }
  return texId;
}

std::vector<float> CGuiCompositeShaderGL::GenerateDegammaLUT()
{
  std::vector<float> lut(LUT_SIZE);
  for (int i = 0; i < LUT_SIZE; i++)
  {
    float x = static_cast<float>(i) / (LUT_SIZE - 1);
    lut[i] = SRGBToLinear(x);
  }
  return lut;
}

std::vector<float> CGuiCompositeShaderGL::GeneratePQLUT(float sdrPeak)
{
  // PQ is display-referred (absolute luminance). sdrPeak is in PQ-normalized
  // units (nits / 10000), e.g. 203 nits = 0.0203. The LUT maps the full [0,1]
  // texture coordinate range to ForwardPQ([0, sdrPeak]), giving full LUT
  // resolution across the actual SDR luminance range.
  std::vector<float> lut(LUT_SIZE);
  for (int i = 0; i < LUT_SIZE; i++)
  {
    float L = static_cast<float>(i) / (LUT_SIZE - 1) * sdrPeak;
    lut[i] = ForwardPQ(L);
  }
  return lut;
}

bool CGuiCompositeShaderGL::CreateLUTs(int colorTransfer, float sdrGamma)
{
  if (m_lutDegammaTexId)
    glDeleteTextures(1, &m_lutDegammaTexId);
  if (m_lutTFTexId)
    glDeleteTextures(1, &m_lutTFTexId);
  m_lutTFTexId = 0;
  m_lutDegammaTexId = 0;

  m_lutDegammaTexId = CreateLUTTexture(GenerateDegammaLUT());
  if (!m_lutDegammaTexId)
  {
    CLog::Log(LOGERROR, "CGuiCompositeShaderGL::CreateLUTs - failed to create degamma LUT");
    return false;
  }

  if (colorTransfer == AVCOL_TRC_GAMMA22 || colorTransfer == AVCOL_TRC_BT2020_10)
  {
    if (!std::isfinite(sdrGamma) || sdrGamma < 1.8f || sdrGamma > 2.8f) return false;
    // Negative selects SDR in the shared compositor; no PQ/HLG LUT.
    m_ootfGamma = -sdrGamma;
  }
  else if (colorTransfer == AVCOL_TRC_SMPTE2084)
  {
    m_lutTFTexId = CreateLUTTexture(GeneratePQLUT(m_sdrPeak));
    if (!m_lutTFTexId)
    {
      CLog::Log(LOGERROR, "CGuiCompositeShaderGL::CreateLUTs - failed to create PQ LUT");
      return false;
    }
    m_ootfGamma = 0.0f;
    CLog::Log(LOGDEBUG, "CGuiCompositeShaderGL::CreateLUTs - created PQ LUT ({} entries)",
              LUT_SIZE);
  }
  else if (colorTransfer == AVCOL_TRC_ARIB_STD_B67)
  {
    // HLG: no TF LUT needed, shader computes OETF + inverse OOTF directly.
    // BT.2100: gamma = 1.2 + 0.42 * log10(Lw/1000). For 1000-nit ref: 1.2.
    m_ootfGamma = 1.2f;
    CLog::Log(LOGDEBUG, "CGuiCompositeShaderGL::CreateLUTs - HLG mode (gamma {})", m_ootfGamma);
  }
  else
  {
    CLog::Log(LOGERROR, "CGuiCompositeShaderGL::CreateLUTs - unsupported transfer function {}",
              colorTransfer);
    return false;
  }
  return true;
}
