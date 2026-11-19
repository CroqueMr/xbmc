// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "utils/DVBridgeKernelLog.h"
#include "utils/log.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace DVBRIDGE
{
// Used only by a Kodi background job after a rate-limited DV output failure.
// Read-only, nonblocking and bounded. No external process, new service or GPU work.
class KernelLogReader
{
public:
  ~KernelLogReader()
  {
    if (m_fd >= 0)
      close(m_fd);
  }

  void Collect()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd < 0)
    {
      m_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (m_fd < 0)
      {
        if (!m_warned)
          CLog::Log(LOGWARNING, "DVBridge kernel: cannot read /dev/kmsg errno={}; use dv-diagnostics collect", errno);
        m_warned = true;
        return;
      }
    }
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return;
    const uint64_t nowUs = uint64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    std::array<char, 16384> buffer{};
    unsigned int copied = 0, suppressed = 0;
    for (unsigned int records = 0; records < 4096; ++records)
    {
      if (std::chrono::steady_clock::now() >= deadline)
        break;
      const auto count = read(m_fd, buffer.data(), buffer.size());
      if (count < 0)
      {
        if (errno == EPIPE || errno == EINTR)
          continue; // Ring overrun advances this reader to the next available record.
        if (errno != EAGAIN && !m_warned)
        {
          CLog::Log(LOGWARNING, "DVBridge kernel: read failed errno={}; use dv-diagnostics collect", errno);
          m_warned = true;
        }
        break;
      }
      if (!count)
        break;
      const auto message = m_filter.Accept({buffer.data(), static_cast<size_t>(count)}, nowUs);
      if (!message)
        continue;
      if (copied < 16)
      {
        CLog::Log(LOGERROR, "DVBridge kernel: {}", *message);
        ++copied;
      }
      else
        ++suppressed;
    }
    if (suppressed)
      CLog::Log(LOGWARNING, "DVBridge kernel: {} additional records suppressed; full kernel log available via dv-diagnostics collect", suppressed);
  }

private:
  std::mutex m_mutex;
  KernelLogFilter m_filter;
  int m_fd{-1};
  bool m_warned{false};
};
}
