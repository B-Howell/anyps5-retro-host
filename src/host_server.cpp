#include "host_server.h"

#include "retro_ipc_json.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>

namespace anyps5::retro_host {
namespace {
constexpr size_t kInitialSlotBytes = 1280u * 960u * 4u;
constexpr auto kShutdownGrace = std::chrono::seconds(20);

Value Envelope(Map fields) {
  fields.emplace(Value("v"), Value(static_cast<int32_t>(retro_ipc::kProtocolVersion)));
  return Value(std::move(fields));
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
}  // namespace

int64_t ChannelSurface::Register() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (requested_ < 0) return -1;
  ClearLocked();
  active_ = requested_;
  return active_;
}

void ChannelSurface::Release(int64_t texture) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (texture != active_) return;
  ClearLocked();
  active_ = -1;
}

void ChannelSurface::ClearLocked() {
  epoch_ = ring_ ? ring_->Clear() : epoch_ + 1;
}

int64_t ChannelSurface::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

uint32_t ChannelSurface::epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_;
}

void ChannelSurface::Publish(int64_t texture, const uint8_t* rgba, size_t width, size_t height, bool visible) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (texture < 0 || texture != active_ || !rgba || !width || !height) return;
  const auto bytes = width * height * 4;
  if (!ring_ || ring_->slot_bytes() < bytes) {
    auto ring = retro_ipc::FrameRingWriter::Create(std::max(bytes, kInitialSlotBytes), epoch_);
    if (!ring) return;
    Map event{{Value("event"), Value("ring")},
              {Value("size"), Value(static_cast<int64_t>(ring->memory().size()))}};
#if defined(_WIN32)
    event.emplace(Value("handle"), Value(static_cast<int64_t>(reinterpret_cast<uintptr_t>(ring->memory().handle()))));
    if (!channel_.Send(retro_ipc::EncodeJson(Envelope(std::move(event))))) return;
#else
    if (!channel_.Send(retro_ipc::EncodeJson(Envelope(std::move(event))), ring->memory().handle())) return;
#endif
    ring_ = std::move(ring);
  }
  if (!ring_->Write(rgba, width, height, visible)) return;
  channel_.Send(retro_ipc::EncodeJson(Envelope(Map{{Value("event"), Value("frame")}, {Value("texture"), Value(texture)}})));
}

int RunHost(std::unique_ptr<retro_ipc::Channel> channel, const HostOptions& options) {
  ChannelSurface surface(*channel);
  auto backend = CreateBackend(surface, options);
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::optional<Value>> inbox;
  std::thread reader([&] {
    while (auto message = channel->Receive()) {
      std::optional<Value> decoded;
      try {
        decoded = retro_ipc::DecodeJson(message->json);
      } catch (const std::exception&) {
        break;
      }
      std::lock_guard<std::mutex> lock(mutex);
      inbox.push_back(std::move(decoded));
      ready.notify_one();
    }
    std::lock_guard<std::mutex> lock(mutex);
    inbox.push_back(std::nullopt);
    ready.notify_one();
  });
  channel->Send(retro_ipc::EncodeJson(Envelope(Map{{Value("event"), Value("hello")},
                                                   {Value("protocol"), Value(static_cast<int32_t>(retro_ipc::kProtocolVersion))},
                                                   {Value("pid"), Value(retro_ipc::CurrentProcessId())}})));
  while (true) {
    std::optional<Value> next;
    {
      std::unique_lock<std::mutex> lock(mutex);
      ready.wait(lock, [&] { return !inbox.empty(); });
      next = std::move(inbox.front());
      inbox.pop_front();
    }
    const auto* envelope = next ? std::get_if<Map>(&*next) : nullptr;
    if (!envelope || IntegerField(*envelope, "v") != static_cast<int64_t>(retro_ipc::kProtocolVersion)) break;
    if (const auto* event = Field(*envelope, "event")) {
      const auto* name = std::get_if<std::string>(event);
      if (!name || *name == "shutdown") break;
      if (*name == "presented") {
        const auto epoch = IntegerField(*envelope, "epoch");
        const auto* visible = Field(*envelope, "visible");
        if (epoch && *epoch == surface.epoch() && visible && std::holds_alternative<bool>(*visible))
          backend->Presented(std::get<bool>(*visible));
      }
      continue;
    }
    const auto id = IntegerField(*envelope, "id");
    const auto* method = Field(*envelope, "method");
    if (!id || !method || !std::holds_alternative<std::string>(*method)) break;
    const auto* arguments = Field(*envelope, "args");
    surface.set_requested(IntegerField(*envelope, "texture").value_or(-1));
    auto response = std::make_shared<MethodResult::Response>();
    try {
      backend->Handle(MethodCall(std::get<std::string>(*method), arguments ? *arguments : Value()),
                      std::make_unique<MethodResult>(response));
    } catch (const std::exception& failure) {
      if (response->kind == MethodResult::Response::Kind::kNone)
        *response = {MethodResult::Response::Kind::kError, Value(), "retroCore", failure.what()};
    }
    surface.set_requested(-1);
    Map reply{{Value("id"), Value(*id)},
              {Value("surface"), Value(surface.active())},
              {Value("session"), Value(backend->HasSession())}};
    switch (response->kind) {
      case MethodResult::Response::Kind::kNone:
      case MethodResult::Response::Kind::kSuccess:
        reply.emplace(Value("result"), std::move(response->value));
        break;
      case MethodResult::Response::Kind::kError:
        reply.emplace(Value("error"), Value(Map{{Value("code"), Value(response->code)},
                                                {Value("message"), Value(response->message)}}));
        break;
      case MethodResult::Response::Kind::kNotImplemented:
        reply.emplace(Value("notImplemented"), Value(true));
        break;
    }
    if (!channel->Send(retro_ipc::EncodeJson(Envelope(std::move(reply))))) break;
  }
  std::thread([] {
    std::this_thread::sleep_for(kShutdownGrace);
    std::_Exit(3);
  }).detach();
  backend->Shutdown();
  backend.reset();
  channel->CloseWrite();
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(0);
}

}  // namespace anyps5::retro_host
