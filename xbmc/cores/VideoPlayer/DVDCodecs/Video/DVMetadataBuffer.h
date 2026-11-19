/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

extern "C"
{
#include <libavutil/buffer.h>
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

class CDVMetadataBuffer
{
public:
  bool Assign(const uint8_t* data, size_t size)
  {
    if (!data || size == 0 || size > MAX_PAYLOAD_SIZE)
    {
      Reset();
      return false;
    }

    AVBufferRef* buffer = av_buffer_alloc(size);
    if (!buffer)
    {
      Reset();
      return false;
    }
    std::memcpy(buffer->data, data, size);
    m_buffer = std::shared_ptr<AVBufferRef>(buffer, [](AVBufferRef* ref) { av_buffer_unref(&ref); });
    return true;
  }

  void Reset() { m_buffer.reset(); }
  const uint8_t* Data() const { return m_buffer ? m_buffer->data : nullptr; }
  size_t Size() const { return m_buffer ? m_buffer->size : 0; }

private:
  // Bound unexpected side data before allocating; no truncation is allowed.
  static constexpr size_t MAX_PAYLOAD_SIZE = 1024 * 1024;
  std::shared_ptr<AVBufferRef> m_buffer;
};
