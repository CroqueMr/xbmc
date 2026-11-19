/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "guilib/Shader.h"

#include <array>
#include <string>
#include <vector>

class CGuiCompositeShaderGLES : public Shaders::CGLSLShaderProgram
{
public:
  explicit CGuiCompositeShaderGLES(const std::string& prefix);
  ~CGuiCompositeShaderGLES() override;

  void SetProjection(const GLfloat* proj) { m_proj = proj; }
  void SetInputPQ(bool value) { m_inputPQ = value; }
  void SetSDRBT2020(bool value, bool premultiplied = false)
  {
    m_sdrBT2020 = value;
    m_unpremultiplyGui = premultiplied;
  }
  void SetOutputRange(float scale, float offset) { m_outputScale = scale; m_outputOffset = offset; }

  bool CreateLUTs(int colorTransfer, float sdrGamma = 2.4f);
  void SetPlayerLed(float peak = 0.0f, const std::array<float,9>& gui = {},
                    const std::array<float,9>& subtitles = {})
  {
    m_playerLedScale = peak > 0 ? 10000.0f / peak : 0.0f;
    m_playerLedGui = gui;
    m_playerLedSubtitles = subtitles;
  }

  GLint GetPosLoc() { return m_hPos; }
  GLint GetTexLoc() { return m_hTex; }
  GLuint GetDegammaTexture() const { return m_lutDegammaTexId; }
  GLuint GetTransferTexture() const { return m_lutTFTexId; }

protected:
  void OnCompiledAndLinked() override;
  bool OnEnabled() override;

private:
  // One entry per RGBA8 input value; increase to match GUI bit depth.
  static constexpr int LUT_SIZE = 256;

  GLuint CreateLUTTexture(const std::vector<float>& data);
  static std::vector<float> GenerateDegammaLUT();
  static std::vector<float> GeneratePQLUT(float sdrPeak);

  const GLfloat* m_proj{nullptr};
  float m_sdrPeak{203.0f / 10000.0f};

  GLuint m_lutDegammaTexId{0};
  GLuint m_lutTFTexId{0};
  float m_ootfGamma{0.0f};
  bool m_inputPQ{false}, m_sdrBT2020{false}, m_unpremultiplyGui{false};
  float m_outputScale{1.0f}, m_outputOffset{0.0f};

  GLint m_hPos{-1};
  GLint m_hTex{-1};
  GLint m_hSamp{-1};
  GLint m_hLutDegamma{-1};
  GLint m_hLutTF{-1};
  GLint m_hProj{-1};
  GLint m_hOotfGamma{-1};
  GLint m_hInputPQ{-1}, m_hSDRBT2020{-1}, m_hUnpremultiplyGui{-1};
  GLint m_hOutputRange{-1};
  GLint m_hPlayerLedScale{-1}, m_hPlayerLedGui{-1}, m_hPlayerLedSubtitles{-1};
  float m_playerLedScale{};
  std::array<float,9> m_playerLedGui{}, m_playerLedSubtitles{};
};
