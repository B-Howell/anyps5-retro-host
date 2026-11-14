// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "host.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "paths.h"
#include "platform.h"

#ifndef ANYPS5_RETRO_HOST_VERSION
#define ANYPS5_RETRO_HOST_VERSION "0.0.0"
#endif

namespace retro_host {
namespace {

constexpr int64_t kMaxId = 2147483647;
constexpr int64_t kWriteLimitMs = 5000;
constexpr int64_t kStallMs = 10000;
constexpr int64_t kFirstFrameMs = 60000;
constexpr int64_t kStopLimitMs = 5000;
constexpr int64_t kLeaveLimitMs = 10000;
// A state is saved or loaded between frames; the reply to saveState or loadState still comes
// within the 5 seconds a call has.
constexpr int64_t kStateLimitMs = 4000;
constexpr int64_t kMaxFrame = 4294967295LL;

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

struct ArgumentError {
  std::string message;
};

Host::Reply Failure(const std::string& code, const std::string& message) {
  Host::Reply reply;
  reply.error = Json(JsonObject{{"code", code}, {"message", message}});
  return reply;
}

Host::Reply Result(Json result) {
  Host::Reply reply;
  reply.result = std::move(result);
  return reply;
}

void Need(bool condition, const std::string& message) {
  if (!condition) throw ArgumentError{message};
}

std::string NeedPath(const Json& args, const char* field, const char* method = "start") {
  const Json* value = args.Find(field);
  Need(value && value->IsString() && IsProtocolPath(value->AsString(), kWindows),
       std::string(method) + "." + field + " must be an absolute path.");
  return value->AsString();
}

// One controller port's state as input and step give it: buttons and the four stick axes.
bool ParsePort(const Json& message, PortInput& input) {
  const Json* buttons = message.Find("buttons");
  if (!buttons) return false;
  const auto button_bits = buttons->Integer(0, 65535);
  if (!button_bits) return false;
  input.buttons = static_cast<uint16_t>(*button_bits);
  const char* const axes[] = {"leftX", "leftY", "rightX", "rightY"};
  for (int i = 0; i < 4; ++i) {
    const Json* axis = message.Find(axes[i]);
    if (!axis) return false;
    const auto value = axis->Integer(-32768, 32767);
    if (!value) return false;
    input.sticks[i] = static_cast<int16_t>(*value);
  }
  return true;
}

LockstepConfig NeedLockstep(const Json& args, const std::string& where) {
  Need(args.IsObject(), where + " must be an object.");
  const Json* check = args.Find("check");
  Need(check != nullptr, where + ".check must be an integer from 1 to 3600.");
  const auto every = check->Integer(1, 3600);
  Need(every.has_value(), where + ".check must be an integer from 1 to 3600.");
  LockstepConfig config;
  config.check = static_cast<uint32_t>(*every);
  if (const Json* paced = args.Find("paced")) {
    Need(paced->IsBool(), where + ".paced must be true or false.");
    config.paced = paced->AsBool();
  }
  if (const Json* rollback = args.Find("rollback")) {
    const std::string frames_error = where + ".rollback.frames must be an integer from 1 to " +
                                     std::to_string(kMaxRollbackFrames) + ".";
    Need(rollback->IsObject(), frames_error);
    const Json* frames = rollback->Find("frames");
    Need(frames != nullptr, frames_error);
    const auto count = frames->Integer(1, kMaxRollbackFrames);
    Need(count.has_value(), frames_error);
    config.rollback = static_cast<unsigned>(*count);
  }
  return config;
}

Json StateJson(const std::optional<StateResult>& result) {
  if (!result) return Json(false);
  return Json(JsonObject{{"frame", result->frame}, {"bytes", result->bytes}, {"micros", result->micros}});
}

std::string Hex(const std::optional<uint64_t>& value) {
  if (!value) return std::string();
  char text[17];
  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(*value));
  return text;
}

}  // namespace

