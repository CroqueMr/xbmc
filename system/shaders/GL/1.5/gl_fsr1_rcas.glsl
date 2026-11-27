// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#version 150

// FidelityFX Super Resolution v1 RCAS. RGB fragment adaptation for Kodi.
uniform sampler2D img;
uniform vec2 stepxy;
uniform float m_alpha;
in vec2 m_cord;
out vec4 fragColor;

float luma(vec3 rgb)
{
  return rgb.g + 0.5 * (rgb.r + rgb.b);
}

vec3 FsrRcas(vec3 b, vec3 d, vec3 e, vec3 f, vec3 h)
{
  vec3 lo = min(min(b,d),min(f,h));
  vec3 hi = max(max(b,d),max(f,h));
  vec3 hitMin = min(lo,e) / max(4.0*hi, vec3(1e-6));
  vec3 hitMax = (1.0-max(hi,e)) / min(4.0*lo-4.0, vec3(-1e-6));
  vec3 limits = max(-hitMin,hitMax);
  float lobe = clamp(max(max(limits.r,limits.g),limits.b), -0.1875, 0.0) * 0.5;
  float lb = luma(b), ld = luma(d), le = luma(e), lf = luma(f), lh = luma(h);
  float range = max(max(lb,ld),max(max(lf,lh),le)) - min(min(lb,ld),min(min(lf,lh),le));
  float noise = clamp(abs(0.25*(lb+ld+lf+lh)-le) / max(range,1e-6), 0.0, 1.0);
  lobe *= 1.0 - 0.5*noise;
  return (e+lobe*(b+d+f+h)) / (1.0+4.0*lobe);
}

void main()
{
  vec3 b = texture(img, m_cord - vec2(0.0,stepxy.y)).rgb;
  vec3 d = texture(img, m_cord - vec2(stepxy.x,0.0)).rgb;
  vec3 e = texture(img, m_cord).rgb;
  vec3 f = texture(img, m_cord + vec2(stepxy.x,0.0)).rgb;
  vec3 h = texture(img, m_cord + vec2(0.0,stepxy.y)).rgb;
  fragColor = vec4(FsrRcas(b,d,e,f,h),m_alpha);
#if defined(KODI_CLAMP_OUTPUT)
  fragColor = clamp(fragColor, 0.0, 1.0);
#endif
}
