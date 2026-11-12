#include "host_server.h"

#include "retro_linux_channel.h"
#include "retro_linux_platform.h"
#if defined(ANYPS5_RETRO_VULKAN)
#include "retro_vulkan_video.h"
#endif

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace anyps5::retro_host {
namespace {
using namespace anyps5::retro;
namespace lc = anyps5::retro::linux_channel;

Value Int(int64_t value) { return Value(value); }

Value Strings(const std::vector<std::string>& values) {
  List result;
  for (const auto& value : values) result.emplace_back(value);
  return Value(std::move(result));
}

Value EncodeStatus(const SessionStatus& status) {
  Map result{
      {Value("ready"), Value(status.ready)},
      {Value("presented"), Value(status.presented)},
      {Value("ended"), Value(status.ended)},
      {Value("netplayReady"), Value(status.netplay_ready)},
      {Value("portableCheckpoint"), Value(status.portable_checkpoint)},
      {Value("error"), Value(status.error)},
      {Value("aspect"), Value(status.aspect)},
      {Value("hardware"), Value(status.hardware)},
      {Value("frameWidth"), Int(status.frame_width)},
      {Value("frameHeight"), Int(status.frame_height)},
      {Value("resolutionError"), Value(status.resolution_error)},
      {Value("graphicsPending"), Value(status.graphics_pending)},
      {Value("ordinaryCheckpointParked"), Value(status.checkpoint_parked)},
      {Value("progressLabel"), status.progress_label ? Value(*status.progress_label) : Value()},
      {Value("progressPercent"), status.progress_percent ? Int(*status.progress_percent) : Value()},
  };
  if (status.ps3_provider_status >= 0) result.emplace(Value("ps3ProviderStatus"), Int(status.ps3_provider_status));
  if (status.checkpoint_source > 0) result.emplace(Value("localBlankCardCheckpoint"), Int(status.checkpoint_source));
  if (status.recovered_source > 0) result.emplace(Value("ps1RecoveredSource"), Int(status.recovered_source));
  if (const auto& discs = status.discs) {
    result.emplace(Value("discs"), Value(Map{
        {Value("labels"), Strings(discs->labels)},
        {Value("inserted"), discs->inserted ? Int(*discs->inserted) : Value()},
        {Value("playedDiscPath"), Value(discs->played ? discs->inserted_path : std::string())},
        {Value("busy"), Value(discs->busy)},
        {Value("failed"), Value(discs->failed)},
    }));
  }
  Map options;
  for (const auto& [key, option] : status.options) {
    options.emplace(Value(key), Value(Map{
        {Value("values"), Strings(option.values)},
        {Value("labels"), Strings(option.labels)},
        {Value("title"), Value(option.title)},
        {Value("restart"), Value(option.restart)},
        {Value("selected"), Int(option.selected)},
        {Value("activeValue"), Value(option.active_value)},
    }));
  }
  result.emplace(Value("options"), Value(std::move(options)));
  const auto& telemetry = status.telemetry;
  List intervals;
  for (const auto count : telemetry.frame_intervals) intervals.emplace_back(Int(count));
  result.emplace(Value("tuningTelemetry"), Value(Map{
      {Value("clockMs"), Int(telemetry.clock_ms)},
      {Value("frames"), Int(telemetry.frames)},
      {Value("steps"), Int(telemetry.steps)},
      {Value("audioFrames"), Int(telemetry.audio_frames)},
      {Value("droppedAudioFrames"), Int(telemetry.dropped_audio_frames)},
      {Value("frameIntervals"), Value(std::move(intervals))},
      {Value("targetFps"), Value(telemetry.target_fps)},
      {Value("audioRate"), Value(telemetry.audio_rate)},
  }));
  return Value(std::move(result));
}

Value EncodeMatchTick(const Ps1MatchTick& tick) {
  List local;
  for (const auto& sample : tick.local) {
    List row{Int(sample.frame), Int(sample.input.buttons)};
    for (const auto axis : sample.input.axes) row.emplace_back(Int(axis));
    local.emplace_back(std::move(row));
  }
  List fingerprints;
  for (const auto& [frame, hash] : tick.fingerprints) fingerprints.emplace_back(List{Int(frame), Value(hash)});
  return Value(Map{
      {Value("local"), Value(std::move(local))},
      {Value("fingerprints"), Value(std::move(fingerprints))},
      {Value("frame"), Int(tick.frame)},
      {Value("confirmedFrame"), tick.confirmed ? Int(*tick.confirmed) : Value()},
      {Value("receivedFrame"), tick.received ? Int(*tick.received) : Value()},
      {Value("waiting"), Value(tick.waiting)},
  });
}

Value EncodeMatchHistory(const Ps1MatchHistory& history) {
  List frames;
  for (const auto& frame : history.frames) {
    List ports;
    for (const auto& pad : frame) {
      List row{Int(pad.buttons)};
      for (const auto axis : pad.axes) row.emplace_back(Int(axis));
      ports.emplace_back(std::move(row));
    }
    frames.emplace_back(std::move(ports));
  }
  return Value(Map{{Value("frames"), Value(std::move(frames))}, {Value("nextFrame"), Int(history.next_frame)}});
}

std::optional<Options> ReadOptions(const Value* values) {
  const auto* map = lc::Map(values);
  if (!map) return std::nullopt;
  Options options;
  for (const auto& [key, value] : *map) {
    const auto* name = std::get_if<std::string>(&key);
    const auto* text = std::get_if<std::string>(&value);
    if (!name || !text) return std::nullopt;
    options.emplace(*name, *text);
  }
  return options;
}

const char* StringField(const Value* fields, const char* key) {
  const auto* value = lc::String(lc::Field(fields, key));
  return value ? value->c_str() : nullptr;
}

void SetInput(Session& session, const Value* input) {
  const auto* values = lc::List(input);
  if (!values || (values->size() != 5 && values->size() != 6)) return;
  const size_t first = values->size() - 5;
  int64_t port = 0;
  if (first) {
    if (!lc::IsInt(&(*values)[0])) return;
    port = lc::Int(&(*values)[0]);
    if (port < 0 || port > 3) return;
  }
  const auto* mask = &(*values)[first];
  if (!lc::IsInt(mask)) return;
  std::array<int16_t, 4> axes{};
  for (size_t i = 0; i < axes.size(); ++i) {
    const auto* axis = &(*values)[first + i + 1];
    if (lc::IsInt(axis)) axes[i] = static_cast<int16_t>(std::clamp<int64_t>(lc::Int(axis), -32768, 32767));
  }
  session.SetInput(static_cast<unsigned>(port), static_cast<uint16_t>(lc::Int(mask)), axes);
}

class NullVideo final : public Video {
 public:
  bool Open() override { return false; }
  int PreferredContext() const override { return 0; }
  std::string_view PreferredRenderer() const override { return {}; }
  bool Bind(HardwareCallbacks&) override { return false; }
  bool Environment(unsigned, void*, int) override { return false; }
  void Reset(const HardwareCallbacks&) override {}
  void Clear() override {}
  void Close() override {}
  std::shared_ptr<Frame> Read(unsigned, unsigned, const HardwareCallbacks&) override { return {}; }
};

class ProbeAudio final : public Audio {
 public:
  bool Open(double) override { return true; }
  void Write(const int16_t* samples, size_t frames) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < frames * 2; ++i) {
      hash_ ^= static_cast<uint16_t>(samples[i]);
      hash_ *= 1099511628211ull;
    }
    frames_ += frames;
  }
  std::pair<uint64_t, uint64_t> Read() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {frames_, hash_};
  }
  void Flush() override {}
  int64_t dropped_frames() const override { return 0; }
  void Close() override {}

 private:
  std::mutex mutex_;
  uint64_t frames_ = 0, hash_ = 14695981039346656037ull;
};

