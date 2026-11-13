// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_GAME_H_
#define ANYPS5_RETRO_HOST_GAME_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio.h"
#include "gpu.h"
#include "json.h"
#include "platform.h"
#include "ring.h"

struct retro_game_geometry;

namespace retro_host {

// How a game in lockstep runs (protocol.md "lockstep"): one frame per step the launcher sends, a
// sync message every check frames, and, when paced, no faster than the core's frame rate.
struct LockstepConfig {
  uint32_t check = 1;
  bool paced = true;
};

struct GameConfig {
  std::string core;
  std::string disc;
  std::string system;
  std::string saves;
  std::vector<std::pair<std::string, std::string>> options;
  std::optional<unsigned> initial_disc;
  std::optional<LockstepConfig> lockstep;
  uint32_t epoch = 0;
};

struct PortInput {
  uint16_t buttons = 0;
  std::array<int16_t, 4> sticks{};
};

// One lockstep frame's input: how many controller ports have a controller in, counting from port
// 1, and their state. A port without a controller reads as none.
struct StepInput {
  unsigned connected = 1;
  std::array<PortInput, 2> ports{};
};

// What a lockstep frame left behind, as 64-bit FNV-1a hashes: the console's main RAM, the last
// picture published and the core's serialized state. An absent hash is one the core cannot give.
struct SyncPoint {
  uint64_t frame = 0;
  std::optional<uint64_t> ram;
  std::optional<uint64_t> video;
  std::optional<uint64_t> state;
};

// What became of a step (protocol.md "step"): queued, dropped because the game has ended, or
// refused because the game is not in lockstep, the frame is not the next one, or too many wait.
enum class StepResult { kTaken, kIgnored, kRefused };

// The lockstep frame a saved or loaded state belongs to, or nothing when it could not be done.
struct StateResult {
  uint64_t frame = 0;
  uint64_t bytes = 0;
};

// What a running game hands back to the host: frames to publish and rumble to forward. Called
// on the game's thread.
enum class FrameResult { kShown, kTooLarge, kNoMemory, kGpuFailed };

class GameEvents {
 public:
  virtual ~GameEvents() = default;
  // pixels are rows of width x 4 bytes in the ring's pixel format. A frame no ring can hold, or
  // one the host cannot make memory for, ends the game.
  virtual FrameResult OnFrame(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch) = 0;
  virtual void OnRumble(unsigned port, uint16_t strong, uint16_t weak) = 0;
  virtual void OnSync(const SyncPoint& point) = 0;
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

