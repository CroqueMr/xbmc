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

#version 100

// FidelityFX Super Resolution v1 EASU. RGB fragment adaptation for Kodi.
// https://github.com/GPUOpen-Effects/FidelityFX-FSR
precision highp float;
uniform sampler2D img;
uniform vec2 stepxy;
uniform vec2 input_size;
uniform float m_alpha;
varying vec2 cord;

vec3 samplePixel(vec2 pixel)
{
  return texture2D(img, (clamp(pixel, vec2(0.0), max(input_size - 1.0, 0.0)) + 0.5) * stepxy).rgb;
}

float luma(vec3 rgb)
{
  return rgb.g + 0.5 * (rgb.r + rgb.b);
}

void FsrEasuSet(inout vec2 direction, inout float len, float weight,
                float a, float b, float c, float d, float e)
{
  vec2 negative = vec2(c - b, c - a);
  vec2 positive = vec2(d - c, e - c);
  vec2 gradient = vec2(d - b, e - a);
  vec2 strength = clamp(abs(gradient) / max(max(abs(positive), abs(negative)), 1e-7),
                        0.0, 1.0);
  direction += gradient * weight;
  len += dot(strength, strength) * weight;
}

void FsrEasuTap(inout vec3 color, inout float total, vec2 offset, vec2 direction,
                vec2 kernel, float lobe, float clip, vec3 pixel)
{
  vec2 oriented = vec2(dot(offset, direction), dot(offset, vec2(-direction.y, direction.x)));
  oriented *= kernel;
  float distance2 = min(dot(oriented, oriented), clip);
  float window = 0.4 * distance2 - 1.0;
  float lanczos = lobe * distance2 - 1.0;
  float weight = (1.5625 * window * window - 0.5625) * lanczos * lanczos;
  color += pixel * weight;
  total += weight;
}

void main()
{
  vec2 source = cord / stepxy - 0.5;
  vec2 base = floor(source);
  vec2 p = fract(source);
  vec3 b = samplePixel(base + vec2(0.0, -1.0));
  vec3 c = samplePixel(base + vec2(1.0, -1.0));
  vec3 e = samplePixel(base + vec2(-1.0, 0.0));
  vec3 f = samplePixel(base);
  vec3 g = samplePixel(base + vec2(1.0, 0.0));
  vec3 h = samplePixel(base + vec2(2.0, 0.0));
  vec3 i = samplePixel(base + vec2(-1.0, 1.0));
  vec3 j = samplePixel(base + vec2(0.0, 1.0));
  vec3 k = samplePixel(base + vec2(1.0, 1.0));
  vec3 l = samplePixel(base + vec2(2.0, 1.0));
  vec3 n = samplePixel(base + vec2(0.0, 2.0));
  vec3 o = samplePixel(base + vec2(1.0, 2.0));

  vec2 direction = vec2(0.0);
  float len = 0.0;
  FsrEasuSet(direction, len, (1.0-p.x)*(1.0-p.y), luma(b),luma(e),luma(f),luma(g),luma(j));
  FsrEasuSet(direction, len, p.x*(1.0-p.y), luma(c),luma(f),luma(g),luma(h),luma(k));
  FsrEasuSet(direction, len, (1.0-p.x)*p.y, luma(f),luma(i),luma(j),luma(k),luma(n));
  FsrEasuSet(direction, len, p.x*p.y, luma(g),luma(j),luma(k),luma(l),luma(o));
  float direction2 = dot(direction, direction);
  direction = direction2 < (1.0/32768.0) ? vec2(1.0,0.0) : direction * inversesqrt(direction2);
  len = 0.25 * len * len;
  float stretch = 1.0 / max(abs(direction.x), abs(direction.y));
  vec2 kernel = vec2(1.0 + (stretch-1.0)*len, 1.0 - 0.5*len);
  float lobe = 0.5 - 0.29 * len;
  float clip = 1.0 / lobe;
  vec3 color = vec3(0.0);
  float total = 0.0;
  FsrEasuTap(color,total,vec2(0.0,-1.0)-p,direction,kernel,lobe,clip,b);
  FsrEasuTap(color,total,vec2(1.0,-1.0)-p,direction,kernel,lobe,clip,c);
  FsrEasuTap(color,total,vec2(-1.0,0.0)-p,direction,kernel,lobe,clip,e);
  FsrEasuTap(color,total,vec2(0.0,0.0)-p,direction,kernel,lobe,clip,f);
  FsrEasuTap(color,total,vec2(1.0,0.0)-p,direction,kernel,lobe,clip,g);
  FsrEasuTap(color,total,vec2(2.0,0.0)-p,direction,kernel,lobe,clip,h);
  FsrEasuTap(color,total,vec2(-1.0,1.0)-p,direction,kernel,lobe,clip,i);
  FsrEasuTap(color,total,vec2(0.0,1.0)-p,direction,kernel,lobe,clip,j);
  FsrEasuTap(color,total,vec2(1.0,1.0)-p,direction,kernel,lobe,clip,k);
  FsrEasuTap(color,total,vec2(2.0,1.0)-p,direction,kernel,lobe,clip,l);
  FsrEasuTap(color,total,vec2(0.0,2.0)-p,direction,kernel,lobe,clip,n);
  FsrEasuTap(color,total,vec2(1.0,2.0)-p,direction,kernel,lobe,clip,o);
  vec3 lo = min(min(f,g),min(j,k));
  vec3 hi = max(max(f,g),max(j,k));
  gl_FragColor = vec4(clamp(color / total, lo, hi), m_alpha);
#if defined(KODI_CLAMP_OUTPUT)
  gl_FragColor = clamp(gl_FragColor, 0.0, 1.0);
#endif
}