class LinuxBackend final : public Backend {
 public:
  LinuxBackend(Surface& surface, const HostOptions& options) : surface_(surface), probe_(options.probe) {}

  void Handle(const MethodCall& call, std::unique_ptr<MethodResult> result) override {
    try {
      HandleCall(call.method_name(), call.arguments(), *result);
    } catch (const std::exception& error) {
      result->Error("retroCore", error.what());
    }
  }

  void Presented(bool) override {
    if (session_) session_->MarkPresented();
  }

  bool HasSession() const override { return session_ != nullptr; }

  void Shutdown() override {
    if (session_) session_->Stop();
    Release();
  }

 private:
  struct FrameSink {
    std::mutex mutex;
    Session* session = nullptr;
    int64_t texture = -1;
  };

  void HandleCall(std::string_view method, const Value* args, MethodResult& result) {
    if (method == "start") {
      Start(args, result);
      return;
    }
    if (method == "ps1LocalCheckpointReopen" || method == "ps1LocalCheckpointDiscard") {
      const auto source = lc::IsInt(args) ? lc::Int(args) : 0;
      if (!ordinary_recovery_ || source != ordinary_recovery_->recovery->source)
        throw std::runtime_error("The private PS1 recovery checkpoint is unavailable.");
      if (session_ && session_->ended()) Release();
      if (session_) throw std::runtime_error("An embedded game is already running.");
      if (method == "ps1LocalCheckpointDiscard") {
        ordinary_recovery_.reset();
        result.Success(Value(true));
      } else {
        StartSession(result, *ordinary_recovery_, Console::kPs1);
      }
      return;
    }
    if (probe_ && method == "probeReadRam") return ProbeReadRam(args, result);
    // The registered texture survives individual games. Commands must carry
    // the run owner as well, so a delayed callback cannot mutate a later game.
    const auto owned = lc::OwnedArguments(args, active_session_);
    if (!owned) {
      result.Success();
      return;
    }
    args = *owned;
    if (!session_) {
      result.Success();
    } else if (method == "status") {
      result.Success(EncodeStatus(session_->Status()));
    } else if (probe_ && method == "probeStats") {
      const auto status = session_->Status();
      const auto [frames, hash] = probe_audio_ ? probe_audio_->Read() : std::pair<uint64_t, uint64_t>{};
      result.Success(Value(Map{{Value("audioFrames"), Int(static_cast<int64_t>(frames))},
                               {Value("audioHash"), Value(std::to_string(hash))},
                               {Value("audioDrained"), Int(status.confirmed_audio_frame)},
                               {Value("corrections"), Int(static_cast<int64_t>(status.rollback_corrections))}}));
    } else if (method == "stop") {
      session_->Stop();
      result.Success();
    } else if (method == "release") {
      if (session_->ended()) Release();
      result.Success();
    } else if (method == "pause") {
      if (lc::IsBool(args)) session_->Pause(std::get<bool>(*args));
      result.Success();
    } else if (method == "changeDisc") {
      const auto index = lc::IsInt(args) ? lc::Int(args) : -1;
      result.Success(Value(index >= 0 && index <= INT32_MAX && session_->ChangeDisc(static_cast<unsigned>(index))));
    } else if (method == "input") {
      SetInput(*session_, args);
      result.Success();
    } else if (method == "netplayStart") {
      result.Success(Value(session_->StartNetplay()));
    } else if (method == "ps1CheckpointHold") {
      const auto frame = session_->HoldNetplay();
      result.Success(frame ? Int(*frame) : Value());
    } else if (method == "ps1ResumeLocal") {
      result.Success(Value(session_->ResumeLocalNetplay()));
    } else if (method == "ps1CheckpointHistory") {
      const auto history = session_->MatchHistory(lc::ReadMatchFrame(args));
      if (!history) throw std::runtime_error("The requested input history is unavailable.");
      result.Success(EncodeMatchHistory(*history));
    } else if (method == "ps1CheckpointReplay") {
      const auto first = lc::ReadMatchFrame(lc::Field(args, "frame"));
      const auto ticket = session_->RequestMatchReplay(first, lc::ReadMatchReplay(args));
      if (!ticket) throw std::runtime_error("The guest cannot replay from that frame.");
      result.Success(Int(*ticket));
    } else if (method == "netplaySync") {
      const auto exchange = lc::ReadMatchExchange(args);
      const auto tick = session_->SyncNetplay(exchange.remote, exchange.acknowledged);
      if (!tick) throw std::runtime_error("The PS1 netplay input exchange was rejected.");
      result.Success(EncodeMatchTick(*tick));
    } else if (method == "ps1LocalCheckpointDetach" || method == "ps1LocalCheckpointAccept") {
      const auto source = lc::ReadCheckpointSource(args, texture_);
      if (!source) throw std::runtime_error("The PS1 recovery session changed.");
      if (method == "ps1LocalCheckpointDetach") {
        if (ordinary_recovery_) throw std::runtime_error("PS1 recovery is already pending.");
        auto recovery = session_->DetachOrdinaryRecovery(*source);
        if (!recovery) throw std::runtime_error("The original PS1 game is not parked.");
        ordinary_recovery_ = std::move(recovery);
        Release();
        result.Success(Int(*source));
      } else {
        if (!ordinary_recovery_ || *source != ordinary_recovery_->recovery->source ||
            !session_->AcceptOrdinaryRecovery(*source))
          throw std::runtime_error("The original PS1 game is not ready to resume.");
        ordinary_recovery_.reset();
        result.Success(Value(true));
      }
    } else if (method == "ps1LocalCheckpointExport" || method == "ps1LocalCheckpointPark" ||
               method == "ps1LocalCheckpointResume") {
      const auto source = lc::ReadCheckpointSource(args, texture_);
      if (!source) {
        result.Success();
        return;
      }
      const auto ticket = method == "ps1LocalCheckpointResume"
                              ? session_->RequestOrdinaryResume(*source)
                              : session_->RequestOrdinaryCheckpoint(*source, lc::ReadCheckpointDigest(args),
                                                                    method == "ps1LocalCheckpointPark");
      result.Success(ticket ? Int(*ticket) : Value());
    } else if (method == "ps1CheckpointExport" || method == "ps1CheckpointImport") {
      const auto digest = lc::ReadCheckpointDigest(args);
      std::optional<int64_t> ticket;
      if (method == "ps1CheckpointImport") {
        auto snapshot = lc::ReadMatchCheckpoint(args);
        ticket = session_->RequestMatchImport(digest, snapshot.frame, std::move(snapshot.state));
      } else {
        ticket = session_->RequestMatchExport(digest);
      }
      if (!ticket) throw std::runtime_error("This core cannot run that checkpoint operation now.");
      result.Success(Int(*ticket));
    } else if (method == "ps1CheckpointResult" || method == "ps1CheckpointCancel") {
      const auto ticket = lc::IsInt(args) ? lc::Int(args) : 0;
      if (method == "ps1CheckpointCancel") {
        session_->CancelCheckpoint(ticket);
        result.Success();
        return;
      }
      auto reply = session_->TakeCheckpoint(ticket);
      if (const auto* completed = std::get_if<bool>(&reply)) {
        result.Success(Value(*completed));
      } else if (auto* snapshot = std::get_if<ps1_checkpoint::Snapshot>(&reply)) {
        result.Success(Value(Map{{Value("frame"), Int(snapshot->frame)},
                                 {Value("state"), Value(std::move(snapshot->state))}}));
      } else {
        result.Success();
      }
    } else if (method == "ps2LanReset") {
      const bool texture_matches = lc::IsInt(args) && lc::Int(args) == texture_;
      result.Success(Value(texture_matches && session_->ClearLanFrames()));
    } else if (method == "ps2LanExchange") {
      const auto input = lc::ReadLanFrames(args, texture_);
      if (!input) {
        result.Success();
        return;
      }
      const auto output = session_->ExchangeLan(*input);
      if (!output) {
        result.Success();
        return;
      }
      List frames;
      for (const auto& frame : *output) frames.emplace_back(frame);
      result.Success(Value(std::move(frames)));
    } else if (method == "option") {
      const auto* key = StringField(args, "key");
      const auto* value = StringField(args, "value");
      result.Success(Value(key && value && session_->SetOption(key, value)));
    } else if (method == "tuningOptions") {
      const auto expected = ReadOptions(lc::Field(args, "expected"));
      const auto values = ReadOptions(lc::Field(args, "values"));
      result.Success(Value(expected && values && session_->SetTuningOptions(*expected, *values)));
    } else {
      result.NotImplemented();
    }
  }

