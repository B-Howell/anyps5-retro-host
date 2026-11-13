// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "channel.h"

namespace retro_host {

bool Channel::Write(const std::string& body, NativeHandle memory) {
  std::string frame;
  frame.reserve(4 + body.size());
  const auto size = static_cast<uint32_t>(body.size());
  for (int shift = 0; shift < 32; shift += 8) frame += static_cast<char>((size >> shift) & 0xFF);
  frame += body;
  std::lock_guard<std::mutex> lock(write_mutex_);
  write_started_ = NowMs();
  const bool written = WriteFrame(frame, memory);
  write_started_ = -1;
  return written;
}

int64_t Channel::WriteAge() const {
  const int64_t started = write_started_;
  return started < 0 ? -1 : NowMs() - started;
}

}  // namespace retro_host
