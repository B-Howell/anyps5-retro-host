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
// sync message every check frames, and, when paced, no faster than the core's frame rate. With
// rollback (capability ps1Rollback) a step may leave ports unknown, and at most that many frames
// are stepped beyond the last confirmed one.
struct LockstepConfig {
  uint32_t check = 1;
  bool paced = true;
  std::optional<unsigned> rollback;
};

// The most frames lockstep.rollback.frames allows (protocol.md "lockstep").
constexpr unsigned kMaxRollbackFrames = 8;

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
// 1, and their state. A port without a controller reads as none. In rollback a connected port's
// input may not be known yet (protocol.md "step"); the game then runs the frame on a prediction.
struct StepInput {
  unsigned connected = 1;
  std::array<PortInput, 2> ports{};
  std::array<bool, 2> known{true, true};
  bool AllKnown() const { return known[0] && (connected < 2 || known[1]); }
};

// A re-simulation after a wrong prediction (protocol.md "rollback"): the first frame run again, how
// many were, and the time it took in all and to load the state.
struct RollbackReport {
  uint64_t frame = 0;
  uint64_t frames = 0;
  uint64_t micros = 0;
  uint64_t load_micros = 0;
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

// The lockstep frame a saved or loaded state belongs to, its size, and how long the host spent on
// it, or nothing when it could not be done.
struct StateResult {
  uint64_t frame = 0;
  uint64_t bytes = 0;
  uint64_t micros = 0;
};

// The in-memory states a game in lockstep keeps (protocol.md "saveState and loadState").
constexpr unsigned kStateSlots = 8;

// Where a state goes: a file the launcher names, or one of the game's in-memory slots.
struct StateTarget {
  std::string path;
  std::optional<unsigned> slot;
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
  virtual void OnRollback(const RollbackReport& report) = 0;
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
  // when it enters. Both this and LeaveLockstep wait for the frame being run to end, so no frame
  // straddles the change. Returns the next frame the game will run, or nothing when it has ended
  // or its frame did not end within 4 seconds.
  std::optional<uint64_t> Lockstep(const LockstepConfig& config);
  // Leaves lockstep and drops the steps not yet run; false when the game has ended or its frame
  // did not end within 4 seconds.
  bool LeaveLockstep();
  // Queues the input for one lockstep frame.
  StepResult Step(uint64_t frame, const StepInput& input);
  // Gives the input a step left unknown, for one port of one frame (protocol.md "correct").
  StepResult Correct(uint64_t frame, unsigned port, const PortInput& input);
  // Writes or reads the core's state, with its lockstep frame, between frames (protocol.md
  // "saveState" and "loadState"). Nothing when the game is not in lockstep, has ended, the slot
  // holds no state of this lockstep count, the core refused, or the game's thread did not get to
  // it within timeout_ms.
  std::optional<StateResult> SaveState(const StateTarget& target, int64_t timeout_ms);
  std::optional<StateResult> LoadState(const StateTarget& target, int64_t timeout_ms);

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
  // The port count the core was last given, which its controllers have from the next frame on; a
  // state saved between frames holds those controllers. Used only on the game's thread.
  unsigned core_ports_ = 1;
  std::optional<unsigned> disc_request_;
  std::array<PortInput, 2> input_{};
  // Each port's motors as the core last set them, and as the launcher was last told.
  std::array<std::array<uint16_t, 2>, 2> rumble_{};
  std::array<std::array<uint16_t, 2>, 2> rumble_sent_{};

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
    StateTarget target;
    bool done = false;
    std::optional<StateResult> result;
  };
  struct NextStep {
    bool lockstep = false;
    bool paced = true;
    uint64_t frame = 0;
    std::optional<StepInput> input;
    // In rollback: the frame is kept until it is confirmed, and its state is saved before it runs
    // when its input is partly predicted.
    bool kept = false;
    bool save = false;
    unsigned rollback = 0;
  };
  // In lockstep, takes the next step when one is queued, waiting at most wait_ms for it.
  NextStep TakeStep(int64_t wait_ms);
  // Hashes what lockstep frame frame left behind and hands it to the host when a check is due.
  void FinishStep(uint64_t frame);
  // What lockstep frame frame left behind, when a check is due after it.
  std::optional<SyncPoint> Measure(uint64_t frame);
  // The core's serialized state, in a buffer of retro_serialize_size bytes that starts zeroed, or
  // nothing when the core cannot serialize now.
  const std::string* Serialize();
  std::string serialized_;
  std::optional<StateResult> RequestState(StateWork work, const StateTarget& target, int64_t timeout_ms);
  // Serves a save or load the launcher asked for; runs on the game's thread between frames.
  void ServeStateRequest();
  std::optional<StateResult> WriteState(const std::string& path);
  std::optional<StateResult> ReadState(const std::string& path);
  // In-memory states, used only on the game's thread. A slot keeps the core's data in a buffer it
  // reuses, the lockstep frame and picture hash it continues with, the motors as the core had set
  // them, and the lockstep count it was saved in: entering lockstep anew starts frames at 0 again,
  // so older slots no longer load.
  // The core is told the state stays in this process (RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE).
  struct MemoryState {
    std::string data;
    uint64_t frame = 0;
    std::optional<uint64_t> video;
    std::array<std::array<uint16_t, 2>, 2> rumble{};
    unsigned ports = 1;
    uint64_t entry = 0;
    bool used = false;
  };
  std::array<MemoryState, kStateSlots> slots_;
  std::optional<StateResult> WriteSlot(unsigned slot);
  std::optional<StateResult> ReadSlot(unsigned slot);
  // Saves the core's state in memory, in the same-instance form, as the state lockstep frame frame
  // continues from, or loads it back.
  bool SaveMemory(MemoryState& memory, uint64_t frame);
  bool LoadMemory(const MemoryState& memory);
  // Forgets the rollback input, corrections and kept frames; with mutex_ held.
  void ClearRollback();
  // Connects the ports a state was saved with, at once, so the core loads each controller's
  // state into a controller of its own rather than into none, or into one made afresh at the
  // next frame. Runs on the game's thread, before the core reads the state.
  void UsePorts(unsigned count);
  // True while the game's thread runs a frame (see Lockstep and LeaveLockstep). Calls waiting for
  // the frame to end count themselves in fence_waiters_, and no frame starts while one waits.
  // Both are guarded by mutex_; fence_signal_ is notified when a frame ends.
  bool in_frame_ = false;
  int fence_waiters_ = 0;
  std::condition_variable fence_signal_;
  void EndFrame();
  // With lock holding mutex_: waits up to 4 seconds for the frame being run to end; true when it
  // has, and then no frame starts until lock is released.
  bool AwaitFrameEnd(std::unique_lock<std::mutex>& lock);
  std::condition_variable step_signal_;
  std::condition_variable state_signal_;
  std::optional<LockstepConfig> lockstep_;
  std::deque<StepInput> steps_;
  uint64_t next_frame_ = 0;
  // Counts the times the game entered lockstep; guarded by mutex_.
  uint64_t lockstep_entry_ = 0;
  std::shared_ptr<StateRequest> state_request_;
  // The step being run, read by InputState on the game's thread.
  std::optional<StepInput> step_input_;
  // The savestate context the core is told about while the host serializes it.
  int savestate_context_ = 0;
  std::optional<uint64_t> video_hash_;

