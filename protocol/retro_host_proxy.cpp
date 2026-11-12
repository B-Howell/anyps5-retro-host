#include "retro_host_proxy.h"

#include "retro_ipc_frames.h"
#include "retro_ipc_json.h"
#include "retro_ipc_transport.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace anyps5::retro_ipc {
namespace {
using Clock = std::chrono::steady_clock;

constexpr char kLostMessage[] = "The emulator closed unexpectedly.";
constexpr char kHungMessage[] = "The emulator stopped responding and was closed.";
constexpr char kSpawnMessage[] = "The emulator could not be started.";

bool CreatesSession(const std::string& method) {
  return method == "start" || method == "ps1LocalCheckpointReopen";
}

bool StartsHostWork(const std::string& method) {
  return CreatesSession(method) || method == "installPs3Packages";
}

ProxyResult LostReply(const std::string& method, const std::string& reason) {
  if (method == "status") return ProxyResult::Success(Value(Map{{Value("ended"), Value(true)}, {Value("error"), Value(reason)}}));
  if (method == "ps3PackageStatus") {
    return ProxyResult::Success(Value(Map{{Value("ended"), Value(true)}, {Value("error"), Value(reason)},
                                          {Value("completed"), Value(0)}, {Value("rejected"), Value(false)}}));
  }
  if (method == "start") return ProxyResult::Error("start", reason);
  if (method == "installPs3Packages") return ProxyResult::Error("install", reason);
  if (method == "stop" || method == "release" || method == "pause" || method == "input" ||
      method == "ps1CheckpointCancel" || method == "ps1CheckpointHold" || method == "ps2LanExchange")
    return ProxyResult::Success();
  if (method == "changeDisc" || method == "option" || method == "tuningOptions" || method == "netplayStart" ||
      method == "ps1ResumeLocal" || method == "ps2LanReset" || method == "ps1CheckpointResult")
    return ProxyResult::Success(Value(false));
  if (method.rfind("ps1", 0) == 0) return ProxyResult::Error("checkpoint", reason);
  return ProxyResult::Error("retroHost", reason);
}

const Value* Field(const Map& map, const char* key) {
  const auto found = map.find(Value(key));
  return found == map.end() ? nullptr : &found->second;
}

std::optional<int64_t> IntegerField(const Map& map, const char* key) {
  const auto* value = Field(map, key);
  if (!value || (!std::holds_alternative<int32_t>(*value) && !std::holds_alternative<int64_t>(*value)))
    return std::nullopt;
  return value->LongValue();
}

struct Pending {
  std::string method;
  ProxyReply reply;
  Clock::time_point sent;
  int64_t candidate;
};
}  // namespace

class RetroHostProxy::Impl : public std::enable_shared_from_this<Impl> {
 public:
  Impl(ProxyOptions options, ProxyPlatform platform)
      : options_(std::move(options)), platform_(std::move(platform)) {}

  ~Impl() { StopWatchdog(); }

  void Start() { watchdog_ = std::thread([self = weak_from_this()] { Watch(self); }); }

