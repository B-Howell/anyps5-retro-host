// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_CHANNEL_H_
#define ANYPS5_RETRO_HOST_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "platform.h"

namespace retro_host {

constexpr uint32_t kMinMessage = 2;
constexpr uint32_t kMaxMessage = 1u << 20;

enum class ReadResult { kMessage, kEnd, kProtocolError };

// The control stream to the launcher: 4-byte little-endian length, then that many bytes of JSON.
class Channel {
 public:
  virtual ~Channel() = default;

  // Reads one message body into body. The host never receives a descriptor, so one arriving with
  // any read is closed and is a protocol error.
  virtual ReadResult Read(std::string& body) = 0;

  // Sends one message, with the ring memory attached when memory is not kNoHandle (on Linux and
  // macOS as SCM_RIGHTS data; Windows passes the handle value inside the message instead).
  // Returns false when the launcher has gone.
  bool Write(const std::string& body, NativeHandle memory = kNoHandle);

  // Milliseconds since the write in progress started, or -1 when none is.
  int64_t WriteAge() const;

 protected:
  virtual bool WriteFrame(const std::string& frame, NativeHandle memory) = 0;

 private:
  std::mutex write_mutex_;
  std::atomic<int64_t> write_started_{-1};
};

// The channel named on the command line (--ipc-fd 3, or --ipc-read and --ipc-write on Windows).
std::unique_ptr<Channel> OpenChannel(const Arguments& arguments);

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_CHANNEL_H_