int Host::Run() {
  std::thread([this] { Watch(); }).detach();
  for (;;) {
    std::string body;
    const ReadResult result = channel_.Read(body);
    if (result == ReadResult::kEnd) return ShutDown(kExitClean);
    if (result == ReadResult::kProtocolError) return ShutDown(kExitProtocolError);
    const auto message = ParseJson(body);
    if (!message || !message->IsObject()) return ShutDown(kExitProtocolError);
    switch (Handle(*message)) {
      case Outcome::kContinue: break;
      case Outcome::kExit: return ShutDown(kExitClean);
      case Outcome::kProtocolError: return ShutDown(kExitProtocolError);
    }
  }
}

Host::Outcome Host::Handle(const Json& message) {
  const Json* type = message.Find("type");
  if (!type || !type->IsString()) return Outcome::kProtocolError;
  const std::string& kind = type->AsString();
  if (!greeted_) return kind == "hello" ? Hello(message) : Outcome::kProtocolError;
  if (kind == "hello") return Outcome::kProtocolError;
  if (kind == "call") return Call(message);
  if (kind == "input") return Input(message);
  if (kind == "step") return Step(message);
  if (kind == "correct") return Correct(message);
  if (kind == "ping") {
    const Json* id = message.Find("id");
    if (!id || !id->Integer(1, kMaxId)) return Outcome::kProtocolError;
    Send(Json(JsonObject{{"type", "pong"}, {"id", *id}}));
    return Outcome::kContinue;
  }
  if (kind == "shutdown") return Outcome::kExit;
  return Outcome::kContinue;
}

Host::Outcome Host::Hello(const Json& message) {
  const Json* min_version = message.Find("minVersion");
  const Json* max_version = message.Find("maxVersion");
  const Json* formats = message.Find("pixelFormats");
  if (!min_version || !max_version || !formats || !formats->IsArray()) return Outcome::kProtocolError;
  const auto low = min_version->Integer(1, 4294967295LL);
  const auto high = max_version->Integer(1, 4294967295LL);
  if (!low || !high || *low > *high) return Outcome::kProtocolError;
  std::optional<PixelFormat> format;
  std::string format_name;
  for (const auto& entry : formats->AsArray()) {
    if (!entry.IsString()) return Outcome::kProtocolError;
    if (format) continue;
    if (entry.AsString() == "rgba8") format = PixelFormat::kRgba8;
    if (entry.AsString() == "bgra8") format = PixelFormat::kBgra8;
    if (format) format_name = entry.AsString();
  }
  if (*low > 1 || *high < 1 || !format) {
    Send(Json(JsonObject{{"type", "unsupported"},
                         {"minVersion", 1},
                         {"maxVersion", 1},
                         {"pixelFormats", JsonArray{"rgba8", "bgra8"}}}));
    return Outcome::kExit;
  }
  greeted_ = true;
  rings_ = std::make_unique<FrameRings>(*format);
  Send(Json(JsonObject{{"type", "welcome"},
                       {"version", 1},
                       {"pid", ProcessId()},
                       {"hostVersion", ANYPS5_RETRO_HOST_VERSION},
                       {"capabilities", JsonArray{"ps1", "ps1Lockstep", "ps1Rollback"}},
                       {"pixelFormat", format_name}}));
  return Outcome::kContinue;
}

Host::Outcome Host::Call(const Json& message) {
  const Json* id = message.Find("id");
  const Json* method = message.Find("method");
  if (!id || !id->Integer(1, kMaxId) || !method || !method->IsString()) return Outcome::kProtocolError;
  const Json* args = message.Find("args");
  const Json none;
  Reply reply;
  try {
    reply = Dispatch(method->AsString(), args ? *args : none);
  } catch (const ArgumentError& error) {
    reply = Failure("argument", error.message);
  }
  Json answer = reply.error ? Json(JsonObject{{"type", "reply"}, {"id", *id}, {"error", *reply.error}})
                            : Json(JsonObject{{"type", "reply"}, {"id", *id}, {"result", reply.result}});
  if (answer.Serialize().size() > kMaxMessage) {
    answer = Json(JsonObject{{"type", "reply"},
                             {"id", *id},
                             {"error", JsonObject{{"code", "tooLarge"},
                                                  {"message", "The result does not fit in one message."}}}});
  }
  Send(answer);
  if (reply.after) reply.after();
  if (reply.exit_after) ExitNow(kExitMissedLimit);
  return Outcome::kContinue;
}