  // Rollback (protocol.md "Rollback"). A frame is confirmed once it and every frame before it have
  // every port's input known. Guarded by mutex_:
  // - known_ holds, from the first frame not confirmed to the last frame stepped, which ports of each
  //   frame have known input (a port a step does not connect is known), as steps and corrections
  //   came; the launcher's messages are checked against it.
  // - corrections_ holds the corrections the game's thread has not taken yet.
  // - kept_ holds the frames the game has run that it has not confirmed yet, from kept_from_: the
  //   input known for each, the input it last ran with, whether its state was saved before it, and
  //   what it left behind when a check is due after it.
  // - predicted_ is each port's latest known input, which a frame runs with while that port's input
  //   for it is not known.
  struct Correction {
    uint64_t frame = 0;
    unsigned port = 0;
    PortInput input;
  };
  struct KeptFrame {
    StepInput known;
    StepInput ran;
    bool saved = false;
    std::optional<SyncPoint> sync;
  };
  std::deque<std::array<bool, 2>> known_;
  std::deque<Correction> corrections_;
  std::deque<KeptFrame> kept_;
  uint64_t kept_from_ = 0;
  std::array<PortInput, 2> predicted_{};
  // The input a frame runs with: the known input, and the prediction for a port not known; with
  // mutex_ held.
  StepInput Predict(const StepInput& known) const;
  // Takes the corrections the launcher sent, runs the game again from the first frame that ran
  // with a wrong prediction, and sends the sync messages of the frames now confirmed. Runs on the
  // game's thread between frames; false when the game must end, with the reason in end_error and
  // end_code.
  bool Settle(std::string& end_error, std::string& end_code);
  bool Resimulate(uint64_t first, std::string& end_error, std::string& end_code);
  // The states before frames whose input was partly predicted, at frame modulo rollback frames;
  // used only on the game's thread.
  std::vector<MemoryState> rollback_states_;
  // While the game's thread runs frames again, it passes on only the newest picture, once it is
  // done (held_picture_ is its size while it waits), no sound, and the rumble once it is done.
  bool resimulating_ = false;
  std::optional<std::pair<uint32_t, uint32_t>> held_picture_;
  // Runs the core for one frame: with a lockstep frame's input and port count, or with the input
  // of the last input messages.
  void RunFrame(const std::optional<StepInput>& input);
  void (*run_)() = nullptr;
  // Tells the launcher each port's motors that differ from what it was last told.
  void SendRumble();
  // Whether the last frame could not be shown, which ends the game; the reason goes into end.
  bool FrameFailed(std::string& end_error, std::string& end_code);
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_GAME_H_
