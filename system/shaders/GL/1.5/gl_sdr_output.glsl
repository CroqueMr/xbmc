/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#if defined(KODI_SDR_OUTPUT_CONVERSION)
uniform vec2 m_sdrRange;

vec3 encodeSDROutput(vec3 rgb)
{
#if defined(KODI_SDR_WIDE_TONEMAP)
  const mat3 toBT2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
#if defined(KODI_TONE_MAPPING_ACES)
  const float gamma = 1.0 / 0.27;
#else
  const float gamma = 2.2;
#endif
  vec3 linear = pow(clamp(rgb, 0.0, 1.0), vec3(gamma));
  rgb = pow(max(toBT2020 * linear, vec3(0.0)), vec3(1.0 / gamma));
#endif
  return rgb * m_sdrRange.x + m_sdrRange.y;
}
#endif