Host::Outcome Host::Input(const Json& message) {
  const Json* port = message.Find("port");
  if (!port) return Outcome::kProtocolError;
  const auto port_index = port->Integer(0, 1);
  PortInput input;
  if (!port_index || !ParsePort(message, input)) return Outcome::kProtocolError;
  const unsigned index = static_cast<unsigned>(*port_index);
  inputs_[index] = input;
  if (auto game = CurrentGame()) game->SetInput(index, input);
  return Outcome::kContinue;
}

Host::Outcome Host::Step(const Json& message) {
  const Json* frame = message.Find("frame");
  const Json* ports = message.Find("ports");
  if (!frame) return Outcome::kProtocolError;
  const auto number = frame->Integer(0, kMaxFrame);
  if (!number || !ports || !ports->IsArray() || ports->AsArray().empty() || ports->AsArray().size() > 2) {
    return Outcome::kProtocolError;
  }
  StepInput input;
  input.connected = static_cast<unsigned>(ports->AsArray().size());
  for (size_t port = 0; port < input.connected; ++port) {
    const Json& entry = ports->AsArray()[port];
    if (entry.IsNull()) {
      input.known[port] = false;
    } else if (!ParsePort(entry, input.ports[port])) {
      return Outcome::kProtocolError;
    }
  }
  const auto game = CurrentGame();
  if (!game) return Outcome::kProtocolError;
  return game->Step(static_cast<uint64_t>(*number), input) == StepResult::kRefused ? Outcome::kProtocolError
                                                                                  : Outcome::kContinue;
}

Host::Outcome Host::Correct(const Json& message) {
  const Json* frame = message.Find("frame");
  const Json* port = message.Find("port");
  if (!frame || !port) return Outcome::kProtocolError;
  const auto number = frame->Integer(0, kMaxFrame);
  const auto port_index = port->Integer(0, 1);
  PortInput input;
  if (!number || !port_index || !ParsePort(message, input)) return Outcome::kProtocolError;
  const auto game = CurrentGame();
  if (!game) return Outcome::kProtocolError;
  return game->Correct(static_cast<uint64_t>(*number), static_cast<unsigned>(*port_index), input) ==
                 StepResult::kRefused
             ? Outcome::kProtocolError
             : Outcome::kContinue;
}

Host::Reply Host::Dispatch(const std::string& method, const Json& args) {
  const auto game = CurrentGame();
  if (method == "start") return Start(args);
  if (method == "status") {
    Need(args.IsNull(), "status takes no argument.");
    return Result(game ? game->Status() : Json(nullptr));
  }
  if (method == "pause") {
    Need(args.IsBool(), "pause takes true or false.");
    if (game) game->Pause(args.AsBool());
    return Result(Json(nullptr));
  }
  if (method == "connectedPorts") {
    const auto count = args.Integer(1, 2);
    Need(count.has_value(), "connectedPorts takes 1 or 2.");
    return Result(Json(game ? game->SetConnectedPorts(static_cast<unsigned>(*count)) : false));
  }
  if (method == "changeDisc") {
    const auto index = args.Integer(0, 127);
    Need(index.has_value(), "changeDisc takes a disc index from 0 to 127.");
    return Result(Json(game ? game->ChangeDisc(static_cast<unsigned>(*index)) : false));
  }
  if (method == "option") {
    const Json* key = args.Find("key");
    const Json* value = args.Find("value");
    Need(key && key->IsString() && value && value->IsString(), "option takes a key and a value.");
    return Result(Json(game ? game->SetOption(key->AsString(), value->AsString()) : false));
  }
  if (method == "volume") {
    const Json* level = args.Find("level");
    const Json* muted = args.Find("muted");
    Need(level && level->IsNumber() && level->AsNumber() >= 0 && level->AsNumber() <= 1 && muted && muted->IsBool(),
         "volume takes a level from 0 to 1 and muted.");
    volume_ = static_cast<float>(level->AsNumber());
    muted_ = muted->AsBool();
    audio_.SetVolume(volume_, muted_);
    return Result(Json(nullptr));
  }
  if (method == "lockstep") {
    if (args.IsBool() && !args.AsBool()) return Result(Json(game ? game->LeaveLockstep() : false));
    const LockstepConfig config = NeedLockstep(args, "lockstep");
    const auto frame = game ? game->Lockstep(config) : std::nullopt;
    return Result(frame ? Json(JsonObject{{"frame", *frame}}) : Json(false));
  }
  if (method == "saveState" || method == "loadState") return State(method == "saveState", args);
  if (method == "stop") {
    Need(args.IsNull(), "stop takes no argument.");
    if (game && !game->Ended()) {
      game->Stop();
      int64_t none = -1;
      stop_requested_at_.compare_exchange_strong(none, NowMs());
    }
    return Result(Json(nullptr));
  }
  if (method == "release") {
    Need(args.IsNull(), "release takes no argument.");
    if (!game || !game->Ended()) return Result(Json(nullptr));
    Reply reply = Result(Json(nullptr));
    if (!game->Join(1000)) {
      abandoned_ = game;
      reply.exit_after = true;
    }
    {
      std::lock_guard<std::mutex> lock(game_mutex_);
      game_.reset();
    }
    stop_requested_at_ = -1;
    rings_->Clear();
    return reply;
  }
  return Failure("unknownMethod", "This host has no such method.");
}

