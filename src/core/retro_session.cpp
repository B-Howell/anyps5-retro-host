#include "retro_session.h"
#include "ps1_match.h"

#include "retro_library.h"
#include "ps2_virtual_lan.h"
#include "ps3_network_policy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace anyps5::retro {
namespace {
constexpr char kPs3Resolution[] = "rpcs3_resolution_scale";

bool ResolutionOption(const std::string& key) {
  return key == "swanstation_GPU_ResolutionScale" || key == "pcsx2_upscale_multiplier";
}

constexpr auto kPs1Resolution = "swanstation_GPU_ResolutionScale";
constexpr auto kPs1Filter = "swanstation_GPU_TextureFilter";
constexpr auto kPs2Resolution = "pcsx2_upscale_multiplier";
constexpr auto kPs2Anisotropy = "pcsx2_anisotropic_filtering";
constexpr auto kPs2Trilinear = "pcsx2_trilinear_filtering";

bool Ps1GraphicsOption(const std::string& key) {
  return key == kPs1Resolution || key == kPs1Filter;
}

bool TrialOption(const std::string& key) {
  return Ps1GraphicsOption(key) || key == kPs2Resolution || key == kPs2Anisotropy || key == kPs2Trilinear ||
         key == kPs3Resolution || key == "rpcs3_anisotropic_filter";
}

bool LiveCoreOption(const std::string& key, bool hardware) {
  return key == "swanstation_Display_CropMode" || key == "pcsx2_deinterlace_mode" ||
         (hardware && (ResolutionOption(key) || TrialOption(key)));
}

bool BootCoreOption(const std::string& key) {
  static constexpr std::string_view keys[] = {
      "swanstation_GPU_Renderer", "swanstation_CPU_ExecutionMode",
      "swanstation_CDROM_PreCacheCHD", "swanstation_CDROM_ReadThread",
      "swanstation_CDROM_ReadaheadSectors", "pcsx2_renderer", "pcsx2_mtvu",
      "pcsx2_fastcdvd", "pcsx2_sw_renderer_threads", "pcsx2_disable_shader_cache",
      "rpcs3_ppu_decoder", "rpcs3_spu_decoder", "rpcs3_preferred_spu_threads",
      "rpcs3_llvm_threads", "rpcs3_shader_compiler_threads", "rpcs3_llvm_precompilation",
      "rpcs3_emulate_hdd_speed", "rpcs3_emulate_bdvd_speed", "rpcs3_disk_cache_size",
      "rpcs3_spu_cache", "rpcs3_master_volume", "rpcs3_language", "rpcs3_license_area",
      "rpcs3_enter_button",
  };
  return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

bool SupportedBootValue(const std::string& key, std::string_view value, std::string_view renderer) {
  if (key == "swanstation_GPU_Renderer") {
    return value == "Software" || (!renderer.empty() && value == renderer);
  }
  if (key == "pcsx2_renderer") {
    return value == "Software (SW)" || value == "Software (HW)" ||
           (!renderer.empty() && value == renderer);
  }
  return key != "rpcs3_disk_cache_size" || value != "wipe";
}

void RestoreCard(const std::filesystem::path& path, void* data, size_t size) {
  if (!data || !size || !std::filesystem::exists(path)) return;
  if (std::filesystem::file_size(path) != size) {
    throw std::runtime_error("The PlayStation memory card has an unexpected size");
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.read(static_cast<char*>(data), static_cast<std::streamsize>(size))) {
    throw std::runtime_error("Could not read the PlayStation memory card");
  }
}

void StoreCard(const std::filesystem::path& path, const void* data, size_t size) {
  if (!data || !size) return;
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  file.close();
  if (!file) throw std::runtime_error("Could not write the PlayStation memory card");
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
#endif
    throw std::runtime_error("Could not write the PlayStation memory card");
  }
}
}  // namespace

Console ConsoleForCore(const std::string& path) {
  const auto name = std::filesystem::u8path(path).stem().u8string();
  if (name == "swanstation_libretro") return Console::kPs1;
  if (name == "pcsx2_libretro") return Console::kPs2;
  if (name == "rpcs3_libretro") return Console::kPs3;
  return Console::kUnknown;
}

class Session::Impl {
 public:
  Impl(SessionConfig config, std::unique_ptr<Video> video, std::unique_ptr<Audio> audio,
       std::function<void()> frame_available)
      : config_(std::move(config)), content_(std::filesystem::u8path(config_.disc).parent_path().u8string()),
        console_(ConsoleForCore(config_.core)), ps3_(console_ == Console::kPs3),
        frame_available_(std::move(frame_available)), video_(std::move(video)), audio_(std::move(audio)) {
    if (config_.ps1_match) {
      if (console_ != Console::kPs1 || config_.recovery || config_.initial_disc || config_.lan_mac)
        throw std::invalid_argument("Invalid synchronized PS1 launch configuration");
      if (config_.ps1_match->local_staging && config_.ps1_match->seat != 0)
        throw std::invalid_argument("Only the PS1 host can stage a local match.");
      match_timeline_.emplace(config_.ps1_match->seat, config_.ps1_match->rollback
          ? ps1_netplay::Timeline::kStandardPrediction : 0);
      match_storage_ = std::make_unique<Ps1MatchStorage>(config_.ps1_match->bios);
      config_.system = match_storage_->system();
      config_.saves = match_storage_->saves();
      content_ = match_storage_->content();
      suppress_frame_ = true;
    }
    if (config_.recovery) {
      const auto& recovery = *config_.recovery;
      if (console_ != Console::kPs1 || recovery.source <= 0 || recovery.state.empty() ||
          recovery.state.size() > ps1_checkpoint::kMaximumBytes)
        throw std::invalid_argument("Invalid private PS1 recovery checkpoint");
      recovery_accepted_ = false;
      paused_ = true;
      suppress_frame_ = true;
    }
    if (config_.lan_mac) {
      if (console_ != Console::kPs2 || !Ps2VirtualLan::ValidMac(*config_.lan_mac))
        throw std::invalid_argument("Expected a private PS2 LAN address");
      lan_ = std::make_unique<Ps2VirtualLan>(*config_.lan_mac);
    }
  }

  ~Impl() { Stop(); Join(); }

  void Start() { worker_ = std::thread([this] { Run(); }); }

  bool ended() const { return ended_; }
  void Stop() { stop_ = true; checkpoint_.Stop(); if (lan_) lan_->Close(); }
  void Join() { if (worker_.joinable()) worker_.join(); }

  void Pause(bool pause) {
    if (lan_ || match_timeline_) {
      if (pause) ClearInput();
      return;
    }
    if (paused_.exchange(pause) == pause) return;
    if (pause) {
      ClearInput();
      audio_->Flush();
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      last_frame_at_ = {};
    }
  }

