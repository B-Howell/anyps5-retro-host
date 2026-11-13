// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_GAME_H_
#define ANYPS5_RETRO_HOST_GAME_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "json.h"
#include "platform.h"
#include "ring.h"

struct retro_game_geometry;

namespace retro_host {

struct GameConfig {
  std::string core;
  std::string disc;
  std::string system;
  std::string saves;
  std::vector<std::pair<std::string, std::string>> options;
  std::optional<unsigned> initial_disc;
  uint32_t epoch = 0;
};

struct PortInput {
  uint16_t buttons = 0;
  std::array<int16_t, 4> sticks{};
};

// What a running game hands back to the host: frames to publish and rumble to forward. Called
// on the game's thread.
enum class FrameResult { kShown, kTooLarge, kNoMemory };

class GameEvents {
 public:
  virtual ~GameEvents() = default;
  // pixels are rows of width x 4 bytes in the ring's pixel format. A frame no ring can hold, or
  // one the host cannot make memory for, ends the game.
  virtual FrameResult OnFrame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch) = 0;
  virtual void OnRumble(unsigned port, uint16_t strong, uint16_t weak) = 0;
};

// One loaded game: a libretro core running on its own thread (protocol.md "Methods").
class Game {
 public:
  Game(GameConfig config, PixelFormat format, GameEvents& events, AudioOutput& audio);
  ~Game();

  // Starts the game's thread, once the reply to start has been sent, so no frame of this game
  // reaches the launcher before its epoch does. The thread opens the core and the disc; when it
  // cannot, the game ends with errorCode core or disc.
  void Begin();

  Json Status() const;
  void Pause(bool paused);
  bool SetConnectedPorts(unsigned count);
  bool ChangeDisc(unsigned index);
  bool SetOption(const std::string& key, const std::string& value);
  void SetInput(unsigned port, const PortInput& input);

  // Asks the game to end. Ended() turns true once the core is unloaded.
  void Stop();
  bool Ended() const;
  // Whether the game's thread has unloaded the core and returned.
  bool Finished() const;

  // Ends the game with this error and code, as a game limit does (protocol.md "Timeouts").
  void Fail(const std::string& error, const std::string& code);

  // Joins the game's thread if it finishes within timeout_ms; false when it has not (a core that
  // is stuck in retro_run never does). A game whose thread has not finished must not be
  // destroyed: the destructor joins it.
  bool Join(int64_t timeout_ms);

  // Milliseconds of running, not paused, time since the core's last frame (or since start,
  // before the first one), and whether the first frame has been published.
  int64_t QuietMs() const;
  bool Ready() const;

  // Called by the libretro callbacks; public only for them.
  bool Environment(unsigned command, void* data);
  void Video(const void* data, unsigned width, unsigned height, size_t pitch);
  size_t AudioBatch(const int16_t* samples, size_t frames);
  int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);
  void Log(int level, const char* text);
  bool Rumble(unsigned port, int effect, uint16_t strength);

 private:
  struct Option {
    std::string key;
    std::string title;
    std::vector<std::string> values;
    std::vector<std::string> labels;
    size_t selected = 0;
    std::string active;
    bool visible = true;
    // Applied only when the game starts again; libretro cores mark these with "(Restart)" at the
    // end of the option's text.
    bool restart = false;
  };
  struct Disc {
    std::vector<std::string> labels;
    std::vector<std::string> paths;
    std::optional<unsigned> inserted;
    std::string played;
    bool busy = false;
    bool failed = false;
  };

  void Run();
  void RunGame();
  bool LoadCore(std::string& error);
  // Takes the core's aspect ratio, or base_width / base_height when it gives none (libretro).
  void SetAspect(const ::retro_game_geometry& geometry);
  void Finish(const std::string& error, const std::string& code);
  void ApplyPendingWork();
  // Writes memory card 1 when it changed since the last write, or always; false when it could
  // not be written. A card file the host could not read is never written over.
  bool SaveCard(bool always = false);
  std::string ResolutionError() const;
  std::string CardPath() const;
  void RefreshDiscs();
  std::optional<std::string> PlaylistEntry(unsigned index) const;
  template <typename T>
  T Get(const char* name) const {
    return reinterpret_cast<T>(library_.Symbol(name));
  }

  GameConfig config_;
  PixelFormat format_;
  GameEvents& events_;
  AudioOutput& audio_;
  Library library_;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::mutex join_mutex_;
  std::condition_variable finished_signal_;
  bool ready_ = false;
  bool ended_ = false;
  bool finished_ = false;
  std::string error_;
  std::string error_code_;
  double aspect_ = 4.0 / 3.0;
  std::vector<Option> options_;
  bool options_changed_ = false;
  Disc disc_;
  unsigned connected_ports_ = 1;
  bool ports_changed_ = false;
  std::optional<unsigned> disc_request_;
  std::array<PortInput, 2> input_{};
  std::array<std::array<uint16_t, 2>, 2> rumble_{};

  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> quiet_since_{0};
  std::atomic<int64_t> paused_at_{-1};

  int core_pixel_format_ = 0;
  double fps_ = 60.0;
  double sample_rate_ = 0.0;
  bool timing_changed_ = false;
  std::vector<uint8_t> converted_;
  FrameResult frame_failure_ = FrameResult::kShown;
  bool published_ = false;
  bool card_kept_ = true;
  bool (*update_display_)() = nullptr;
  bool display_refresh_ = false;
  mutable std::mutex pause_mutex_;
  bool bios_missing_ = false;
  std::string card_;
  struct DiscControl;
  std::unique_ptr<DiscControl> disc_control_;
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_GAME_H_