Host::Reply Host::Start(const Json& args) {
  Need(args.IsObject(), "start takes an object.");
  GameConfig config;
  config.core = NeedPath(args, "core");
  config.disc = NeedPath(args, "disc");
  config.system = NeedPath(args, "system");
  config.saves = NeedPath(args, "saves");
  const bool playlist = HasExtension(config.disc, ".m3u");
  Need(playlist || HasExtension(config.disc, ".cue") || HasExtension(config.disc, ".chd") ||
           HasExtension(config.disc, ".pbp"),
       "start.disc must be a .cue, .chd, .pbp or .m3u file.");
  if (const Json* options = args.Find("options")) {
    Need(options->IsObject(), "start.options must be an object of strings.");
    for (const auto& [key, value] : options->AsObject()) {
      Need(value.IsString(), "start.options must be an object of strings.");
      config.options.emplace_back(key, value.AsString());
    }
  }
  if (const Json* initial = args.Find("initialDisc")) {
    const auto index = initial->Integer(0, 127);
    Need(index.has_value(), "start.initialDisc must be an integer from 0 to 127.");
    Need(playlist, "initialDisc needs an .m3u disc.");
    config.initial_disc = static_cast<unsigned>(*index);
  }
  if (const Json* lockstep = args.Find("lockstep")) config.lockstep = NeedLockstep(*lockstep, "start.lockstep");
  if (CurrentGame()) return Failure("busy", "A game is already loaded.");
  if (games_started_ >= kMaxId) return Failure("start", "This host has started its last game.");
  config.epoch = games_started_ + 1;
  rings_->Clear();
  auto game = std::make_shared<Game>(config, rings_->format(), *this, audio_);
  for (unsigned port = 0; port < 2; ++port) game->SetInput(port, inputs_[port]);
  ++games_started_;
  {
    std::lock_guard<std::mutex> lock(game_mutex_);
    game_ = std::move(game);
  }
  Reply reply = Result(Json(JsonObject{{"epoch", config.epoch}}));
  reply.after = [started = CurrentGame()] { started->Begin(); };
  return reply;
}