  bool ChangeDisc(unsigned index) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (config_.ps3_online || match_timeline_ || !recovery_accepted_ || checkpoint_.busy()) return false;
    if (stop_ || ended_ || !discs_ || discs_->busy ||
        index >= discs_->labels.size() || discs_->inserted == index)
      return false;
    discs_->busy = true;
    discs_->failed = false;
    pending_disc_ = index;
    return true;
  }

  void ClearInput() {
    std::lock_guard<std::mutex> lock(input_mutex_);
    buttons_.fill(0);
    for (auto& axes : axes_) axes.fill(0);
  }

  void SetInput(unsigned port, uint16_t buttons, const std::array<int16_t, 4>& axes) {
    if (port >= kLocalPorts) return;
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!recovery_accepted_) return;
    buttons_[port] = buttons;
    axes_[port] = axes;
  }

  std::optional<LanFrames> ExchangeLan(const LanFrames& input) {
    if (!lan_ || !lan_ready_ || stop_ || ended_ || input.size() > Ps2VirtualLan::kBatchSize)
      return std::nullopt;
    for (const auto& frame : input)
      if (!lan_->Accepts(frame.data(), frame.size())) return std::nullopt;
    for (const auto& frame : input) lan_->Inject(frame.data(), frame.size());
    LanFrames output;
    std::array<uint8_t, Ps2VirtualLan::kMaxFrame> frame{};
    while (output.size() < Ps2VirtualLan::kBatchSize) {
      const auto size = lan_->Drain(frame.data(), frame.size());
      if (!size) break;
      output.emplace_back(frame.begin(), frame.begin() + size);
    }
    return output;
  }

  bool ClearLanFrames() {
    if (!lan_ || !lan_ready_ || stop_ || ended_) return false;
    lan_->ClearFrames();
    return !stop_ && !ended_;
  }

  std::optional<int64_t> RequestOrdinaryCheckpoint(int64_t source, const ps1_checkpoint::Digest& digest, bool park) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (match_timeline_ || !recovery_accepted_ || !presented_ || stop_ || ended_ || source <= 0 ||
        source != checkpoint_.source() ||
        options_changed_ || graphics_queued_ || graphics_applying_ || pending_disc_ ||
        (discs_ && discs_->busy)) return std::nullopt;
    return checkpoint_.RequestCapture(digest, park);
  }
  std::optional<int64_t> RequestOrdinaryResume(int64_t source) {
    if (stop_ || ended_ || source <= 0 || source != checkpoint_.source()) return std::nullopt;
    return checkpoint_.RequestResume();
  }
  std::optional<SessionConfig> DetachOrdinaryRecovery(int64_t source) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (!recovery_accepted_ || stop_ || ended_ || !checkpoint_.parked()) return std::nullopt;
    SessionConfig reopen = config_;
    if (discs_) {
      if (!discs_->inserted || discs_->inserted_path.empty()) return std::nullopt;
      reopen.initial_disc = InitialDisc{*discs_->inserted, discs_->inserted_path};
    }
    reopen.initial_graphics.clear();
    for (const auto& [key, option] : options_)
      reopen.initial_graphics.emplace(key, option.values[option.selected]);
    auto recovery = std::make_shared<ps1_checkpoint::Recovery>();
    if (!checkpoint_.Detach(source, *recovery)) return std::nullopt;
    stop_ = true;
    reopen.recovery = std::move(recovery);
    return reopen;
  }
  bool AcceptOrdinaryRecovery(int64_t source) {
    if (source <= 0 || recovered_source_ != source || !presented_ || stop_ || ended_) return false;
    bool pending = false;
    return recovery_accepted_.compare_exchange_strong(pending, true);
  }
  bool StartNetplay() {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || match_import_ || (StagingLocal() && !match_held_)) return false;
    if (match_held_) {
      match_timeline_->Reset();
      match_history_.clear();
      match_fingerprints_.clear();
      match_held_ = false;
    }
    match_timeline_->Start();
    match_started_ = true;
    match_replay_cursor_.reset();
    return true;
  }
  std::optional<int64_t> RequestMatchExport(const ps1_checkpoint::Digest& digest) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || match_import_ || match_replay_cursor_ ||
        (config_.ps1_match->rollback && match_started_)) return std::nullopt;
    return checkpoint_.RequestCapture(digest, false);
  }
  std::optional<int64_t> RequestMatchImport(const ps1_checkpoint::Digest& digest, uint32_t frame,
                                            ps1_checkpoint::Bytes state) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || match_started_ || StagingLocal() || frame > ps1_netplay::Timeline::kMaxFrame)
      return std::nullopt;
    const auto ticket = checkpoint_.RequestImport(digest, std::move(state));
    if (ticket) {
      match_import_ = std::make_pair(*ticket, frame);
      match_replay_cursor_.reset();
    }
    return ticket;
  }
  std::optional<uint32_t> HoldNetplay() {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || !checkpoint_.ready() || match_frame_in_flight_) return std::nullopt;
    if (config_.ps1_match->rollback && match_started_) return std::nullopt;
    if (!StagingLocal() && (!match_started_ || !match_timeline_->Hold())) return std::nullopt;
    match_held_ = true;
    return MatchFrame();
  }
  bool ResumeLocalNetplay() {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || !config_.ps1_match->local_staging || !match_held_) return false;
    if (match_started_) {
      match_started_ = false;
      match_local_frame_ = 0;
      match_history_.clear();
    }
    match_held_ = false;
    return true;
  }
  std::optional<Ps1MatchHistory> MatchHistory(uint32_t first) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!match_ready_ || stop_ || ended_ || !checkpoint_.ready() ||
        (!StagingLocal() && (!match_started_ || config_.ps1_match->rollback)))
      return std::nullopt;
    const auto next = MatchFrame();
    const auto oldest = next - static_cast<uint32_t>(match_history_.size());
    if (first < oldest || first > next) return std::nullopt;
    const auto count = std::min<size_t>(next - first, kPs1CatchupBatch);
    const auto begin = match_history_.begin() + (first - oldest);
    return Ps1MatchHistory{next, {begin, begin + count}};
  }
  std::optional<int64_t> RequestMatchReplay(uint32_t first, std::vector<Ps1Ports> frames) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!MatchCheckpointIdle() || match_import_ || match_started_ || match_replay_cursor_ != first ||
        frames.empty() || frames.size() > kPs1CatchupBatch ||
        first > ps1_netplay::Timeline::kMaxFrame - frames.size()) return std::nullopt;
    const auto ticket = ps1_checkpoint::NextIdentity();
    if (!ticket) return std::nullopt;
    match_replay_ = Replay{*ticket, first, std::move(frames), false, std::nullopt};
    return ticket;
  }
  std::optional<Ps1MatchTick> SyncNetplay(const std::vector<ps1_netplay::FrameInput>& remote,
                                         std::optional<uint32_t> acknowledged) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!match_ready_ || stop_ || ended_ || StagingLocal() || remote.size() > 32 ||
        !match_timeline_->Receive(remote, acknowledged)) return std::nullopt;
    ConfirmFingerprint();
    return Ps1MatchTick{match_timeline_->frame(), match_timeline_->confirmed(), match_timeline_->received(),
                        match_timeline_->waiting(), match_timeline_->LocalPacket(), match_fingerprints_};
  }
  ps1_checkpoint::Mailbox::Reply TakeCheckpoint(int64_t ticket) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (match_replay_ && match_replay_->ticket == ticket) {
      if (stop_ || ended_) return false;
      if (!match_replay_->result) return std::monostate{};
      const auto success = *match_replay_->result;
      match_replay_.reset();
      return success;
    }
    auto reply = checkpoint_.Take(ticket);
    if (match_import_ && match_import_->first == ticket) {
      if (const auto* imported = std::get_if<bool>(&reply); imported && *imported) {
        match_replay_cursor_ = match_import_->second;
        match_import_.reset();
      }
    }
    return reply;
  }
  void CancelCheckpoint(int64_t ticket) {
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (match_replay_ && match_replay_->ticket == ticket) {
      if (match_replay_->running || match_replay_->result == true) Stop();
      match_replay_.reset();
      return;
    }
    checkpoint_.Cancel(ticket);
  }

  SessionStatus Status() {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    SessionStatus status;
    status.ps3_provider_status = ps3_provider_status_;
    status.ready = frame_ready_;
    status.confirmed_audio_frame = confirmed_audio_frame_;
    status.rollback_corrections = rollback_corrections_;
    status.presented = presented_;
    status.ended = ended_;
    status.netplay_ready = match_ready_ && !stop_ && !ended_;
    status.portable_checkpoint = status.netplay_ready && checkpoint_.ready();
    status.recovered_source = recovered_source_;
    if (!match_timeline_ && recovery_accepted_ && presented_ && !stop_ && !ended_)
      status.checkpoint_source = checkpoint_.source();
    status.checkpoint_parked = checkpoint_.parked();
    status.error = error_;
    status.aspect = aspect_;
    status.hardware = hardware_frame_;
    status.frame_width = static_cast<int32_t>(frame_width_.load());
    status.frame_height = static_cast<int32_t>(frame_height_.load());
    status.resolution_error = resolution_error_;
    status.graphics_pending = graphics_queued_ || graphics_applying_;
    if (std::chrono::steady_clock::now() < progress_until_) {
      status.progress_label = progress_label_;
      if (progress_percent_ >= 0) status.progress_percent = progress_percent_;
    }
    for (const auto& [key, option] : options_) {
      if (!option.graphics && !option.boot && !LiveCoreOption(key, hardware_frame_)) continue;
      status.options.emplace(key, OptionStatus{option.values, option.labels, option.title,
          option.values[option.selected], option.boot || (option.graphics && !TrialOption(key)),
          static_cast<int32_t>(option.pending.value_or(option.selected))});
    }
    status.telemetry = TuningTelemetry();
    status.discs = discs_;
    if (status.discs)
      status.discs->played = status.discs->inserted && status.telemetry.steps > disc_inserted_at_step_;
    return status;
  }

  std::shared_ptr<const Frame> AcquireFrame() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_) presented_ = true;
    return frame_;
  }

  std::shared_ptr<const Frame> LatestFrame() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return frame_;
  }

  void MarkPresented() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_) presented_ = true;
  }

  bool SetOption(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (config_.ps3_online || match_timeline_ || !recovery_accepted_ || checkpoint_.busy()) return false;
    const auto option = options_.find(key);
    if (option == options_.end() ||
        (!option->second.graphics && !option->second.boot && !LiveCoreOption(key, hardware_frame_))) return false;
    auto& data = option->second;
    const auto choice = std::find(data.values.begin(), data.values.end(), value);
    if (choice == data.values.end()) return false;
    const auto selected = static_cast<size_t>(choice - data.values.begin());
    if (graphics_applying_ || (graphics_queued_ && (key == kPs2Anisotropy || key == kPs2Trilinear))) return false;
    if (Ps1GraphicsOption(key) || key == kPs2Resolution) {
      data.pending = selected;
      graphics_queued_ = true;
      resolution_error_.clear();
    } else if (data.boot || (data.graphics && !TrialOption(key))) {
      data.pending = selected;
    } else if (selected != data.selected) {
      data.selected = selected;
      options_changed_ = true;
    }
    return true;
  }

  bool SetTuningOptions(const Options& expected, const Options& values) {
    if (!presented_ || !hardware_frame_ || values.empty() || expected.size() != values.size())
      return false;
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (config_.ps3_online || match_timeline_ || !recovery_accepted_ || checkpoint_.busy() || graphics_applying_ || graphics_queued_) return false;
    std::vector<std::pair<Option*, size_t>> changes;
    bool checkpoint = false;
    for (const auto& [key, value] : values) {
      if (!TrialOption(key)) return false;
      checkpoint |= Ps1GraphicsOption(key) || key == kPs2Resolution;
      const auto current = expected.find(key);
      const auto option = options_.find(key);
      if (current == expected.end() || option == options_.end()) return false;
      auto& data = option->second;
      if (data.values[data.selected] != current->second) return false;
      const auto choice = std::find(data.values.begin(), data.values.end(), value);
      if (choice == data.values.end()) return false;
      changes.emplace_back(&data, static_cast<size_t>(choice - data.values.begin()));
    }
    for (const auto& change : changes) {
      if (checkpoint) change.first->pending = change.second;
      else change.first->selected = change.second;
    }
    if (checkpoint) {
      graphics_queued_ = true;
      resolution_error_.clear();
    } else {
      options_changed_ = true;
    }
    return true;
  }

 private:
  struct Replay {
    int64_t ticket;
    uint32_t first;
    std::vector<Ps1Ports> frames;
    bool running;
    std::optional<bool> result;
  };

  // Match helpers below are called with match_mutex_ held unless they acquire it.
  bool MatchCheckpointIdle() const {
    return match_ready_ && !stop_ && !ended_ && !checkpoint_.busy() && !match_replay_;
  }
  bool StagingLocal() const { return config_.ps1_match->local_staging && !match_started_; }
  uint32_t MatchFrame() const { return StagingLocal() ? match_local_frame_ : match_timeline_->frame(); }

  void ServiceMatchReplay(void (*step)()) {
    std::vector<Ps1Ports> frames;
    int64_t ticket;
    uint32_t first;
    {
      std::lock_guard<std::mutex> lock(match_mutex_);
      if (!match_replay_ || match_replay_->running || match_replay_->result) return;
      match_replay_->running = true;
      ticket = match_replay_->ticket;
      first = match_replay_->first;
      frames = std::move(match_replay_->frames);
    }
    suppress_frame_ = true;
    for (const auto& ports : frames) {
      if (stop_) break;
      match_ports_ = ports;
      step();
    }
    std::lock_guard<std::mutex> lock(match_mutex_);
    if (!match_replay_ || match_replay_->ticket != ticket) return;
    match_replay_->running = false;
    match_replay_->result = !stop_;
    if (!stop_) match_replay_cursor_ = first + static_cast<uint32_t>(frames.size());
  }

  struct Option {
    std::vector<std::string> values;
    std::vector<std::string> labels;
    std::string title;
    bool graphics = false;
    bool boot = false;
    size_t selected = 0;
    std::optional<size_t> pending;
  };

  void SetCoreOptions(const CoreOptionsV2* options) {
    if (!ps3_ || !options || !options->definitions) return;
    std::lock_guard<std::mutex> lock(settings_mutex_);
    for (auto* row = options->definitions; row->key; ++row) {
      const std::string key = row->key;
      if (options_.count(key)) continue;
      Option option;
      if (row->title) option.title = row->title;
      option.graphics = row->category && std::strcmp(row->category, "gpu") == 0 &&
                        key != "rpcs3_renderer";
      option.boot = BootCoreOption(key);
      std::string preferred = row->default_value ? row->default_value : "";
      if (key == "rpcs3_renderer") preferred = "vulkan";
      if (!match_timeline_ && (option.graphics || option.boot)) {
        const auto saved = config_.initial_graphics.find(key);
        if (saved != config_.initial_graphics.end()) preferred = saved->second;
      }
      for (const auto& value : row->values) {
        if (!value.value) break;
        if (!SupportedBootValue(key, value.value, hardware_available_ ? video_->PreferredRenderer() : std::string_view{})) continue;
        if (key == kPs3Resolution && std::atoi(value.value) > 200) continue;
        option.values.emplace_back(value.value);
        option.labels.emplace_back(value.label ? value.label : value.value);
      }
      auto choice = std::find(option.values.begin(), option.values.end(), preferred);
      if (choice == option.values.end() && row->default_value) {
        choice = std::find(option.values.begin(), option.values.end(), row->default_value);
      }
      if (choice != option.values.end()) option.selected = static_cast<size_t>(choice - option.values.begin());
      ApplyRecoveryOption(key, option);
      ApplyMatchOption(key, option);
      ApplyPrivateLanOption(key, option);
      if (!option.values.empty()) options_.emplace(key, std::move(option));
    }
  }

  template <typename Reopen>
  void ApplyCheckpointGraphics(const DynamicLibrary& core, const Reopen& reopen, void (*step)(), bool ps1,
                               void*& card, size_t& card_size, const std::filesystem::path& card_path) {
    using Clock = std::chrono::steady_clock;
    struct Change { Option* option; size_t before; size_t after; };
    std::vector<Change> changes;
    {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      if (!graphics_queued_) return;
      graphics_queued_ = false;
      constexpr std::array<const char*, 2> ps1_keys{kPs1Resolution, kPs1Filter};
      constexpr std::array<const char*, 3> ps2_keys{kPs2Resolution, kPs2Anisotropy, kPs2Trilinear};
      const auto* keys = ps1 ? ps1_keys.data() : ps2_keys.data();
      const auto count = ps1 ? ps1_keys.size() : ps2_keys.size();
      for (size_t index = 0; index < count; ++index) {
        const auto* key = keys[index];
        const auto found = options_.find(key);
        if (found == options_.end()) continue;
        auto& option = found->second;
        const auto pending = std::exchange(option.pending, std::nullopt);
        if (pending && *pending != option.selected)
          changes.push_back({&option, option.selected, *pending});
      }
      if (changes.empty()) return;
      graphics_applying_ = true;
    }
    const auto serialize_size = core.Get<size_t (*)()>("retro_serialize_size");
    const auto serialize = core.Get<bool (*)(void*, size_t)>("retro_serialize");
    const auto unserialize = core.Get<bool (*)(const void*, size_t)>("retro_unserialize");
    const auto size = serialize_size();
    std::vector<uint8_t> state(size);
    bool checkpoint_ready = size != 0 && serialize(state.data(), size);
    std::string failure = "The emulator could not apply those graphics settings.";
    if (checkpoint_ready && card) {
      try {
        StoreCard(card_path, card, card_size);
      } catch (const std::exception& error) {
        checkpoint_ready = false;
        failure = error.what();
      }
    }
    bool applied = false;
    if (checkpoint_ready) {
      const auto restore = [&](bool use_requested) {
        {
          std::lock_guard<std::mutex> lock(settings_mutex_);
          for (const auto& change : changes)
            change.option->selected = use_requested ? change.after : change.before;
          if (ps1) options_changed_ = true;
        }
        suppress_frame_ = true;
        if (ps1) step();
        const bool restored = (ps1 || reopen()) && unserialize(state.data(), size);
        suppress_frame_ = false;
        if (!restored) return false;
        const auto before = hardware_frames_;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!stop_ && hardware_frames_ == before && Clock::now() < deadline) {
          step();
          if (hardware_frames_ == before)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return stop_ || hardware_frames_ != before;
      };
      audio_->Flush();
      applied = restore(true);
      if (!applied && !restore(false))
        throw std::runtime_error("The emulator could not restore the game after changing graphics.");
      if (ps1) {
        card = core.Get<void* (*)(unsigned)>("retro_get_memory_data")(0);
        card_size = core.Get<size_t (*)(unsigned)>("retro_get_memory_size")(0);
      }
    }
    std::lock_guard<std::mutex> lock(settings_mutex_);
    graphics_applying_ = false;
    if (!applied) resolution_error_ = std::move(failure);
  }

  void Publish(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (suppress_frame_ || !data || width == 0 || height == 0 || width > kMaxFrameSize || height > kMaxFrameSize)
      return;
    if (data == reinterpret_cast<const void*>(UINTPTR_MAX)) {
      try {
        if (auto frame = video_->Read(width, height, hardware_callbacks_)) SubmitFrame(std::move(frame), true);
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        error_ = error.what();
        stop_ = true;
      }
      return;
    }
    if (auto frame = DecodeSoftwareFrame(data, width, height, pitch, pixel_format_)) {
      SubmitFrame(std::move(frame), false);
    }
  }

  Telemetry TuningTelemetry() {
    using Clock = std::chrono::steady_clock;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    return {now, video_frames_, steps_, audio_frames_, audio_->dropped_frames(),
            frame_intervals_, target_fps_, audio_rate_};
  }

  void RecordFrame() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    ++video_frames_;
    if (last_frame_at_ != Clock::time_point{}) {
      const auto millis = std::chrono::duration<double, std::milli>(now - last_frame_at_).count();
      static constexpr std::array<int, 13> limits{8, 12, 16, 20, 25, 35, 50, 75, 100, 150, 250, 500, 1000};
      const auto index = std::min(static_cast<size_t>(std::lower_bound(limits.begin(), limits.end(), millis) -
                                                     limits.begin()), limits.size() - 1);
      ++frame_intervals_[index];
    }
    last_frame_at_ = now;
  }

  void SubmitFrame(std::shared_ptr<Frame> frame, bool hardware) {
    if (!paused_) RecordFrame();
    if (hardware) ++hardware_frames_;
    hardware_frame_ = hardware;
    frame_width_ = static_cast<unsigned>(frame->width);
    frame_height_ = static_cast<unsigned>(frame->height);
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      frame_ = std::move(frame);
    }
    frame_ready_ = true;
    frame_available_();
  }

  void SetAspect(float aspect) {
    if (std::isfinite(aspect) && aspect > 0.5f && aspect <= 4.f) aspect_ = aspect;
  }

  void ApplyPrivateLanOption(const std::string& key, Option& option) const {
    if (!lan_ || key != "pcsx2_dev9_eth") return;
    const auto enabled = std::find(option.values.begin(), option.values.end(), "enabled");
    if (enabled != option.values.end()) option.selected = static_cast<size_t>(enabled - option.values.begin());
  }

  void ApplyRecoveryOption(const std::string& key, Option& option) const {
    if (!config_.recovery) return;
    const auto saved = config_.initial_graphics.find(key);
    if (saved == config_.initial_graphics.end()) return;
    const auto value = std::find(option.values.begin(), option.values.end(), saved->second);
    if (value != option.values.end()) option.selected = static_cast<size_t>(value - option.values.begin());
  }

  void ApplyMatchOption(const std::string& key, Option& option) const {
    if (!match_timeline_) return;
    for (const auto& [name, value] : kPs1NetplayOptions) {
      if (name != key) continue;
      const auto choice = std::find(option.values.begin(), option.values.end(), value);
      if (choice != option.values.end()) option.selected = static_cast<size_t>(choice - option.values.begin());
      return;
    }
  }

  void SetVariables(const Variable* variables) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    for (; variables->key && variables->value; ++variables) {
      const std::string key = variables->key;
      const std::string declaration = variables->value;
      const auto delimiter = declaration.find("; ");
      if (delimiter == std::string::npos || options_.count(key)) continue;
      Option option;
      option.title = declaration.substr(0, delimiter);
      option.boot = BootCoreOption(key);
      std::istringstream list(declaration.substr(delimiter + 2));
      for (std::string value; std::getline(list, value, '|');) {
        if (!SupportedBootValue(key, value, hardware_available_ ? video_->PreferredRenderer() : std::string_view{})) continue;
        if (ResolutionOption(key) && !value.empty() && value[0] >= '0' && value[0] <= '9' &&
            std::stoi(value) > 4) continue;
        if (!match_timeline_) {
          if (key.find("renderer") != std::string::npos || key.find("Renderer") != std::string::npos) {
            if (value == "Software" || value == "Software (HW)") option.selected = option.values.size();
          } else if (key.find("FastBoot") != std::string::npos || key == "pcsx2_fastboot") {
            if (value == "enabled" || value == "true") option.selected = option.values.size();
          }
        }
        option.values.push_back(std::move(value));
      }
      if (hardware_available_ &&
          (key == "swanstation_GPU_Renderer" || key == "pcsx2_renderer")) {
        const auto hardware = std::find(option.values.begin(), option.values.end(), video_->PreferredRenderer());
        if (hardware != option.values.end())
          option.selected = static_cast<size_t>(hardware - option.values.begin());
      }
      if (hardware_available_ && ResolutionOption(key)) {
        const auto saved = std::find(option.values.begin(), option.values.end(), config_.initial_resolution);
        if (saved != option.values.end())
          option.selected = static_cast<size_t>(saved - option.values.begin());
      }
      if (!match_timeline_ && !ResolutionOption(key) &&
          (option.boot || (hardware_available_ && TrialOption(key)))) {
        const auto saved = config_.initial_graphics.find(key);
        if (saved != config_.initial_graphics.end()) {
          const auto choice = std::find(option.values.begin(), option.values.end(), saved->second);
          if (choice != option.values.end())
            option.selected = static_cast<size_t>(choice - option.values.begin());
        }
      }
      ApplyRecoveryOption(key, option);
      ApplyMatchOption(key, option);
      ApplyPrivateLanOption(key, option);
      if (!option.values.empty()) options_.emplace(key, std::move(option));
    }
  }

  std::string DiscPath(unsigned index) const {
    std::array<char, 16384> path{};
    if (!disk_callbacks_.get_image_path ||
        !disk_callbacks_.get_image_path(index, path.data(), path.size())) return {};
    return {path.begin(), std::find(path.begin(), path.end(), '\0')};
  }

  void RefreshDiscs(bool failed = false) {
    const auto& disk = disk_callbacks_.basic;
    std::optional<DiscStatus> updated;
    if (disk.set_eject_state && disk.get_eject_state && disk.get_image_index &&
        disk.set_image_index && disk.get_num_images) {
      const auto count = disk.get_num_images();
      if (count >= 2 && count <= 128) {
        updated.emplace();
        updated->labels.resize(count);
        if (disk_callbacks_.get_image_label) {
          for (unsigned index = 0; index < count; ++index) {
            std::array<char, 1024> label{};
            if (disk_callbacks_.get_image_label(index, label.data(), label.size()))
              updated->labels[index].assign(label.begin(), std::find(label.begin(), label.end(), '\0'));
          }
        }
        const auto index = disk.get_image_index();
        if (!disk.get_eject_state() && index < count) {
          updated->inserted = index;
          updated->inserted_path = DiscPath(index);
        }
        updated->failed = failed;
      }
    }
    std::lock_guard<std::mutex> lock(settings_mutex_);
    // Preserve a request queued while the core was refreshing its state.
    if (updated) updated->busy = pending_disc_.has_value();
    if (updated && (!discs_ || updated->inserted != discs_->inserted ||
                    updated->inserted_path != discs_->inserted_path)) {
      disc_inserted_at_step_ = steps_;
    }
    discs_ = std::move(updated);
  }

  void ApplyDiscChange() {
    std::optional<unsigned> requested;
    {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      requested = std::exchange(pending_disc_, std::nullopt);
    }
    if (!requested) return;
    const auto& disk = disk_callbacks_.basic;
    if (!disk.get_eject_state || !disk.get_image_index || !disk.set_eject_state ||
        !disk.set_image_index || !disk.get_num_images || *requested >= disk.get_num_images()) {
      RefreshDiscs(true);
      return;
    }
    const bool was_open = disk.get_eject_state();
    const auto previous = disk.get_image_index();
    const bool changed = (was_open || disk.set_eject_state(true)) &&
        disk.set_image_index(*requested) && disk.set_eject_state(false) &&
        !disk.get_eject_state() && disk.get_image_index() == *requested;
    if (!changed && (disk.get_eject_state() != was_open || disk.get_image_index() != previous)) {
      // Restore the previous disc only when its index is known to be selected.
      if (disk.get_image_index() != previous &&
          (disk.get_eject_state() || disk.set_eject_state(true)))
        disk.set_image_index(previous);
      if (disk.get_image_index() == previous && disk.get_eject_state() != was_open)
        disk.set_eject_state(was_open);
    }
    RefreshDiscs(!changed);
  }

  void ConfirmFingerprint() {
    if (!pending_fingerprint_ || match_timeline_->confirmed() < pending_fingerprint_->first) return;
    if (match_fingerprints_.size() == 16) match_fingerprints_.erase(match_fingerprints_.begin());
    match_fingerprints_.push_back(std::move(*pending_fingerprint_));
    pending_fingerprint_.reset();
  }

  struct MatchAudio {
    std::array<int16_t, 8192> samples;
    size_t count = 0;
    bool overflow = false;
  };

  void WriteAudio(const int16_t* samples, size_t frames) {
    if (frames == 0) return;
    if (auto* capture = audio_capture_) {
      if (frames > (capture->samples.size() - capture->count) / 2) {
        capture->overflow = true;
        return;
      }
      std::copy_n(samples, frames * 2, capture->samples.data() + capture->count);
      capture->count += frames * 2;
    } else if (!paused_ && !suppress_frame_ && recovery_accepted_) {
      audio_frames_ += frames;
      audio_->Write(samples, frames);
    }
  }

  bool Environment(unsigned command, void* data) {
    constexpr unsigned kSavestateContext = 72 | 0x10000;
    if (command == kSavestateContext) {
      if (!rollback_context_) return false;
      if (data) {
        *static_cast<int*>(data) = 1;  // Same core instance.
        rollback_context_queried_ = true;
      }
      return true;
    }
    if (command == kShutdown) {
      stop_ = true;
      return true;
    }
    if (command == kGetInputBitmasks) return true;
    if (!data) return false;
    switch (command) {
      case kMessageVersion:
        if (!ps3_) return false;
        *static_cast<unsigned*>(data) = 1;
        return true;
      case kFrontendCompileProgress:
        *static_cast<bool*>(data) = ps3_;
        return ps3_;
      case kExtendedMessage: {
        if (!ps3_) return false;
        const auto& message = *static_cast<const ExtendedMessage*>(data);
        if (config_.ps3_online && message.type == 0 && message.level == 3 && message.target != 2 && message.text) {
          std::lock_guard<std::mutex> lock(settings_mutex_);
          error_ = message.text;
          return true;
        }
        if (message.type != 3 || message.target == 2 || !message.text ||
            message.progress < -1 || message.progress > 100)
          return false;
        std::lock_guard<std::mutex> lock(settings_mutex_);
        progress_label_ = message.text;
        progress_percent_ = message.progress;
        progress_until_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::min(message.duration, 5000u));
        return true;
      }
      case kDiskControlVersion:
        if (ps3_) return false;
        *static_cast<unsigned*>(data) = 1;
        return true;
      case kSetDiskControl:
        if (ps3_) return false;
        disk_callbacks_ = {};
        disk_callbacks_.basic = *static_cast<const DiskControlCallbacks*>(data);
        return true;
      case kSetDiskControlExt:
        if (ps3_) return false;
        disk_callbacks_ = *static_cast<const DiskControlExtCallbacks*>(data);
        return true;
      case kSetHardware: {
        auto& callbacks = *static_cast<HardwareCallbacks*>(data);
        if (!hardware_available_ || !video_->Bind(callbacks)) return false;
        hardware_callbacks_ = callbacks;
        return true;
      }
      case kHardwareNegotiation:
      case kHardwareInterface:
        return video_->Environment(command, data, hardware_callbacks_.context_type);
      case kPreferredHardware:
        if (!hardware_available_) return false;
        *static_cast<int*>(data) = video_->PreferredContext();
        return true;
      case kCanDupe:
        *static_cast<bool*>(data) = true;
        return true;
      case kSystemDirectory:
        *static_cast<const char**>(data) = config_.system.c_str();
        return true;
      case kCorePath:
        *static_cast<const char**>(data) = config_.core.c_str();
        return true;
      case kContentDirectory:
        *static_cast<const char**>(data) = content_.c_str();
        return true;
      case kSaveDirectory:
        *static_cast<const char**>(data) = config_.saves.c_str();
        return true;
      case kPixelFormat: {
        const auto value = *static_cast<const int*>(data);
        if (value < 0 || value > 2) return false;
        pixel_format_ = value;
        return true;
      }
      case kGetVariable: {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        auto& variable = *static_cast<Variable*>(data);
        const auto found = variable.key ? options_.find(variable.key) : options_.end();
        variable.value = found == options_.end() ? nullptr :
            found->second.values[found->second.selected].c_str();
        return true;
      }
      case kSetVariables:
        SetVariables(static_cast<const Variable*>(data));
        return true;
      case kVariableUpdate: {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        *static_cast<bool*>(data) = std::exchange(options_changed_, false);
        return true;
      }
      case kInputDescriptors:
      case kSupportNoGame:
        return true;
      case kSetAv: {
        const auto& geometry = static_cast<const AvInfo*>(data)->geometry;
        video_->GeometryChanged(geometry);
        SetAspect(geometry.aspect);
        return true;
      }
      case kSetGeometry: {
        const auto& geometry = *static_cast<const Geometry*>(data);
        video_->GeometryChanged(geometry);
        SetAspect(geometry.aspect);
        return true;
      }
      case kFastForward:
        *static_cast<bool*>(data) = false;
        return true;
      case kAudioVideoEnable:
        *static_cast<unsigned*>(data) = 3;
        return true;
      case kCoreOptionsVersion:
        *static_cast<unsigned*>(data) = ps3_ ? 2u : 0u;
        return true;
      case kSetCoreOptionsV2:
        SetCoreOptions(static_cast<const CoreOptionsV2*>(data));
        return ps3_;
      case kSetCoreOptionsV2Intl:
        SetCoreOptions(static_cast<const CoreOptionsV2Intl*>(data)->english);
        return ps3_;
      default:
        return false;
    }
  }

  int16_t Input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (match_timeline_) {
      if (port >= match_ports_.size()) return 0;
      const auto& pad = match_ports_[port];
      if (device == 1 && id == 256) return static_cast<int16_t>(pad.buttons);
      if (device == 1 && id < 16) return (pad.buttons & (1u << id)) != 0;
      if (device == 5 && index < 2 && id < 2) return pad.axes[index * 2 + id];
      return 0;
    }
    if (suppress_frame_) return 0;
    if (port >= kLocalPorts || paused_) return 0;
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (device == 1 && id == 256) return static_cast<int16_t>(buttons_[port]);
    if (device == 1 && id < 16) return (buttons_[port] & (1u << id)) != 0;
    if (device == 5 && index < 2 && id < 2) return axes_[port][index * 2 + id];
    return 0;
  }

  void Run() {
    using Clock = std::chrono::steady_clock;
    using VoidFunction = void (*)();
    std::optional<DynamicLibrary> library;
    VoidFunction deinit = nullptr;
    VoidFunction unload = nullptr;
    bool initialized = false;
    bool loaded = false;
    Impl* idle = nullptr;
    if (!active_.compare_exchange_strong(idle, this)) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      error_ = "An embedded game is already running";
      ended_ = true;
      return;
    }
    try {
      if (config_.ps3_online && (!ps3_ || match_timeline_ || lan_))
        throw std::runtime_error("PS3 online requires a dedicated PS3 session.");
      hardware_available_ = !match_timeline_ && video_->Open();
      if (ps3_ && !hardware_available_)
        throw std::runtime_error("PS3 requires a working Vulkan driver.");
      const auto path = std::filesystem::u8path(config_.core);
      const auto& core = library.emplace(path);
      if (core.Get<unsigned (*)()>("retro_api_version")() != 1)
        throw std::runtime_error("Unsupported emulator core API");
      using ReadProviderStatus = int32_t (*)();
      ReadProviderStatus read_provider_status = nullptr;
      if (config_.ps3_online) {
        read_provider_status = core.Get<ReadProviderStatus>("retro_anyps5_ps3_provider_status_v1");
        ApplyPs3OnlinePolicy(core.Get<Ps3VerifiedOnlinePolicy>(kPs3VerifiedOnlinePolicyExport, false), *config_.ps3_online);
        ps3_provider_status_ = 0;
      } else if (ps3_) {
        ApplyPs3OfflinePolicy(core.Get<Ps3OfflinePolicy>(kPs3OfflinePolicyExport, false), false);
      }
      const auto init = core.Get<VoidFunction>("retro_init");
      deinit = core.Get<VoidFunction>("retro_deinit");
      unload = core.Get<VoidFunction>("retro_unload_game");
      const auto load = core.Get<bool (*)(const GameInfo*)>("retro_load_game");
      const auto step = core.Get<VoidFunction>("retro_run");
      const auto av_info = core.Get<void (*)(AvInfo*)>("retro_get_system_av_info");
      const auto bind_lan = lan_ ? core.Get<Ps2VirtualLan::Bind>("retro_anyps5_dev9_bind_v1") : nullptr;
      core.Get<void (*)(bool (*)(unsigned, void*))>("retro_set_environment")(
          [](unsigned command, void* data) { return active_.load()->Environment(command, data); });
      const auto initial_disk_callbacks = disk_callbacks_;
      core.Get<void (*)(void (*)(const void*, unsigned, unsigned, size_t))>("retro_set_video_refresh")(
          [](const void* data, unsigned width, unsigned height, size_t pitch) {
            active_.load()->Publish(data, width, height, pitch);
          });
      core.Get<void (*)(void (*)(int16_t, int16_t))>("retro_set_audio_sample")(
          [](int16_t left, int16_t right) {
            const int16_t stereo[]{left, right};
            active_.load()->WriteAudio(stereo, 1);
          });
      core.Get<void (*)(size_t (*)(const int16_t*, size_t))>("retro_set_audio_sample_batch")(
          [](const int16_t* samples, size_t frames) {
            active_.load()->WriteAudio(samples, frames);
            return frames;
          });
      core.Get<void (*)(void (*)())>("retro_set_input_poll")([] {});
      core.Get<void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned))>("retro_set_input_state")(
          [](unsigned port, unsigned device, unsigned index, unsigned id) {
            return active_.load()->Input(port, device, index, id);
          });
      const GameInfo game{config_.disc.c_str(), nullptr, 0, nullptr};
      const auto reopen = [&] {
        lan_ready_ = false;
        if (loaded) unload();
        loaded = false;
        if (lan_ && !lan_->Detach()) throw std::runtime_error("Could not detach the private PS2 network");
        if (initialized) deinit();
        loaded = initialized = false;
        video_->Clear();
        hardware_callbacks_ = {};
        disk_callbacks_ = initial_disk_callbacks;
        init();
        initialized = true;
        if (lan_) {
          {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            const auto eth = options_.find("pcsx2_dev9_eth");
            if (eth == options_.end() || eth->second.values[eth->second.selected] != "enabled")
              throw std::runtime_error("The core cannot enable the private PS2 network");
          }
          if (!lan_->Attach(bind_lan)) throw std::runtime_error("Could not attach the private PS2 network");
        }
        if (ps3_ && hardware_callbacks_.context_type != video_->PreferredContext())
          throw std::runtime_error("The PS3 core did not request a Vulkan context.");
        if (ps3_) {
          const auto controller = core.Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device");
          controller(0, 1);
          for (unsigned port = 1; port < 7; ++port) controller(port, 0);
        }
        const auto& initial_disc = config_.initial_disc;
        if (initial_disc &&
            (!disk_callbacks_.set_initial_image ||
             !disk_callbacks_.set_initial_image(initial_disc->index, initial_disc->path.c_str()))) {
          throw std::runtime_error("The core cannot boot the selected disc.");
        }
        loaded = load(&game);
        if (loaded && !match_timeline_ && console_ == Console::kPs1) {
          if (const auto controller = core.Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device", false))
            controller(1, kPs1DualShockDevice);
        }
        if (loaded && match_timeline_) {
          const auto controller = core.Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device");
          const auto device = config_.ps1_match->dualshock ? kPs1DualShockDevice : 1u;
          controller(0, device);
          controller(1, device);
        }
        lan_ready_ = loaded && lan_ != nullptr;
        if (loaded && initial_disc) {
          const auto& disk = disk_callbacks_.basic;
          std::error_code error;
          if (!disk.get_image_index || disk.get_image_index() != initial_disc->index ||
              !disk.get_eject_state || disk.get_eject_state() ||
              !std::filesystem::equivalent(std::filesystem::u8path(DiscPath(initial_disc->index)),
                                           std::filesystem::u8path(initial_disc->path), error)) {
            throw std::runtime_error("The core did not boot the selected disc.");
          }
        }
        if (loaded && hardware_callbacks_.reset) {
          AvInfo av{};
          av_info(&av);
          video_->GeometryChanged(av.geometry);
          video_->Reset(hardware_callbacks_);
        }
        if (loaded) RefreshDiscs();
        return loaded;
      };
      const bool ps2 = console_ == Console::kPs2;
      const bool ps1 = console_ == Console::kPs1;
      const auto use_native_resolution = [&] {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        const auto scale = options_.find(ps2 ? "pcsx2_upscale_multiplier" : "swanstation_GPU_ResolutionScale");
        return scale != options_.end() && std::exchange(scale->second.selected, 0) != 0;
      };
      if (!reopen() && (config_.ps3_online || config_.recovery || !use_native_resolution() || !reopen()))
        throw std::runtime_error("The core could not boot this disc. Check its BIOS and game files.");
      void* card = nullptr;
      size_t card_size = 0;
      std::filesystem::path card_path;
      const auto restore_card = [&] {
        if (!ps1 || match_timeline_) return;
        card = core.Get<void* (*)(unsigned)>("retro_get_memory_data")(0);
        card_size = core.Get<size_t (*)(unsigned)>("retro_get_memory_size")(0);
        card_path = std::filesystem::u8path(config_.saves) / std::filesystem::u8path(config_.disc).stem();
        card_path += ".srm";
        if (!config_.recovery) RestoreCard(card_path, card, card_size);
      };
      restore_card();
      if (ps1 && !match_timeline_) {
        const auto blank = core.Get<ps1_checkpoint::Save>("retro_anyps5_export_blank_card_checkpoint_v1", false);
        const auto private_save = core.Get<ps1_checkpoint::Save>("retro_anyps5_export_checkpoint_v1", false);
        const auto load_private = core.Get<ps1_checkpoint::Load>("retro_anyps5_import_checkpoint_v1", false);
        const auto size = blank ? core.Get<size_t (*)()>("retro_serialize_size")() : 0;
        if (size && size <= ps1_checkpoint::kMaximumBytes - ps1_checkpoint::kPrefixBytes)
          checkpoint_.Configure({blank, private_save, load_private, size + ps1_checkpoint::kPrefixBytes});
        if (config_.recovery) {
          {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            for (const auto& [key, value] : config_.initial_graphics) {
              const auto option = options_.find(key);
              if (option == options_.end() || option->second.values[option->second.selected] != value)
                throw std::runtime_error("The original PS1 settings are unavailable.");
            }
          }
          const auto& recovery = *config_.recovery;
          if (!load_private || !load_private(recovery.state.data(), recovery.state.size(), recovery.digest.data()))
            throw std::runtime_error("The emulator could not recover the original PS1 game.");
          card = core.Get<void* (*)(unsigned)>("retro_get_memory_data")(0);
          card_size = core.Get<size_t (*)(unsigned)>("retro_get_memory_size")(0);
          const auto source = recovery.source;
          {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            config_.recovery.reset();
          }
          suppress_frame_ = false;
          recovered_source_ = source;
        }
      }
      AvInfo av{};
      av_info(&av);
      SetAspect(av.geometry.aspect);
      const bool audio_open = audio_->Open(av.sample_rate);
      const double fps = std::isfinite(av.fps) && av.fps > 30 && av.fps <= 120 ? av.fps : 60.0;
      {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        target_fps_ = fps;
        audio_rate_ = audio_open ? av.sample_rate : 0;
      }
      const auto frame_time = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
      const uint8_t* match_ram = nullptr;
      constexpr size_t match_ram_size = 2 * 1024 * 1024;
      std::function<void()> validate_match_memory;
      if (match_timeline_) {
        validate_match_memory = [&] {
          match_ram = static_cast<const uint8_t*>(core.Get<void* (*)(unsigned)>("retro_get_memory_data")(2));
          if (!match_ram || core.Get<size_t (*)(unsigned)>("retro_get_memory_size")(2) != match_ram_size)
            throw std::runtime_error("PS1 netplay could not access the core's main RAM.");
        };
        validate_match_memory();
        {
          std::lock_guard<std::mutex> lock(settings_mutex_);
          for (const auto& [key, value] : kPs1NetplayOptions) {
            const auto option = options_.find(std::string(key));
            if (option == options_.end() || option->second.values[option->second.selected] != value)
              throw std::runtime_error("The PS1 core could not configure the required netplay profile.");
          }
        }
        const auto save = core.Get<ps1_checkpoint::Save>("retro_anyps5_export_checkpoint_v1", false);
        const auto load_state = core.Get<ps1_checkpoint::Load>("retro_anyps5_import_checkpoint_v1", false);
        const auto size = save && load_state ? core.Get<size_t (*)()>("retro_serialize_size")() : 0;
        if (size && size <= ps1_checkpoint::kMaximumBytes - ps1_checkpoint::kPrefixBytes)
          checkpoint_.Configure({save, nullptr, load_state, size + ps1_checkpoint::kPrefixBytes});
        if (config_.ps1_match->local_staging && !checkpoint_.ready())
          throw std::runtime_error("Local PS1 hosting requires the portable checkpoint core.");
      }
      const bool rollback = match_timeline_ && config_.ps1_match->rollback;
      const auto serialize = rollback ? core.Get<bool (*)(void*, size_t)>("retro_serialize") : nullptr;
      const auto restore = rollback ? core.Get<bool (*)(const void*, size_t)>("retro_unserialize") : nullptr;
      const auto state_size = rollback ? core.Get<size_t (*)()>("retro_serialize_size")() : 0;
      if (rollback && (!state_size || state_size > ps1_checkpoint::kMaximumBytes))
        throw std::runtime_error("Invalid PS1 rollback checkpoint size.");
      const auto window = rollback ? match_timeline_->prediction_window() : 0;
      struct Snapshot {
        std::optional<uint32_t> frame;
        ps1_checkpoint::Bytes bytes;
      };
      std::vector<Snapshot> snapshots(window);
      std::vector<MatchAudio> match_audio(rollback ? window + 1 : 0);
      uint32_t next_audio = 0;
      if (match_timeline_) match_ready_ = true;
      auto next = Clock::now();
      auto hardware_deadline = next + std::chrono::seconds(8);
      std::optional<Clock::time_point> recovery_deadline;
      const std::function<uint64_t()> checkpoint_frame = [&]() -> uint64_t {
        if (!match_timeline_) return static_cast<uint64_t>(steps_.load());
        std::lock_guard<std::mutex> lock(match_mutex_);
        return MatchFrame();
      };
      while (!stop_) {
        if (!match_timeline_ && !checkpoint_.busy()) ApplyDiscChange();
        if (!match_timeline_ && (ps1 || ps2) && !checkpoint_.busy())
          ApplyCheckpointGraphics(core, reopen, step, ps1, card, card_size, card_path);
        if (!recovered_source_ && !ps3_ && hardware_callbacks_.reset && !frame_ready_ && Clock::now() > hardware_deadline) {
          if (!use_native_resolution()) {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            auto& renderer = options_.at(ps2 ? "pcsx2_renderer" : "swanstation_GPU_Renderer");
            const auto software = std::find(renderer.values.begin(), renderer.values.end(),
                                            ps2 ? (video_->PreferredRenderer() == "D3D11" ? "Software (HW)" : "Software (SW)")
                                                : "Software");
            if (software == renderer.values.end())
              throw std::runtime_error("The core could not present hardware frames.");
            renderer.selected = static_cast<size_t>(software - renderer.values.begin());
          }
          audio_->Flush();
          if (card) StoreCard(card_path, card, card_size);
          if (!reopen()) throw std::runtime_error("The core could not restart after a graphics startup failure.");
          restore_card();
          hardware_deadline = Clock::now() + std::chrono::seconds(8);
        }
        const auto checkpoint_effects = checkpoint_.Service(checkpoint_frame, validate_match_memory);
        if (checkpoint_effects.refresh_memory && ps1 && !match_timeline_) {
          card = core.Get<void* (*)(unsigned)>("retro_get_memory_data")(0);
          card_size = core.Get<size_t (*)(unsigned)>("retro_get_memory_size")(0);
        }
        if (checkpoint_effects.flush_audio) audio_->Flush();
        if (match_timeline_) ServiceMatchReplay(step);
        if (stop_) break;
        if (recovered_source_ && !frame_ready_ && !paused_) {
          const auto now = Clock::now();
          if (!recovery_deadline) recovery_deadline = now + std::chrono::seconds(8);
          if (now >= *recovery_deadline)
            throw std::runtime_error("The restored PS1 game did not present a visible frame.");
        } else {
          recovery_deadline.reset();
        }
        if ((paused_ && (frame_ready_ || recovered_source_)) || checkpoint_.parked()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          next = Clock::now();
          continue;
        }
        bool replaying = false;
        if (rollback) {
          std::optional<uint32_t> confirmed, correction;
          {
            std::lock_guard<std::mutex> lock(match_mutex_);
            confirmed = match_timeline_->confirmed();
            correction = match_timeline_->correction();
          }
          while (confirmed && next_audio <= *confirmed) {
            const auto& pcm = match_audio[next_audio % match_audio.size()];
            if (pcm.count) {
              audio_->Write(pcm.samples.data(), pcm.count / 2);
              audio_frames_ += pcm.count / 2;
            }
            confirmed_audio_frame_ = next_audio++;
          }
          if (correction) {
            const auto& snapshot = snapshots[*correction % window];
            if (snapshot.frame != correction)
              throw std::runtime_error("Missing PS1 rollback checkpoint.");
            suppress_frame_ = true;
            rollback_context_queried_ = false;
            rollback_context_ = true;
            const bool restored = restore(snapshot.bytes.data(), snapshot.bytes.size());
            rollback_context_ = false;
            if (!restored || !rollback_context_queried_)
              throw std::runtime_error("The PS1 core rejected a rollback checkpoint.");
            validate_match_memory();
            std::lock_guard<std::mutex> lock(match_mutex_);
            if (match_timeline_->Rewind(*correction)) {
              ++rollback_corrections_;
              if (pending_fingerprint_ && pending_fingerprint_->first >= *correction)
                pending_fingerprint_.reset();
            }
            continue;
          }
        }
        if (match_timeline_) {
          ps1_netplay::PadInput local;
          {
            std::lock_guard<std::mutex> lock(input_mutex_);
            local = {buttons_[0], axes_[0]};
          }
          bool prepared;
          bool staging;
          bool checkpoint_needed = false;
          uint32_t frame;
          {
            std::lock_guard<std::mutex> lock(match_mutex_);
            staging = StagingLocal();
            frame = staging ? MatchFrame() : match_timeline_->cursor();
            if (staging) {
              if (frame == ps1_netplay::Timeline::kMaxFrame)
                throw std::runtime_error("PS1 staging reached its frame limit.");
              prepared = !match_held_;
              match_ports_ = {local, {}};
            } else {
              const auto& trace = config_.ps1_match->input_trace;
              if (!trace.empty())
                local = frame + ps1_netplay::Timeline::kDelay < trace.size()
                    ? trace[frame + ps1_netplay::Timeline::kDelay] : ps1_netplay::PadInput{};
              prepared = (trace.empty() || frame < trace.size()) &&
                  match_timeline_->Prepare(local, match_ports_);
              checkpoint_needed = match_timeline_->needs_checkpoint();
              replaying = frame < match_timeline_->frame();
            }
            match_frame_in_flight_ = prepared;
          }
          if (!prepared) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            next = Clock::now();
            continue;
          }
          if (stop_) break;
          if (checkpoint_needed) {
            auto& snapshot = snapshots[frame % window];
            snapshot.bytes.resize(state_size);
            rollback_context_ = true;
            const bool saved = serialize(snapshot.bytes.data(), snapshot.bytes.size());
            rollback_context_ = false;
            if (!saved) throw std::runtime_error("Could not save a PS1 rollback checkpoint.");
            snapshot.frame = frame;
          }
          suppress_frame_ = replaying;
          if (rollback && !staging) {
            audio_capture_ = &match_audio[frame % match_audio.size()];
            audio_capture_->count = 0;
            audio_capture_->overflow = false;
          }
          step();
          auto* captured = std::exchange(audio_capture_, nullptr);
          if (captured && captured->overflow) throw std::runtime_error("PS1 rollback audio overflow.");
          auto fingerprint = !staging && (frame + 1) % 120 == 0 ?
              NetplayFingerprint(match_ram, match_ram_size) : std::string{};
          std::lock_guard<std::mutex> lock(match_mutex_);
          if (staging) ++match_local_frame_;
          else match_timeline_->Commit();
          match_frame_in_flight_ = false;
          if (checkpoint_.ready() && (staging || !rollback)) {
            if (match_history_.size() == 1024) match_history_.pop_front();
            match_history_.push_back(match_ports_);
          }
          if (!fingerprint.empty()) pending_fingerprint_ = std::make_pair(frame, std::move(fingerprint));
          ConfirmFingerprint();
        } else {
          step();
        }
        if (read_provider_status) ps3_provider_status_ = read_provider_status();
        if (!paused_) ++steps_;
        if (video_->UnsupportedSync())
          throw std::runtime_error("This RPCS3 core uses Vulkan synchronization AnyPS5 does not support.");
        if (replaying) continue;
        next += frame_time;
        const auto now = Clock::now();
        if (next < now - frame_time) next = now;
        std::this_thread::sleep_until(next);
      }
      if (card && recovery_accepted_) StoreCard(card_path, card, card_size);
    } catch (const std::exception& failure) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      if (!config_.ps3_online || error_.empty()) error_ = failure.what();
    }
    audio_capture_ = nullptr;
    rollback_context_ = false;
    lan_ready_ = false;
    checkpoint_.Stop();
    if (lan_) lan_->Close();
    if (loaded) unload();
    const bool lan_detached = !lan_ || lan_->Detach();
    video_->BeforeCoreDeinit(hardware_callbacks_);
    if (initialized) deinit();
    if (!lan_detached && !lan_->Detach()) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      error_ = "The core did not release its private PS2 network";
    }
    video_->Close();
    audio_->Close();
    library.reset();
    active_ = nullptr;
    ended_ = true;
  }

  static std::atomic<Session::Impl*> active_;
  SessionConfig config_;
  std::string content_;
  const Console console_;
  const bool ps3_;
  const std::function<void()> frame_available_;
  std::thread worker_;
  std::unique_ptr<Video> video_;
  std::unique_ptr<Audio> audio_;
  std::unique_ptr<Ps2VirtualLan> lan_;
  ps1_checkpoint::Mailbox checkpoint_;
  std::unique_ptr<Ps1MatchStorage> match_storage_;
  std::optional<ps1_netplay::Timeline> match_timeline_;
  std::atomic<bool> match_ready_{false};
  bool match_started_ = false;
  bool match_held_ = false, match_frame_in_flight_ = false;
  uint32_t match_local_frame_ = 0;
  std::optional<std::pair<int64_t, uint32_t>> match_import_;
  std::optional<uint32_t> match_replay_cursor_;
  std::optional<Replay> match_replay_;
  std::deque<Ps1Ports> match_history_;
  std::mutex match_mutex_;
  std::array<ps1_netplay::PadInput, 2> match_ports_{};
  std::vector<std::pair<uint32_t, std::string>> match_fingerprints_;
  std::optional<std::pair<uint32_t, std::string>> pending_fingerprint_;
  std::atomic<int64_t> confirmed_audio_frame_{-1};
  std::atomic<uint64_t> rollback_corrections_{0};
  std::atomic<bool> rollback_context_{false}, rollback_context_queried_{false};
  inline static thread_local MatchAudio* audio_capture_ = nullptr;
  std::atomic<bool> recovery_accepted_{true};
  std::atomic<int64_t> recovered_source_{0};
  std::atomic<bool> lan_ready_{false};
  HardwareCallbacks hardware_callbacks_{};
  DiskControlExtCallbacks disk_callbacks_{};
  bool hardware_available_ = false;
  uint64_t hardware_frames_ = 0;
  std::mutex telemetry_mutex_;
  std::chrono::steady_clock::time_point last_frame_at_{};
  std::array<int64_t, 13> frame_intervals_{};
  int64_t video_frames_ = 0;
  double target_fps_ = 0;
  double audio_rate_ = 0;
  std::atomic<int64_t> steps_{0};
  std::atomic<int64_t> audio_frames_{0};
  std::atomic<bool> hardware_frame_{false};
  std::atomic<unsigned> frame_width_{0}, frame_height_{0};
  std::atomic<bool> stop_{false}, paused_{false}, ended_{false}, frame_ready_{false}, presented_{false};
  std::atomic<int> pixel_format_{0};
  std::atomic<float> aspect_{4.f / 3.f};
  std::mutex frame_mutex_, input_mutex_, settings_mutex_;
  std::shared_ptr<Frame> frame_;
  static constexpr unsigned kLocalPorts = 4;
  std::array<uint16_t, kLocalPorts> buttons_{};
  std::array<std::array<int16_t, 4>, kLocalPorts> axes_{};
  std::map<std::string, Option> options_;
  std::optional<DiscStatus> discs_;
  int64_t disc_inserted_at_step_ = 0;
  std::optional<unsigned> pending_disc_;
  bool options_changed_ = false;
  bool graphics_queued_ = false;
  bool graphics_applying_ = false;
  std::atomic<bool> suppress_frame_{false};
  std::chrono::steady_clock::time_point progress_until_{};
  std::string progress_label_;
  int32_t progress_percent_ = -1;
  std::atomic<int32_t> ps3_provider_status_{-1};
  std::string error_, resolution_error_;
};