  void Start(const Value* args, MethodResult& result) {
    if (session_ && session_->ended()) Release();
    if (session_) {
      result.Error("busy", "An embedded game is already running");
      return;
    }
    const auto* core = StringField(args, "core");
    const auto* disc = StringField(args, "disc");
    const auto* system = StringField(args, "system");
    const auto* saves = StringField(args, "saves");
    const auto* resolution = StringField(args, "internalResolution");
    if (!core || !disc || !system || !saves) {
      result.Error("argument", "The core, disc and storage paths are required");
      return;
    }
    const auto console = ConsoleForCore(core);
    if (console == Console::kUnknown) {
      result.Error("unsupportedCore", "This emulator core is unavailable on Linux");
      return;
    }
    try {
      auto match = lc::ReadPs1Match(args);
      std::optional<std::array<uint8_t, 6>> lan_mac;
      if (const auto* mac = lc::Field(args, "ps2LanMac")) {
        const auto* bytes = lc::Bytes(mac);
        if (console != Console::kPs2 || !bytes || bytes->size() != 6)
          throw std::runtime_error("Expected a PS2 game and six-byte private network address");
        lan_mac.emplace();
        std::copy_n(bytes->begin(), 6, lan_mac->begin());
      }
      std::optional<InitialDisc> initial_disc;
      const auto* initial_index = lc::Field(args, "initialDiscIndex");
      const auto* initial_path = StringField(args, "initialDiscPath");
      if (initial_index || lc::Field(args, "initialDiscPath")) {
        if (!lc::IsInt(initial_index) || !initial_path || !*initial_path) throw std::runtime_error("Invalid initial disc");
        const auto index = lc::Int(initial_index);
        if (index < 0 || index >= 128) throw std::runtime_error("Invalid initial disc");
        initial_disc = InitialDisc{static_cast<unsigned>(index), initial_path};
      }
      if (probe_) {
        if (const auto* trace = lc::List(lc::Field(args, "probeInputTrace"))) {
          if (!match) throw std::runtime_error("A probe input trace requires a PS1 match.");
          for (const auto& entry : *trace) {
            if (!lc::IsInt(&entry) || lc::Int(&entry) < 0 || lc::Int(&entry) > UINT16_MAX)
              throw std::runtime_error("Invalid probe input trace.");
            match->input_trace.push_back({static_cast<uint16_t>(lc::Int(&entry)), {}});
          }
        }
      }
      SessionConfig config{core, disc, system, saves, resolution ? resolution : "",
                           ReadOptions(lc::Field(args, "graphics")).value_or(Options{}), std::move(initial_disc),
                           lan_mac};
      config.ps1_match = std::move(match);
      config.ps3_online = lc::ReadPs3Online(args, console);
      StartSession(result, std::move(config), console);
    } catch (const std::exception& error) {
      result.Error("start", error.what());
    }
  }