  void Handle(const std::string& method, Value arguments, ProxyReply reply) {
    if (shut_down_) return reply(ProxyResult::Error("retroHost", "The launcher is closing."));
    if (method == "hostProcessId") {
      const auto host = CurrentHost();
      return reply(ProxyResult::Success(Value(static_cast<int64_t>(host ? host->process->pid() : 0))));
    }
    if (lost_) {
      if (!StartsHostWork(method)) {
        const auto answer = LostReply(method, *lost_);
        if (method == "release") ForgetLostSession();
        return reply(answer);
      }
      ForgetLostSession();
    }
    auto host = CurrentHost();
    if (!host) host = Spawn();
    if (!host) {
      if (method == "status") return reply(ProxyResult::Success());
      return reply(StartsHostWork(method) ? LostReply(method, kSpawnMessage)
                                          : ProxyResult::Error("retroHost", kSpawnMessage));
    }
    int64_t candidate = -1;
    if (CreatesSession(method)) {
      if (options_.texture_per_session) {
        candidate = platform_.register_texture();
      } else {
        if (persistent_texture_ < 0) persistent_texture_ = platform_.register_texture();
        candidate = persistent_texture_;
      }
    }
    if (method == "stop" && host_session_ && !stop_deadline_.load())
      stop_deadline_ = Millis(Clock::now() + options_.stop_timeout);
    const auto id = ++next_id_;
    Map envelope{{Value("v"), Value(static_cast<int32_t>(kProtocolVersion))},
                 {Value("id"), Value(id)},
                 {Value("method"), Value(method)},
                 {Value("args"), std::move(arguments)}};
    if (candidate >= 0) envelope.emplace(Value("texture"), Value(candidate));
    {
      std::lock_guard<std::mutex> lock(host->mutex);
      host->pending.emplace(id, Pending{method, std::move(reply), Clock::now(), candidate});
    }
    host->channel->Send(EncodeJson(Value(std::move(envelope))));
  }

  std::optional<ProxyFrame> Acquire(int64_t texture) {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    if (!ring_ || texture < 0 || texture != shown_texture_) return std::nullopt;
    FrameView view{};
    if (!ring_->Acquire(view)) return std::nullopt;
    if (events_ && (view.epoch != reported_epoch_ || (view.visible && !reported_visible_))) {
      if (view.epoch != reported_epoch_) reported_visible_ = false;
      reported_epoch_ = view.epoch;
      reported_visible_ = reported_visible_ || view.visible;
      events_->Send(EncodeJson(Value(Map{{Value("v"), Value(static_cast<int32_t>(kProtocolVersion))},
                                         {Value("event"), Value("presented")},
                                         {Value("epoch"), Value(static_cast<int64_t>(view.epoch))},
                                         {Value("visible"), Value(view.visible)}})));
    }
    return ProxyFrame{ring_, view.pixels, view.width, view.height};
  }