std::atomic<Session::Impl*> Session::Impl::active_{nullptr};

Session::Session(SessionConfig config, std::unique_ptr<Video> video, std::unique_ptr<Audio> audio,
                 std::function<void()> frame_available)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(video), std::move(audio),
                                   std::move(frame_available))) {}
Session::~Session() = default;
void Session::Start() { impl_->Start(); }
bool Session::ended() const { return impl_->ended(); }
void Session::Stop() { impl_->Stop(); }
void Session::Join() { impl_->Join(); }
void Session::Pause(bool pause) { impl_->Pause(pause); }
bool Session::ChangeDisc(unsigned index) { return impl_->ChangeDisc(index); }
void Session::SetInput(uint16_t buttons, const std::array<int16_t, 4>& axes) { impl_->SetInput(0, buttons, axes); }
void Session::SetInput(unsigned port, uint16_t buttons, const std::array<int16_t, 4>& axes) {
  impl_->SetInput(port, buttons, axes);
}
std::optional<LanFrames> Session::ExchangeLan(const LanFrames& input) { return impl_->ExchangeLan(input); }
bool Session::ClearLanFrames() { return impl_->ClearLanFrames(); }
std::optional<int64_t> Session::RequestOrdinaryCheckpoint(int64_t source, const ps1_checkpoint::Digest& digest, bool park) {
  return impl_->RequestOrdinaryCheckpoint(source, digest, park);
}
std::optional<int64_t> Session::RequestOrdinaryResume(int64_t source) { return impl_->RequestOrdinaryResume(source); }
std::optional<SessionConfig> Session::DetachOrdinaryRecovery(int64_t source) { return impl_->DetachOrdinaryRecovery(source); }
bool Session::AcceptOrdinaryRecovery(int64_t source) { return impl_->AcceptOrdinaryRecovery(source); }
bool Session::StartNetplay() { return impl_->StartNetplay(); }
std::optional<int64_t> Session::RequestMatchExport(const ps1_checkpoint::Digest& digest) {
  return impl_->RequestMatchExport(digest);
}
std::optional<int64_t> Session::RequestMatchImport(const ps1_checkpoint::Digest& digest, uint32_t frame,
                                                   ps1_checkpoint::Bytes state) {
  return impl_->RequestMatchImport(digest, frame, std::move(state));
}
std::optional<uint32_t> Session::HoldNetplay() { return impl_->HoldNetplay(); }
bool Session::ResumeLocalNetplay() { return impl_->ResumeLocalNetplay(); }
std::optional<Ps1MatchHistory> Session::MatchHistory(uint32_t first) { return impl_->MatchHistory(first); }
std::optional<int64_t> Session::RequestMatchReplay(uint32_t first, std::vector<Ps1Ports> frames) {
  return impl_->RequestMatchReplay(first, std::move(frames));
}
std::optional<Ps1MatchTick> Session::SyncNetplay(const std::vector<ps1_netplay::FrameInput>& remote,
                                                 std::optional<uint32_t> acknowledged) {
  return impl_->SyncNetplay(remote, acknowledged);
}
ps1_checkpoint::Mailbox::Reply Session::TakeCheckpoint(int64_t ticket) { return impl_->TakeCheckpoint(ticket); }
void Session::CancelCheckpoint(int64_t ticket) { impl_->CancelCheckpoint(ticket); }
bool Session::SetOption(const std::string& key, const std::string& value) { return impl_->SetOption(key, value); }
bool Session::SetTuningOptions(const Options& expected, const Options& values) {
  return impl_->SetTuningOptions(expected, values);
}
SessionStatus Session::Status() { return impl_->Status(); }
std::shared_ptr<const Frame> Session::AcquireFrame() { return impl_->AcquireFrame(); }
std::shared_ptr<const Frame> Session::LatestFrame() { return impl_->LatestFrame(); }
void Session::MarkPresented() { impl_->MarkPresented(); }

}  // namespace anyps5::retro
