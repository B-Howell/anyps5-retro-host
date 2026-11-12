#ifndef ANYPS5_RETRO_HOST_PROXY_H_
#define ANYPS5_RETRO_HOST_PROXY_H_

#include "retro_ipc_value.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anyps5::retro_ipc {

struct ProxyResult {
  enum class Kind { kSuccess, kError, kNotImplemented };
  Kind kind = Kind::kSuccess;
  Value value;
  std::string code, message;

  static ProxyResult Success(Value value = Value()) { return {Kind::kSuccess, std::move(value), {}, {}}; }
  static ProxyResult Error(std::string code, std::string message) {
    return {Kind::kError, Value(), std::move(code), std::move(message)};
  }
};

using ProxyReply = std::function<void(ProxyResult)>;

// What the launcher's Flutter embedding supplies. Textures are registered and
// unregistered on the platform thread; frame_available may be called on any
// thread; post runs a task on the platform thread.
struct ProxyPlatform {
  std::function<int64_t()> register_texture;
  std::function<void(int64_t)> unregister_texture;
  std::function<void(int64_t)> frame_available;
  std::function<void(std::function<void()>)> post;
};

struct ProxyOptions {
  std::filesystem::path host;
  std::vector<std::string> arguments;
  // Windows registers a texture for each game; Linux keeps one for the view.
  bool texture_per_session = true;
  std::chrono::milliseconds request_timeout{30000};
  std::chrono::milliseconds stop_timeout{20000};
  std::chrono::milliseconds shutdown_timeout{15000};
};

struct ProxyFrame {
  std::shared_ptr<const void> lease;
  const uint8_t* pixels = nullptr;
  size_t width = 0, height = 0;
};

// The launcher end of the anyps5/retro_core method channel. It starts the
// retro host process when the first call arrives, forwards every call to it
// unchanged and returns its replies in order. Frames reach the launcher's
// Flutter texture through shared memory. If the host exits or stops
// answering while a game runs, the proxy kills it and answers for the lost
// session as an emulator that ended with an error would.
class RetroHostProxy {
 public:
  RetroHostProxy(ProxyOptions options, ProxyPlatform platform);
  ~RetroHostProxy();
  RetroHostProxy(const RetroHostProxy&) = delete;
  RetroHostProxy& operator=(const RetroHostProxy&) = delete;

  void Handle(const std::string& method, Value arguments, ProxyReply reply);
  std::optional<ProxyFrame> Acquire(int64_t texture);
  void Shutdown();
  int64_t host_pid() const;

  static std::filesystem::path DefaultHostPath();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace anyps5::retro_ipc

#endif  // ANYPS5_RETRO_HOST_PROXY_H_