Host::Reply Host::State(bool save, const Json& args) {
  const char* method = save ? "saveState" : "loadState";
  Need(args.IsObject(), std::string(method) + " takes an object.");
  const Json* slot = args.Find("slot");
  Need((slot == nullptr) != (args.Find("path") == nullptr), std::string(method) + " takes a path or a slot.");
  StateTarget target;
  if (slot) {
    const auto index = slot->Integer(0, kStateSlots - 1);
    Need(index.has_value(), std::string(method) + ".slot must be an integer from 0 to " +
                                std::to_string(kStateSlots - 1) + ".");
    target.slot = static_cast<unsigned>(*index);
  } else {
    target.path = NeedPath(args, "path", method);
  }
  const auto game = CurrentGame();
  if (!game) return Result(Json(false));
  return Result(StateJson(save ? game->SaveState(target, kStateLimitMs) : game->LoadState(target, kStateLimitMs)));
}

FrameResult Host::OnFrame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch) {
  auto growth = rings_->Prepare(width, height);
  if (!growth.fits) return FrameResult::kTooLarge;
  if (growth.no_memory) return FrameResult::kNoMemory;
  if (growth.ring) {
    JsonObject ring{{"type", "ring"}, {"bytes", growth.ring->bytes}};
#ifdef _WIN32
    ring.emplace_back("handle", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(growth.ring->handle)));
    Send(Json(std::move(ring)));
#else
    Send(Json(std::move(ring)), growth.ring->handle);
#endif
  }
  rings_->Publish(pixels, width, height, epoch);
  Send(Json(JsonObject{{"type", "frame"}}));
  return FrameResult::kShown;
}

void Host::OnRumble(unsigned port, uint16_t strong, uint16_t weak) {
  Send(Json(JsonObject{{"type", "rumble"}, {"port", port}, {"strong", strong}, {"weak", weak}}));
}

void Host::OnSync(const SyncPoint& point) {
  JsonObject sync{{"type", "sync"}, {"frame", point.frame}};
  const std::pair<const char*, const std::optional<uint64_t>&> hashes[] = {
      {"ram", point.ram}, {"video", point.video}, {"state", point.state}};
  for (const auto& [name, hash] : hashes) {
    if (hash) sync.emplace_back(name, Hex(hash));
    else sync.emplace_back(name, nullptr);
  }
  Send(Json(std::move(sync)));
}

void Host::OnRollback(const RollbackReport& report) {
  Send(Json(JsonObject{{"type", "rollback"},
                       {"frame", report.frame},
                       {"frames", report.frames},
                       {"micros", report.micros},
                       {"loadMicros", report.load_micros}}));
}

void Host::Send(const Json& message, NativeHandle memory) {
  if (launcher_gone_) return;
  if (channel_.Write(message.Serialize(), memory)) return;
  launcher_gone_ = true;
  std::call_once(leaving_once_, [this] { std::thread([this] { LauncherGone(); }).detach(); });
}

std::shared_ptr<Game> Host::CurrentGame() const {
  std::lock_guard<std::mutex> lock(game_mutex_);
  return game_;
}

int Host::ShutDown(int code) {
  std::lock_guard<std::mutex> once(shutdown_mutex_);
  int64_t none = -1;
  leaving_since_.compare_exchange_strong(none, NowMs());
  if (auto game = CurrentGame()) {
    game->Stop();
    if (!game->Join(kLeaveLimitMs - 1000)) ExitNow(kExitMissedLimit);
  }
  return code;
}

void Host::LauncherGone() {
  launcher_gone_ = true;
  ExitNow(ShutDown(kExitClean));
}

void Host::Watch() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (channel_.WriteAge() > kWriteLimitMs) ExitNow(kExitMissedLimit);
    const int64_t leaving = leaving_since_;
    if (leaving >= 0 && NowMs() - leaving > kLeaveLimitMs) ExitNow(kExitMissedLimit);
    const auto game = CurrentGame();
    if (!game) continue;
    const int64_t stop_at = stop_requested_at_;
    if (stop_at >= 0 && !game->Finished() && NowMs() - stop_at > kStopLimitMs) ExitNow(kExitMissedLimit);
    if (game->Ended()) continue;
    if (game->Ready() && game->QuietMs() > kStallMs) {
      game->Fail("The emulator stopped responding.", "stalled");
    } else if (!game->Ready() && game->QuietMs() > kFirstFrameMs) {
      game->Fail("The game did not start.", "didNotStart");
    }
  }
}

}  // namespace retro_host
