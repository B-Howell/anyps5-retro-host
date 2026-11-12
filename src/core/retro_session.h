#ifndef ANYPS5_RETRO_SESSION_H_
#define ANYPS5_RETRO_SESSION_H_

#include "libretro_abi.h"
#include "retro_frame.h"
#include "ps1_checkpoint.h"
#include "ps3_network_policy.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anyps5::retro {

enum class Console { kPs1, kPs2, kPs3, kUnknown };
Console ConsoleForCore(const std::string& path);

using Options = std::map<std::string, std::string>;

struct InitialDisc {
  unsigned index;
  std::string path;
};

struct Ps1MatchConfig {
  unsigned seat;
  std::string bios;
  bool dualshock = false;
  bool local_staging = false;
  bool rollback = false;
  std::vector<ps1_netplay::PadInput> input_trace{};
};

using Ps1Ports = std::array<ps1_netplay::PadInput, 2>;
constexpr size_t kPs1CatchupBatch = 120;
struct Ps1MatchHistory {
  uint32_t next_frame;
  std::vector<Ps1Ports> frames;
};

struct Ps1MatchTick {
  uint32_t frame;
  std::optional<uint32_t> confirmed, received;
  bool waiting;
  std::vector<ps1_netplay::FrameInput> local;
  std::vector<std::pair<uint32_t, std::string>> fingerprints;
};

struct SessionConfig {
  std::string core, disc, system, saves, initial_resolution;
  Options initial_graphics;
  std::optional<InitialDisc> initial_disc = std::nullopt;
  std::optional<std::array<uint8_t, 6>> lan_mac = std::nullopt;
  std::shared_ptr<const ps1_checkpoint::Recovery> recovery = nullptr;
  std::optional<Ps1MatchConfig> ps1_match = std::nullopt;
  std::optional<Ps3NetworkPolicy> ps3_online = std::nullopt;
};

using LanFrames = std::vector<std::vector<uint8_t>>;

struct OptionStatus {
  std::vector<std::string> values, labels;
  std::string title, active_value;
  bool restart;
  int32_t selected;
};

struct Telemetry {
  int64_t clock_ms, frames, steps, audio_frames, dropped_audio_frames;
  std::array<int64_t, 13> frame_intervals;
  double target_fps, audio_rate;
};

struct DiscStatus {
  std::vector<std::string> labels;
  std::optional<unsigned> inserted;
  std::string inserted_path;
  bool played = false;
  bool busy = false, failed = false;
};

struct SessionStatus {
  int64_t checkpoint_source = 0;
  int64_t recovered_source = 0;
  bool checkpoint_parked = false;
  bool netplay_ready = false;
  int64_t confirmed_audio_frame = -1;
  uint64_t rollback_corrections = 0;
  bool portable_checkpoint = false;
  bool ready = false, presented = false, ended = false, hardware = false, graphics_pending = false;
  int32_t frame_width = 0, frame_height = 0;
  double aspect = 4.0 / 3.0;
  std::string error, resolution_error;
  std::optional<std::string> progress_label;
  std::optional<int32_t> progress_percent;
  int32_t ps3_provider_status = -1;
  std::map<std::string, OptionStatus> options;
  Telemetry telemetry{};
  std::optional<DiscStatus> discs;
};

// The worker owns all video operations. The frontend may receive frame notifications
// on that worker, and may acquire immutable frames from a different raster thread.
class Video {
 public:
  virtual ~Video() = default;
  virtual bool Open() = 0;
  virtual int PreferredContext() const = 0;
  virtual std::string_view PreferredRenderer() const = 0;
  virtual bool Bind(HardwareCallbacks& callbacks) = 0;
  virtual bool Environment(unsigned command, void* data, int context) = 0;
  virtual void GeometryChanged(const Geometry&) {}
  virtual void Reset(const HardwareCallbacks& callbacks) = 0;
  virtual void Clear() = 0;
  virtual void BeforeCoreDeinit(const HardwareCallbacks&) {}
  virtual void Close() = 0;
  virtual std::shared_ptr<Frame> Read(unsigned width, unsigned height,
                                      const HardwareCallbacks& callbacks) = 0;
  virtual bool UnsupportedSync() const { return false; }
};

class Audio {
 public:
  virtual ~Audio() = default;
  virtual bool Open(double rate) = 0;
  virtual void Write(const int16_t* stereo, size_t frames) = 0;
  virtual void Flush() = 0;
  virtual int64_t dropped_frames() const = 0;
  virtual void Close() = 0;
};

class Session {
 public:
  Session(SessionConfig config, std::unique_ptr<Video> video, std::unique_ptr<Audio> audio,
          std::function<void()> frame_available);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void Start();
  bool ended() const;
  void Stop();
  void Join();
  void Pause(bool pause);
  bool ChangeDisc(unsigned index);
  bool ConnectPorts(unsigned count);
  void SetInput(uint16_t buttons, const std::array<int16_t, 4>& axes);
  void SetInput(unsigned port, uint16_t buttons, const std::array<int16_t, 4>& axes);
  std::optional<LanFrames> ExchangeLan(const LanFrames& input);
  bool ClearLanFrames();
  std::optional<int64_t> RequestOrdinaryCheckpoint(int64_t source, const ps1_checkpoint::Digest& digest, bool park);
  std::optional<int64_t> RequestOrdinaryResume(int64_t source);
  std::optional<SessionConfig> DetachOrdinaryRecovery(int64_t source);
  bool AcceptOrdinaryRecovery(int64_t source);
  bool StartNetplay();
  std::optional<int64_t> RequestMatchExport(const ps1_checkpoint::Digest& digest);
  std::optional<int64_t> RequestMatchImport(const ps1_checkpoint::Digest& digest, uint32_t frame,
                                            ps1_checkpoint::Bytes state);
  std::optional<uint32_t> HoldNetplay();
  bool ResumeLocalNetplay();
  std::optional<Ps1MatchHistory> MatchHistory(uint32_t first);
  std::optional<int64_t> RequestMatchReplay(uint32_t first, std::vector<Ps1Ports> frames);
  std::optional<Ps1MatchTick> SyncNetplay(const std::vector<ps1_netplay::FrameInput>& remote,
                                         std::optional<uint32_t> acknowledged);
  ps1_checkpoint::Mailbox::Reply TakeCheckpoint(int64_t ticket);
  void CancelCheckpoint(int64_t ticket);
  bool SetOption(const std::string& key, const std::string& value);
  bool SetTuningOptions(const Options& expected, const Options& values);
  SessionStatus Status();
  std::shared_ptr<const Frame> AcquireFrame();
  std::shared_ptr<const Frame> LatestFrame();
  void MarkPresented();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace anyps5::retro

#endif  // ANYPS5_RETRO_SESSION_H_
