// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_HOST_H_
#define ANYPS5_RETRO_HOST_HOST_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "audio.h"
#include "channel.h"
#include "game.h"
#include "json.h"
#include "ring.h"

namespace retro_host {

constexpr int kExitClean = 0;
constexpr int kExitProtocolError = 2;
constexpr int kExitMissedLimit = 3;

// The host side of protocol.md, version 1, capability ps1.
class Host final : public GameEvents {
 public:
  explicit Host(Channel& channel) : channel_(channel) {}

  // Serves the connection until it ends; returns the process exit code.
  int Run();

  // The launcher has gone (end-of-stream, its exit, or SIGTERM): stop the game as shutdown does.
  void LauncherGone();

  bool OnFrame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch) override;
  void OnRumble(unsigned port, uint16_t strong, uint16_t weak) override;

  // A method's answer: its result, or an error object. exit_after ends the host once the reply
  // is sent (a release whose core cannot be stopped).
  struct Reply {
    Json result;
    std::optional<Json> error;
    bool exit_after = false;
  };

 private:
  enum class Outcome { kContinue, kExit, kProtocolError };

  Outcome Handle(const Json& message);
  Outcome Hello(const Json& message);
  Outcome Call(const Json& message);
  Outcome Input(const Json& message);
  Reply Dispatch(const std::string& method, const Json& args);
  Reply Start(const Json& args);
  void Send(const Json& message, NativeHandle memory = kNoHandle);
  int ShutDown(int code);
  void Watch();
  std::shared_ptr<Game> CurrentGame() const;

  Channel& channel_;
  AudioOutput audio_;
  std::unique_ptr<FrameRings> rings_;
  bool greeted_ = false;
  uint32_t games_started_ = 0;
  float volume_ = 1.0f;
  bool muted_ = false;
  PortInput inputs_[2];

  mutable std::mutex game_mutex_;
  std::shared_ptr<Game> game_;
  std::atomic<int64_t> stop_requested_at_{-1};
  std::atomic<int64_t> leaving_since_{-1};
  std::atomic<bool> launcher_gone_{false};
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_HOST_H_