  void Shutdown() {
    if (shut_down_) return;
    shut_down_ = true;
    if (auto host = CurrentHost()) {
      host->intentional = true;
      host->channel->Send(EncodeJson(Value(Map{{Value("v"), Value(static_cast<int32_t>(kProtocolVersion))},
                                               {Value("event"), Value("shutdown")}})));
      if (!host->process->Wait(options_.shutdown_timeout)) host->process->Kill();
      Retire(host);
    }
    StopWatchdog();
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      ring_.reset();
      events_.reset();
      shown_texture_ = -1;
    }
    if (active_texture_ >= 0) platform_.unregister_texture(active_texture_);
    if (persistent_texture_ >= 0 && persistent_texture_ != active_texture_)
      platform_.unregister_texture(persistent_texture_);
    active_texture_ = persistent_texture_ = -1;
  }

  int64_t host_pid() const {
    const auto host = CurrentHost();
    return host ? host->process->pid() : 0;
  }

 private:
  struct Host {
    std::unique_ptr<ChildProcess> process;
    std::shared_ptr<Channel> channel;
    std::thread reader;
    std::mutex mutex;
    std::map<int64_t, Pending> pending;
    std::atomic<bool> finished{false};
    std::atomic<bool> intentional{false};
    std::string failure;
  };

  static int64_t Millis(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
  }

  std::shared_ptr<Host> CurrentHost() const {
    std::lock_guard<std::mutex> lock(host_mutex_);
    return host_;
  }

  std::shared_ptr<Host> Spawn() {
    auto host = std::make_shared<Host>();
    std::unique_ptr<Channel> channel;
    host->process = ChildProcess::Spawn(options_.host, options_.arguments, channel);
    if (!host->process) return nullptr;
    host->channel = std::move(channel);
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      ring_.reset();
      events_ = host->channel;
      reported_epoch_ = UINT32_MAX;
      reported_visible_ = false;
    }
    host_session_ = false;
    stop_deadline_ = 0;
    {
      std::lock_guard<std::mutex> lock(host_mutex_);
      host_ = host;
    }
    host->reader = std::thread([self = weak_from_this(), host] { Read(self, host); });
    return host;
  }

  static void Read(std::weak_ptr<Impl> weak, std::shared_ptr<Host> host) {
    bool invalid = false;
    while (auto message = host->channel->Receive()) {
      auto self = weak.lock();
      if (!self) break;
      try {
        if (!self->Dispatch(host, *message)) {
          invalid = true;
          break;
        }
      } catch (const std::exception&) {
        invalid = true;
        break;
      }
    }
    host->finished = true;
    auto self = weak.lock();
    if (!self) return;
    if (invalid && !host->intentional) {
      {
        std::lock_guard<std::mutex> lock(host->mutex);
        if (host->failure.empty()) host->failure = kLostMessage;
      }
      host->process->Kill();
    }
    self->platform_.post([weak, host] {
      if (auto owner = weak.lock()) owner->OnHostLost(host);
    });
  }

  bool Dispatch(const std::shared_ptr<Host>& host, Message& message) {
    const auto decoded = DecodeJson(message.json);
    const auto* envelope = std::get_if<Map>(&decoded);
    if (!envelope || IntegerField(*envelope, "v") != static_cast<int64_t>(kProtocolVersion)) return false;
    if (const auto id = IntegerField(*envelope, "id")) {
      Pending pending;
      {
        std::lock_guard<std::mutex> lock(host->mutex);
        const auto found = host->pending.find(*id);
        if (found == host->pending.end()) return false;
        pending = std::move(found->second);
        host->pending.erase(found);
      }
      ProxyResult result;
      if (const auto* value = Field(*envelope, "result")) {
        result = ProxyResult::Success(*value);
      } else if (const auto* error = Field(*envelope, "error")) {
        const auto* fields = std::get_if<Map>(error);
        const auto* code = fields ? Field(*fields, "code") : nullptr;
        const auto* text = fields ? Field(*fields, "message") : nullptr;
        result = ProxyResult::Error(code && std::holds_alternative<std::string>(*code) ? std::get<std::string>(*code) : "",
                                    text && std::holds_alternative<std::string>(*text) ? std::get<std::string>(*text) : "");
      } else if (Field(*envelope, "notImplemented")) {
        result.kind = ProxyResult::Kind::kNotImplemented;
      } else {
        return false;
      }
      const auto surface = IntegerField(*envelope, "surface").value_or(-1);
      const auto* session_flag = Field(*envelope, "session");
      const bool session = session_flag && std::holds_alternative<bool>(*session_flag) && std::get<bool>(*session_flag);
      auto shared = std::make_shared<std::pair<Pending, ProxyResult>>(std::move(pending), std::move(result));
      platform_.post([weak = weak_from_this(), host, shared, surface, session] {
        if (auto self = weak.lock()) self->OnReply(host, std::move(shared->first), std::move(shared->second), surface, session);
      });
      return true;
    }
    const auto* event = Field(*envelope, "event");
    const auto* name = event ? std::get_if<std::string>(event) : nullptr;
    if (!name) return false;
    if (*name == "frame") {
      const auto texture = IntegerField(*envelope, "texture");
      if (!texture) return false;
      platform_.frame_available(*texture);
      return true;
    }
    if (*name == "ring") {
      const auto size = IntegerField(*envelope, "size");
      if (!size || *size <= 0) return false;
#if defined(_WIN32)
      const auto remote = IntegerField(*envelope, "handle");
      if (!remote) return false;
      const auto handle = host->process->Duplicate(static_cast<uint64_t>(*remote));
      if (!handle) return false;
      auto memory = SharedMemory::Map(handle, static_cast<size_t>(*size));
      if (!memory) {
        CloseHandle(handle);
        return false;
      }
#else
      if (message.descriptors.size() != 1) return false;
      const int descriptor = message.descriptors.front();
      message.descriptors.clear();
      auto memory = SharedMemory::Map(descriptor, static_cast<size_t>(*size));
      if (!memory) {
        close(descriptor);
        return false;
      }
#endif
      auto reader = FrameRingReader::Attach(std::move(memory));
      if (!reader) return false;
      std::lock_guard<std::mutex> lock(frames_mutex_);
      if (events_ == host->channel) ring_ = std::move(reader);
      return true;
    }
    if (*name == "hello") {
      if (IntegerField(*envelope, "protocol") != static_cast<int64_t>(kProtocolVersion)) return false;
      host->process->Connected();
      return true;
    }
    return false;
  }

  void OnReply(const std::shared_ptr<Host>& host, Pending pending, ProxyResult result, int64_t surface, bool session) {
    if (host == CurrentHost()) {
      host_session_ = session;
      if (options_.texture_per_session) {
        if (pending.candidate >= 0 && surface != pending.candidate) platform_.unregister_texture(pending.candidate);
        if (active_texture_ >= 0 && active_texture_ != surface) {
          platform_.unregister_texture(active_texture_);
          active_texture_ = -1;
        }
        if (pending.candidate >= 0 && surface == pending.candidate) active_texture_ = surface;
        ShowTexture(active_texture_);
      } else if (pending.candidate >= 0) {
        ShowTexture(persistent_texture_);
      }
      if (!session) stop_deadline_ = 0;
      if (pending.method == "status") {
        const auto* status = std::get_if<Map>(&result.value);
        const auto* ended = status ? Field(*status, "ended") : nullptr;
        if (ended && std::holds_alternative<bool>(*ended) && std::get<bool>(*ended)) stop_deadline_ = 0;
      }
    }
    pending.reply(std::move(result));
  }

  void ShowTexture(int64_t texture) {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    shown_texture_ = texture;
  }

  void OnHostLost(const std::shared_ptr<Host>& host) {
    std::string reason;
    {
      std::lock_guard<std::mutex> lock(host->mutex);
      reason = host->failure.empty() ? kLostMessage : host->failure;
    }
    std::map<int64_t, Pending> pending;
    {
      std::lock_guard<std::mutex> lock(host->mutex);
      pending.swap(host->pending);
    }
    bool current = false;
    {
      std::lock_guard<std::mutex> lock(host_mutex_);
      if (host_ == host) {
        host_.reset();
        current = true;
      }
    }
    if (!host->process->Wait(std::chrono::milliseconds(2000))) host->process->Kill();
    if (host->reader.joinable() && host->reader.get_id() != std::this_thread::get_id()) host->reader.join();
    if (!current) return;
    bool session = host_session_;
    for (const auto& entry : pending) session = session || StartsHostWork(entry.second.method);
    host_session_ = false;
    stop_deadline_ = 0;
    if (!host->intentional && session) lost_ = reason;
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      events_.reset();
    }
    for (auto& entry : pending) {
      if (entry.second.candidate >= 0 && options_.texture_per_session && entry.second.candidate != active_texture_)
        platform_.unregister_texture(entry.second.candidate);
      entry.second.reply(LostReply(entry.second.method, reason));
    }
  }

  void ForgetLostSession() {
    lost_.reset();
    {
      std::lock_guard<std::mutex> lock(frames_mutex_);
      ring_.reset();
      if (options_.texture_per_session) shown_texture_ = -1;
    }
    if (options_.texture_per_session && active_texture_ >= 0) {
      platform_.unregister_texture(active_texture_);
      active_texture_ = -1;
    }
  }

  void Retire(const std::shared_ptr<Host>& host) {
    if (host->reader.joinable()) {
      host->process->Abandon();
#if defined(_WIN32)
      if (!host->finished) CancelSynchronousIo(host->reader.native_handle());
#endif
      host->reader.join();
    }
    std::lock_guard<std::mutex> lock(host_mutex_);
    if (host_ == host) host_.reset();
  }

  static void Watch(std::weak_ptr<Impl> weak) {
    while (true) {
      auto self = weak.lock();
      if (!self) return;
      {
        std::unique_lock<std::mutex> lock(self->watch_mutex_);
        if (self->watch_wake_.wait_for(lock, std::chrono::milliseconds(200), [&] { return self->watch_stop_; })) return;
      }
      const auto host = self->CurrentHost();
      if (!host || host->intentional) continue;
      const auto now = Clock::now();
      bool hung = false;
      {
        std::lock_guard<std::mutex> lock(host->mutex);
        for (const auto& entry : host->pending)
          hung = hung || now - entry.second.sent > self->options_.request_timeout;
      }
      const auto deadline = self->stop_deadline_.load();
      hung = hung || (deadline && Millis(now) > deadline);
      if (hung) {
        {
          std::lock_guard<std::mutex> lock(host->mutex);
          host->failure = kHungMessage;
        }
        host->process->Kill();
      }
      if (!host->finished && host->process->Wait(std::chrono::milliseconds(0))) host->process->Abandon();
    }
  }

  void StopWatchdog() {
    {
      std::lock_guard<std::mutex> lock(watch_mutex_);
      watch_stop_ = true;
    }
    watch_wake_.notify_all();
    if (watchdog_.joinable() && watchdog_.get_id() != std::this_thread::get_id()) watchdog_.join();
    if (watchdog_.joinable()) watchdog_.detach();
  }

  const ProxyOptions options_;
  const ProxyPlatform platform_;
  mutable std::mutex host_mutex_;
  std::shared_ptr<Host> host_;
  std::optional<std::string> lost_;
  bool host_session_ = false;
  bool shut_down_ = false;
  int64_t next_id_ = 0;
  int64_t active_texture_ = -1;
  int64_t persistent_texture_ = -1;
  std::atomic<int64_t> stop_deadline_{0};
  std::mutex frames_mutex_;
  std::shared_ptr<FrameRingReader> ring_;
  std::shared_ptr<Channel> events_;
  int64_t shown_texture_ = -1;
  uint32_t reported_epoch_ = UINT32_MAX;
  bool reported_visible_ = false;
  std::thread watchdog_;
  std::mutex watch_mutex_;
  std::condition_variable watch_wake_;
  bool watch_stop_ = false;
};