  void StartSession(MethodResult& result, SessionConfig config, Console console) {
    try {
      if (session_serial_ == std::numeric_limits<int64_t>::max())
        throw std::runtime_error("The native session counter is exhausted");
      const auto texture = surface_.Register();
      if (texture < 0) throw std::runtime_error("Could not register the game surface");
      texture_ = texture;
      const bool probe_match = probe_ && config.ps1_match.has_value();
      std::unique_ptr<Audio> audio;
      if (probe_match) {
        auto sink = std::make_unique<ProbeAudio>();
        probe_audio_ = sink.get();
        audio = std::move(sink);
      } else {
        probe_audio_ = nullptr;
        audio = CreateLinuxAudio();
      }
      std::unique_ptr<Video> video;
      if (probe_match) video = std::make_unique<NullVideo>();
#if defined(ANYPS5_RETRO_VULKAN)
      else if (console == Console::kPs3) video = CreateVulkanVideo();
#else
      else if (console == Console::kPs3) throw std::runtime_error("This build has no PS3 renderer.");
#endif
      else video = CreateLinuxVideo();
      auto sink = std::make_shared<FrameSink>();
      sink->texture = texture;
      session_ = std::make_unique<Session>(std::move(config), std::move(video), std::move(audio),
                                           [this, sink] { PublishFrame(*sink); });
      {
        std::lock_guard<std::mutex> lock(sink->mutex);
        sink->session = session_.get();
      }
      sink_ = sink;
      session_->Start();
      active_session_ = ++session_serial_;
      result.Success(Value(Map{{Value("texture"), Int(texture_)}, {Value("session"), Int(active_session_)}}));
    } catch (...) {
      if (session_) session_->Stop();
      Release();
      throw;
    }
  }

