#ifndef ANYPS5_RETRO_HOST_SERVER_H_
#define ANYPS5_RETRO_HOST_SERVER_H_

#include "retro_ipc_frames.h"
#include "retro_ipc_transport.h"
#include "retro_ipc_value.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace anyps5::retro_host {

using retro_ipc::List;
using retro_ipc::Map;
using retro_ipc::Value;

class MethodCall {
 public:
  MethodCall(std::string method, Value arguments) : method_(std::move(method)), arguments_(std::move(arguments)) {}
  const std::string& method_name() const { return method_; }
  const Value* arguments() const { return &arguments_; }

 private:
  std::string method_;
  Value arguments_;
};

// One reply to one call, with the shape of flutter::MethodResult.
class MethodResult {
 public:
  struct Response {
    enum class Kind { kNone, kSuccess, kError, kNotImplemented } kind = Kind::kNone;
    Value value;
    std::string code, message;
  };

  explicit MethodResult(std::shared_ptr<Response> response) : response_(std::move(response)) {}
  void Success(const Value& value) { Set({Response::Kind::kSuccess, value, {}, {}}); }
  void Success() { Set({Response::Kind::kSuccess, Value(), {}, {}}); }
  void Error(const std::string& code, const std::string& message = "", const Value* = nullptr) {
    Set({Response::Kind::kError, Value(), code, message});
  }
  void NotImplemented() { Set({Response::Kind::kNotImplemented, Value(), {}, {}}); }

 private:
  void Set(Response response) {
    if (response_->kind == Response::Kind::kNone) *response_ = std::move(response);
  }
  std::shared_ptr<Response> response_;
};

// The host's side of the launcher's game texture. The launcher names the
// texture for a call that starts a game; frames published for it go to the
// shared frame ring, and each one is announced so the launcher can mark its
// texture as having a new frame.
class Surface {
 public:
  virtual ~Surface() = default;
  virtual int64_t Register() = 0;
  virtual void Release(int64_t texture) = 0;
  virtual void Publish(int64_t texture, const uint8_t* rgba, size_t width, size_t height, bool visible) = 0;
  virtual int64_t active() const = 0;
};

class ChannelSurface final : public Surface {
 public:
  explicit ChannelSurface(retro_ipc::Channel& channel) : channel_(channel) {}

  int64_t Register() override;
  void Release(int64_t texture) override;
  void Publish(int64_t texture, const uint8_t* rgba, size_t width, size_t height, bool visible) override;
  int64_t active() const override;
  uint32_t epoch() const;
  void set_requested(int64_t texture) { requested_ = texture; }

 private:
  void ClearLocked();

  retro_ipc::Channel& channel_;
  mutable std::mutex mutex_;
  std::unique_ptr<retro_ipc::FrameRingWriter> ring_;
  // Rings already announced stay mapped, so a handle the launcher has not
  // duplicated yet is never closed under it. Rings only grow.
  std::vector<std::unique_ptr<retro_ipc::FrameRingWriter>> announced_;
  int64_t requested_ = -1;
  int64_t active_ = -1;
  uint32_t epoch_ = 1;
};

struct HostOptions {
  bool probe = false;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual void Handle(const MethodCall& call, std::unique_ptr<MethodResult> result) = 0;
  virtual void Presented(bool visible) = 0;
  virtual bool HasSession() const = 0;
  virtual void Shutdown() = 0;
};

std::unique_ptr<Backend> CreateBackend(Surface& surface, const HostOptions& options);

int RunHost(std::unique_ptr<retro_ipc::Channel> channel, const HostOptions& options);

}  // namespace anyps5::retro_host

#endif  // ANYPS5_RETRO_HOST_SERVER_H_
