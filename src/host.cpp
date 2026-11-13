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

std::string NeedPath(const Json& args, const char* field) {
  const Json* value = args.Find(field);
  Need(value && value->IsString() && IsProtocolPath(value->AsString(), kWindows),
       std::string("start.") + field + " must be an absolute path.");
  return value->AsString();
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
                       {"capabilities", JsonArray{"ps1"}},
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
  const Json* buttons = message.Find("buttons");
  if (!port || !buttons) return Outcome::kProtocolError;
  const auto port_index = port->Integer(0, 1);
  const auto button_bits = buttons->Integer(0, 65535);
  if (!port_index || !button_bits) return Outcome::kProtocolError;
  PortInput input;
  input.buttons = static_cast<uint16_t>(*button_bits);
  const char* const axes[] = {"leftX", "leftY", "rightX", "rightY"};
  for (int i = 0; i < 4; ++i) {
    const Json* axis = message.Find(axes[i]);
    const auto value = axis ? axis->Integer(-32768, 32767) : std::nullopt;
    if (!value) return Outcome::kProtocolError;
    input.sticks[i] = static_cast<int16_t>(*value);
  }
  inputs_[*port_index] = input;
  if (auto game = CurrentGame()) game->SetInput(static_cast<unsigned>(*port_index), input);
  return Outcome::kContinue;
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
