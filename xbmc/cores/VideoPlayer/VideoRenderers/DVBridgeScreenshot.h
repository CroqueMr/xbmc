/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace DVBRIDGE
{
enum class SDRScreenshotFormat { RGBA8, BGRA8, RGBA16, RGB10 };
// Capture the final SDR target (movie and GUI), normalized to full-range sRGB.
// ReadFramebufferRegion supplies contiguous rows; preserve their GL order here.
inline bool DecodeSDRScreenshot(const uint8_t* pixels,unsigned width,unsigned height,
    SDRScreenshotFormat format,unsigned bits,bool limited,double gamma,uint16_t* out)
{
  if (!pixels || !out || !width || !height || bits<8 || bits>16 ||
      !std::isfinite(gamma) || gamma<1.8 || gamma>2.8) return false;
  if (format<SDRScreenshotFormat::RGBA8 || format>SDRScreenshotFormat::RGB10) return false;
  const double maximum=(1u<<bits)-1;
  const double offset=limited ? double(16u<<(bits-8))/maximum : 0;
  const double scale=limited ? double(219u<<(bits-8))/maximum : 1;
  const size_t step=format==SDRScreenshotFormat::RGBA16 ? 8 : 4;
  for (size_t i=0;i<size_t(width)*height;++i)
  {
    const auto* p=pixels+i*step;
    const uint32_t packed=uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
    for (unsigned c=0;c<4;++c)
    {
      double value;
      if (format==SDRScreenshotFormat::RGBA16)
        value=double(unsigned(p[2*c]) | unsigned(p[2*c+1])<<8)/65535;
      else if (format==SDRScreenshotFormat::RGB10)
        value=c==3 ? 1 : double((packed>>(10*c))&1023)/1023; // X2BGR has no alpha.
      else
        value=double(p[format==SDRScreenshotFormat::BGRA8 && c<3 ? 2-c : c])/255;
      if (c<3)
      {
        const double linear=std::pow(std::clamp((value-offset)/scale,0.0,1.0),gamma);
        value=linear<=0.0031308 ? 12.92*linear : 1.055*std::pow(linear,1.0/2.4)-0.055;
      }
      out[4*i+c]=static_cast<uint16_t>(std::lround(std::clamp(value,0.0,1.0)*65535));
    }
  }
  return true;
}

// Readback only: decode the completed source-YCC carrier, including its GUI.
// Rows retain their GL order; the screenshot buffer supplies the signed stride.
inline bool DecodeReferenceScreenshot(const uint8_t* carrier, unsigned width, unsigned height,
                                     const float decode[12], const float linear[9], uint16_t* rgb)
{
  if (!carrier || !rgb || !width || !height || (width & 1))
    return false;
  const auto eotf=[](double q) {
    q=std::pow(std::max(q,0.0),32.0/2523.0);
    return std::pow(std::max(q-3424.0/4096.0,0.0)/(2413.0/128.0-2392.0/128.0*q),16384.0/2610.0);
  };
  const auto oetf=[](double n) {
    n=std::pow(std::clamp(n,0.0,1.0),2610.0/16384.0);
    return std::pow((3424.0/4096.0+2413.0/128.0*n)/(1.0+2392.0/128.0*n),2523.0/32.0);
  };
  for (size_t pixel=0;pixel<size_t(width)*height;pixel+=2)
  {
    const uint8_t* pair=carrier+4*pixel;
    const double cb=((unsigned(pair[0])<<4)|(pair[2]>>4))/4095.0;
    const double cr=((unsigned(pair[4])<<4)|(pair[6]>>4))/4095.0;
    for (unsigned side=0;side<2;side++)
    {
      const uint8_t* p=pair+4*side;
      const double y=((unsigned(p[1])<<4)|(p[2]&15))/4095.0;
      double light[3];
      for (unsigned i=0;i<3;i++)
        light[i]=eotf(decode[4*i]*y+decode[4*i+1]*cb+decode[4*i+2]*cr+decode[4*i+3]);
      uint16_t* out=rgb+4*(pixel+side);
      for (unsigned i=0;i<3;i++)
        out[i]=static_cast<uint16_t>(std::lround(65535.0*oetf(
            linear[3*i]*light[0]+linear[3*i+1]*light[1]+linear[3*i+2]*light[2])));
      out[3]=65535;
    }
  }
  return true;
}
}