  // Puts a running game in lockstep, or changes how it runs there; the frame count starts at 0
  // when it enters. Returns the next frame the game will run, or nothing when it has ended.
  std::optional<uint64_t> Lockstep(const LockstepConfig& config);
  // Leaves lockstep and drops the steps not yet run; false when the game has ended.
  bool LeaveLockstep();
  // Queues the input for one lockstep frame.
  StepResult Step(uint64_t frame, const StepInput& input);
  // Writes or reads the core's state, with its lockstep frame, between frames (protocol.md
  // "saveState" and "loadState"). Nothing when the game is not in lockstep, has ended, the core
  // refused, or the game's thread did not get to it within timeout_ms.
  std::optional<StateResult> SaveState(const std::string& path, int64_t timeout_ms);
  std::optional<StateResult> LoadState(const std::string& path, int64_t timeout_ms);

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
  uintptr_t CurrentFramebuffer() const;
  void* ProcAddress(const char* name) const;

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
    // Kept at its default and not listed in status (see kFixedOptions).
    bool fixed = false;
    // The core has asked for this option's value at least once.
    bool read = false;
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
  // Hands the frame in converted_ to the host.
  void Publish(uint32_t width, uint32_t height);
  // Tells the core its OpenGL context is going away, then releases it.
  void DropGpu();
  // Takes the core's aspect ratio, or base_width / base_height when it gives none (libretro).
  void SetAspect(const ::retro_game_geometry& geometry);
  void Finish(const std::string& error, const std::string& code);
  void ApplyPendingWork();
  // Writes memory card 1 when it changed since the last write, or always; false when it could
  // not be written. A card file the host could not read is never written over.
  bool SaveCard(bool always = false);
  std::string ResolutionError() const;
  // Offers only the internal resolutions whose frames fit the ring and the GPU's framebuffer, given
  // the core's largest geometry now, and moves a value that does not fit to the largest one that
  // does. When the value the core drew at is not offered, the core is given the selected one
  // instead, and the largest geometry at it is returned; otherwise std::nullopt, as the core's
  // geometry fits. Called with mutex_ held, on the game's thread, with gpu_ made.
  std::optional<std::pair<uint32_t, uint32_t>> LimitResolution(uint32_t max_width, uint32_t max_height);
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
  // Why the internal resolution asked for is not used, while the game uses another.
  std::string resolution_refused_;
  // The game runs without an OpenGL context, so the core draws in software.
  bool software_ = false;
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
  // The core's OpenGL context (RETRO_ENVIRONMENT_SET_HW_RENDER), used on the game's thread.
  std::unique_ptr<Gpu> gpu_;
  std::atomic<bool> hardware_{false};
  void (*hw_context_reset_)() = nullptr;
  void (*hw_context_destroy_)() = nullptr;
  bool hw_depth_ = false;
  bool hw_stencil_ = false;
  bool hw_bottom_left_ = false;
  bool published_ = false;
  bool card_kept_ = true;
  bool (*update_display_)() = nullptr;
  bool display_refresh_ = false;
  mutable std::mutex pause_mutex_;
  // SwanStation said which BIOS it found in the system folder; without that it boots its built-in
  // OpenBIOS, unless it loaded the BIOS file its options name for the console region.
  bool bios_found_ = false;
  std::string console_region_;
  // SwanStation could not open the disc and boots the BIOS with no disc instead.
  bool disc_missing_ = false;
  bool UsesBios() const;
  std::string card_;
  struct DiscControl;
  std::unique_ptr<DiscControl> disc_control_;

  // Lockstep (protocol.md "lockstep"). lockstep_, steps_ and next_frame_ are guarded by mutex_;
  // step_signal_ wakes the game's thread for a step or a state request.
  enum class StateWork { kSave, kLoad };
  struct StateRequest {
    StateWork work = StateWork::kSave;
    std::string path;
    bool done = false;
    std::optional<StateResult> result;
  };
  struct NextStep {
    bool lockstep = false;
    bool paced = true;
    uint64_t frame = 0;
    std::optional<StepInput> input;
  };
  // In lockstep, takes the next step when one is queued, waiting at most wait_ms for it.
  NextStep TakeStep(int64_t wait_ms);
  // Hashes what lockstep frame frame left behind and hands it to the host when a check is due.
  void FinishStep(uint64_t frame);
  // The core's serialized state, in a buffer of retro_serialize_size bytes that starts zeroed, or
  // nothing when the core cannot serialize now.
  const std::string* Serialize();
  std::string serialized_;
  std::optional<StateResult> RequestState(StateWork work, const std::string& path, int64_t timeout_ms);
  // Serves a save or load the launcher asked for; runs on the game's thread between frames.
  void ServeStateRequest();
  std::optional<StateResult> WriteState(const std::string& path);
  std::optional<StateResult> ReadState(const std::string& path);
  std::condition_variable step_signal_;
  std::condition_variable state_signal_;
  std::optional<LockstepConfig> lockstep_;
  std::deque<StepInput> steps_;
  uint64_t next_frame_ = 0;
  std::shared_ptr<StateRequest> state_request_;
  // The step being run, read by InputState on the game's thread.
  std::optional<StepInput> step_input_;
  // The savestate context the core is told about while the host serializes it.
  int savestate_context_ = 0;
  std::optional<uint64_t> video_hash_;
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_GAME_H_
