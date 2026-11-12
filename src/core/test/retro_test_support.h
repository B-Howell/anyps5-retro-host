#ifndef ANYPS5_RETRO_TEST_SUPPORT_H_
#define ANYPS5_RETRO_TEST_SUPPORT_H_

#include "retro_session.h"
#include <chrono>
#include <stdexcept>
#include <thread>

namespace anyps5::retro::test {
inline void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class TestVideo final : public Video {
 public:
  bool Open() override { return false; }
  int PreferredContext() const override { return 0; }
  std::string_view PreferredRenderer() const override { return {}; }
  bool Bind(HardwareCallbacks&) override { return false; }
  bool Environment(unsigned, void*, int) override { return false; }
  void Reset(const HardwareCallbacks&) override {}
  void Clear() override {}
  void Close() override {}
  std::shared_ptr<Frame> Read(unsigned, unsigned, const HardwareCallbacks&) override { return {}; }
};

class TestAudio final : public Audio {
 public:
  bool Open(double rate) override { return rate == 48000.0; }
  void Write(const int16_t*, size_t) override {}
  void Flush() override {}
  int64_t dropped_frames() const override { return 0; }
  void Close() override {}
};

template <typename Condition>
void WaitFor(Condition condition, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!condition()) {
    Check(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
}  // namespace anyps5::retro::test
#endif