RetroHostProxy::RetroHostProxy(ProxyOptions options, ProxyPlatform platform)
    : impl_(std::make_shared<Impl>(std::move(options), std::move(platform))) {
  impl_->Start();
}

RetroHostProxy::~RetroHostProxy() { impl_->Shutdown(); }

void RetroHostProxy::Handle(const std::string& method, Value arguments, ProxyReply reply) {
  impl_->Handle(method, std::move(arguments), std::move(reply));
}

std::optional<ProxyFrame> RetroHostProxy::Acquire(int64_t texture) { return impl_->Acquire(texture); }

void RetroHostProxy::Shutdown() { impl_->Shutdown(); }

int64_t RetroHostProxy::host_pid() const { return impl_->host_pid(); }

std::filesystem::path RetroHostProxy::DefaultHostPath() {
#if defined(_WIN32)
  wchar_t buffer[32768];
  const auto length = GetEnvironmentVariableW(L"ANYPS5_RETRO_HOST", buffer, 32768);
  if (length > 0 && length < 32768) return std::filesystem::path(buffer);
  const auto size = GetModuleFileNameW(nullptr, buffer, 32768);
  if (!size || size >= 32768) return {};
  return std::filesystem::path(buffer).parent_path() / L"anyps5-retro-host.exe";
#else
  if (const char* configured = std::getenv("ANYPS5_RETRO_HOST"); configured && *configured)
    return std::filesystem::path(configured);
  char buffer[PATH_MAX];
  const auto size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (size <= 0) return {};
  buffer[size] = '\0';
  return std::filesystem::path(buffer).parent_path() / "anyps5-retro-host";
#endif
}

}  // namespace anyps5::retro_ipc