  void PublishFrame(FrameSink& sink) {
    std::lock_guard<std::mutex> lock(sink.mutex);
    if (!sink.session) return;
    const auto frame = sink.session->LatestFrame();
    if (frame) surface_.Publish(sink.texture, frame->pixels.data(), frame->width, frame->height, true);
  }

  void Release() {
    if (session_) session_->Join();
    if (sink_) {
      std::lock_guard<std::mutex> lock(sink_->mutex);
      sink_->session = nullptr;
    }
    sink_.reset();
    surface_.Release(texture_);
    session_.reset();
    probe_audio_ = nullptr;
    active_session_ = 0;
  }

  void ProbeReadRam(const Value* args, MethodResult& result) {
    const auto* core = StringField(args, "core");
    const auto* offset = lc::Field(args, "offset");
    const auto* count = lc::Field(args, "count");
    constexpr int64_t kRam = 2 * 1024 * 1024;
    if (!core || !lc::IsInt(offset) || !lc::IsInt(count) || lc::Int(count) > 256 || lc::Int(offset) < 0 ||
        lc::Int(offset) > kRam || lc::Int(count) < 0 || lc::Int(count) > kRam - lc::Int(offset))
      throw std::runtime_error("Invalid probe RAM read.");
    void* library = dlopen(core, RTLD_NOW | RTLD_NOLOAD);
    if (!library) throw std::runtime_error("The probe core is not loaded.");
    const auto get = reinterpret_cast<void* (*)(unsigned)>(dlsym(library, "retro_get_memory_data"));
    const auto* ram = get ? static_cast<const uint8_t*>(get(2)) : nullptr;
    retro_ipc::Bytes bytes;
    if (ram) bytes.assign(ram + lc::Int(offset), ram + lc::Int(offset) + lc::Int(count));
    dlclose(library);
    if (!ram) throw std::runtime_error("PS1 main RAM is unavailable.");
    result.Success(Value(std::move(bytes)));
  }

  Surface& surface_;
  const bool probe_;
  std::unique_ptr<Session> session_;
  std::shared_ptr<FrameSink> sink_;
  std::optional<SessionConfig> ordinary_recovery_;
  ProbeAudio* probe_audio_ = nullptr;
  int64_t texture_ = -1;
  int64_t session_serial_ = 0, active_session_ = 0;
};
}  // namespace

std::unique_ptr<Backend> CreateBackend(Surface& surface, const HostOptions& options) {
  return std::make_unique<LinuxBackend>(surface, options);
}

}  // namespace anyps5::retro_host
