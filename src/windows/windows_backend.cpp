#include "host_server.h"
#include "ps1_netplay_timeline.h"
#include "ps1_checkpoint.h"
#include "ps1_match.h"
#include "ps2_virtual_lan.h"
#include "ps3_network_policy.h"
#include "ps3_package_batch.h"
#include "retro_vulkan_video.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Value = anyps5::retro_ipc::Value;
using Map = anyps5::retro_ipc::Map;
using List = anyps5::retro_ipc::List;

namespace {

using anyps5::retro::Ps3NetworkPolicy;

// Layouts and command values from the libretro v1 C ABI.
struct GameInfo {
  const char* path;
  const void* data;
  size_t size;
  const char* meta;
};
struct DiskControlCallbacks {
  bool (*set_eject_state)(bool);
  bool (*get_eject_state)();
  unsigned (*get_image_index)();
  bool (*set_image_index)(unsigned);
  unsigned (*get_num_images)();
  bool (*replace_image_index)(unsigned, const GameInfo*);
  bool (*add_image_index)();
};
struct DiskControlExtCallbacks {
  DiskControlCallbacks basic;
  bool (*set_initial_image)(unsigned, const char*);
  bool (*get_image_path)(unsigned, char*, std::size_t);
  bool (*get_image_label)(unsigned, char*, std::size_t);
};
struct Variable {
  const char* key;
  const char* value;
};
struct ExtendedMessage {
  const char* text;
  unsigned duration, priority;
  int level, target, type;
  int8_t progress;
};
struct CoreOptionValue {
  const char* value;
  const char* label;
};
struct CoreOptionDefinition {
  const char* key;
  const char* title;
  const char* category_title;
  const char* description;
  const char* category_description;
  const char* category;
  CoreOptionValue values[128];
  const char* default_value;
};
struct CoreOptionsV2 {
  const void* categories;
  const CoreOptionDefinition* definitions;
};
struct CoreOptionsV2Intl {
  const CoreOptionsV2* english;
  const CoreOptionsV2* localized;
};
struct Geometry {
  unsigned width, height, max_width, max_height;
  float aspect;
};
struct AvInfo {
  Geometry geometry;
  double fps, sample_rate;
};
struct HardwareCallbacks {
  int context_type;
  void (*reset)();
  uintptr_t (*framebuffer)();
  void (*(*proc_address)(const char*))();
  bool depth, stencil, bottom_left_origin;
  unsigned version_major, version_minor;
  bool cache_context;
  void (*destroy)();
  bool debug_context;
};
struct D3D11Interface {
  int type = 3;
  unsigned version = 1;
  void* handle = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL feature_level{};
  decltype(&D3DCompile) compile = D3DCompile;
};
constexpr int kD3D11 = 7;
constexpr char kPs3Resolution[] = "rpcs3_resolution_scale";
constexpr unsigned kMaxFrameSize = 4096;
constexpr size_t kMaxPs1Checkpoint = anyps5::retro::ps1_checkpoint::kMaximumBytes;
constexpr size_t kMaxPs1AudioSamplesPerFrame = 8192;
constexpr size_t kPs1CheckpointPrefix = anyps5::retro::ps1_checkpoint::kPrefixBytes;
constexpr uint32_t kPs1CatchupHistory = 1024;
constexpr uint32_t kPs1CatchupBatch = 120;
using Ps1DiscHash = std::array<uint8_t, 32>;
using CheckpointBytes = std::vector<uint8_t>;
using Ps1Ports = std::array<ps1_netplay::PadInput, 2>;
struct InitialDisc {
  unsigned index;
  std::string path;
};

struct DiscStatus {
  std::vector<std::string> labels;
  std::optional<unsigned> inserted;
  std::string inserted_path;
  bool busy = false, failed = false;
};

enum EnvironmentCommand {
  kCanDupe = 3, kShutdown = 7, kSystemDirectory = 9, kPixelFormat = 10,
  kInputDescriptors = 11, kSetDiskControl = 13, kGetVariable = 15, kSetVariables = 16,
  kVariableUpdate = 17, kSupportNoGame = 18, kCorePath = 19,
  kContentDirectory = 30, kSaveDirectory = 31, kSetAv = 32,
  kSetGeometry = 37, kAudioVideoEnable = 47, kFastForward = 49,
  kCoreOptionsVersion = 52, kSetHardware = 14, kHardwareInterface = 41 | 0x10000,
  kHardwareNegotiation = 43 | 0x10000,
  kPreferredHardware = 56, kDiskControlVersion = 57, kSetDiskControlExt = 58, kSetCoreOptionsV2 = 67, kSetCoreOptionsV2Intl = 68,
  kMessageVersion = 59, kExtendedMessage = 60,
  kFrontendCompileProgress = 0x20000 | 0x4150,
  kGetInputBitmasks = 51 | 0x10000,
  kSavestateContext = 72 | 0x10000,
};

using anyps5::retro::kPs1NetplayOptions;
using anyps5::retro::kPs1DualShockDevice;
using anyps5::retro::Ps1MatchStorage;
using anyps5::retro::NetplayFingerprint;
enum class Ps1MatchCpu { kCachedInterpreter, kRecompiler };

std::optional<int64_t> IntegerValue(const Value& value) {
  if (const auto* narrow = std::get_if<int32_t>(&value)) return *narrow;
  if (const auto* wide = std::get_if<int64_t>(&value)) return *wide;
  return std::nullopt;
}

Value FrameValue(const std::optional<uint32_t>& frame) {
  return frame ? Value(static_cast<int64_t>(*frame)) : Value();
}



struct Frame {
  std::vector<uint8_t> pixels;
  size_t width = 0;
  size_t height = 0;

  bool HasVisiblePixels() const {
    // Sample the frame across its full area. Ignore alpha and near-black
    // renderer clears. This only runs until the first visible boot frame.
    const auto columns = std::min<size_t>(width, 64);
    const auto rows = std::min<size_t>(height, 36);
    for (size_t row = 0; row < rows; ++row) {
      const auto y = (2 * row + 1) * height / (2 * rows);
      for (size_t column = 0; column < columns; ++column) {
        const auto x = (2 * column + 1) * width / (2 * columns);
        const auto* rgb = pixels.data() + 4 * (y * width + x);
        if (std::max({rgb[0], rgb[1], rgb[2]}) > 12) return true;
      }
    }
    return false;
  }
};

class HardwareVideo {
 public:
  bool Open() {
    // LRPS2 selects Shader Model 5.1 at feature level 11.1, which its D3D11 renderer cannot use.
    constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &device_, &render_interface.feature_level,
                                 &context_))) return false;
    render_interface.handle = this;
    render_interface.device = device_.Get();
    render_interface.context = context_.Get();
    return true;
  }

  std::shared_ptr<Frame> Read(unsigned width, unsigned height) {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    context_->PSGetShaderResources(0, 1, &view);
    if (!view) return nullptr;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    view->GetResource(&resource);
    if (FAILED(resource.As(&source))) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const auto format = desc.Format;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      format == DXGI_FORMAT_B8G8R8X8_UNORM;
    if ((!bgra && format != DXGI_FORMAT_R8G8B8A8_UNORM) || desc.SampleDesc.Count != 1 ||
        width > desc.Width || height > desc.Height) return nullptr;
    if (!staging_ || width != width_ || height != height_ || format != format_) {
      D3D11_TEXTURE2D_DESC staging{width, height, 1, 1, format, {1, 0},
                                  D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
      if (FAILED(device_->CreateTexture2D(&staging, nullptr, staging_.ReleaseAndGetAddressOf())))
        return nullptr;
      width_ = width;
      height_ = height;
      format_ = format;
    }
    auto frame = std::make_shared<Frame>();
    frame->width = width;
    frame->height = height;
    frame->pixels.resize(static_cast<size_t>(width) * height * 4);
    const D3D11_BOX box{0, 0, 0, width, height, 1};
    context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, source.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return nullptr;
    const auto* input = static_cast<const uint8_t*>(mapped.pData);
    for (unsigned row = 0; row < height; ++row) {
      const auto* source_row = input + static_cast<size_t>(row) * mapped.RowPitch;
      auto* output = frame->pixels.data() + static_cast<size_t>(row) * width * 4;
      for (unsigned column = 0; column < width; ++column, output += 4, source_row += 4) {
        output[0] = source_row[bgra ? 2 : 0];
        output[1] = source_row[1];
        output[2] = source_row[bgra ? 0 : 2];
        output[3] = 255;
      }
    }
    context_->Unmap(staging_.Get(), 0);
    return frame;
  }

  D3D11Interface render_interface;

  void Clear() {
    if (!context_) return;
    staging_.Reset();
    context_->ClearState();
    context_->Flush();
  }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  unsigned width_ = 0, height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

class StereoOutput {
 public:
  ~StereoOutput() { Close(); }

  bool Open(double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
    if (!std::isfinite(rate) || rate < 8000 || rate > 192000) return false;
    const DWORD sample_rate = static_cast<DWORD>(std::lround(rate));
    WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, sample_rate, sample_rate * 4, 4, 16, 0};
    return waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
  }

  void Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_) waveOutReset(device_);
    pending_count_ = 0;
  }

  void Write(const int16_t* stereo, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) return;
    while (frames > 0) {
      const size_t count = std::min(frames, pending_.size() / 2 - pending_count_);
      std::copy_n(stereo, count * 2, pending_.data() + pending_count_ * 2);
      pending_count_ += count;
      stereo += count * 2;
      frames -= count;
      if (pending_count_ == pending_.size() / 2) Submit();
    }
  }

  int64_t dropped_frames() const { return dropped_frames_; }

  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    CloseLocked();
  }

 private:
  struct Slot {
    std::array<int16_t, 1024> samples{};
    WAVEHDR header{};
  };

  void Submit() {
    pending_count_ = 0;
    for (auto& slot : slots_) {
      if ((slot.header.dwFlags & WHDR_INQUEUE) != 0) continue;
      if ((slot.header.dwFlags & WHDR_PREPARED) != 0) {
        if (waveOutUnprepareHeader(device_, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) continue;
      }
      slot.samples = pending_;
      slot.header = {};
      slot.header.lpData = reinterpret_cast<char*>(slot.samples.data());
      slot.header.dwBufferLength = static_cast<DWORD>(sizeof(slot.samples));
      if (waveOutPrepareHeader(device_, &slot.header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR &&
          waveOutWrite(device_, &slot.header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
        return;
      }
      break;
    }
    dropped_frames_ += pending_.size() / 2;
  }

  void CloseLocked() {
    if (device_) {
      waveOutReset(device_);
      for (auto& slot : slots_) {
        if ((slot.header.dwFlags & WHDR_PREPARED) != 0) {
          waveOutUnprepareHeader(device_, &slot.header, sizeof(WAVEHDR));
        }
        slot.header = {};
      }
      waveOutClose(device_);
      device_ = nullptr;
    }
    pending_count_ = 0;
  }

  std::mutex mutex_;
  HWAVEOUT device_ = nullptr;
  std::array<Slot, 8> slots_{};
  std::array<int16_t, 1024> pending_{};
  size_t pending_count_ = 0;
  std::atomic<int64_t> dropped_frames_{0};
};

class ConfirmedPcmPlayout {
 public:
  using Emit = std::function<void(uint32_t, const int16_t*, size_t)>;

  ConfirmedPcmPlayout(double rate, Emit emit)
      : rate_(rate), emit_(std::move(emit)), worker_([this] { Run(); }) {}

  ~ConfirmedPcmPlayout() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
  }

  void Submit(uint32_t frame, const int16_t* pcm, size_t samples) {
    Block block{frame, std::vector<int16_t>(pcm, pcm + samples)};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (failure_) std::rethrow_exception(failure_);
      if (blocks_.size() == 64) throw std::runtime_error("The confirmed PS1 audio queue is full.");
      blocks_.push_back(std::move(block));
      peak_ = std::max(peak_.load(), blocks_.size());
    }
    ready_.notify_one();
  }

  size_t peak() const { return peak_; }
  size_t underruns() const { return underruns_; }

 private:
  struct Block {
    uint32_t frame;
    std::vector<int16_t> pcm;
  };

  void Run() {
    using Clock = std::chrono::steady_clock;
    constexpr auto reserve = std::chrono::milliseconds(120);
    Clock::time_point next{};
    bool waited_for_pcm = false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
      if (blocks_.empty()) {
        waited_for_pcm = true;
        ready_.wait(lock, [this] { return stopping_ || !blocks_.empty(); });
        continue;
      }
      const auto samples = blocks_.front().pcm.size() / 2;
      if (samples) {
        const auto now = Clock::now();
        const auto duration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(samples) / rate_));
        if (next == Clock::time_point{}) {
          next = now + reserve;
        } else if (waited_for_pcm && now > next + std::chrono::milliseconds(2)) {
          ++underruns_;
          // Rebuild the reserve after a gap so the next jitter event does not
          // immediately empty the queue again.
          next = now + reserve;
        }
        waited_for_pcm = false;
        if (ready_.wait_until(lock, next, [this] { return stopping_; })) break;
        const auto awake = Clock::now();
        if (awake > next + duration) next = awake;
        next += duration;
      }
      auto block = std::move(blocks_.front());
      blocks_.pop_front();
      lock.unlock();
      try {
        emit_(block.frame, block.pcm.data(), samples);
      } catch (...) {
        lock.lock();
        failure_ = std::current_exception();
        return;
      }
      lock.lock();
    }
  }

  const double rate_;
  const Emit emit_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Block> blocks_;
  std::exception_ptr failure_;
  bool stopping_ = false;
  std::atomic<size_t> peak_{0};
  std::atomic<size_t> underruns_{0};
  std::thread worker_;
};

template <typename T>
T Symbol(HMODULE dll, const char* name, bool required = true) {
  auto address = GetProcAddress(dll, name);
  if (!address && required) throw std::runtime_error(std::string("Core is missing ") + name);
  T function;
  static_assert(sizeof(function) == sizeof(address));
  std::memcpy(&function, &address, sizeof(function));
  return function;
}

void SelectPs3NetworkPolicy(HMODULE dll, const std::optional<Ps3NetworkPolicy>& online,
                            bool require_export) {
  using namespace anyps5::retro;
  if (online) {
    ApplyPs3OnlinePolicy(Symbol<Ps3VerifiedOnlinePolicy>(dll, kPs3VerifiedOnlinePolicyExport, false), *online);
  } else {
    ApplyPs3OfflinePolicy(Symbol<Ps3OfflinePolicy>(dll, kPs3OfflinePolicyExport, false), require_export);
  }
}

const std::string* StringField(const Map& map, const char* name) {
  const auto found = map.find(Value(name));
  return found == map.end() ? nullptr : std::get_if<std::string>(&found->second);
}

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

bool SupportedBootValue(const std::string& key, std::string_view value, bool hardware_available) {
  if (key == "swanstation_GPU_Renderer") {
    return value == "Software" || (hardware_available && value == "D3D11");
  }
  if (key == "pcsx2_renderer") {
    return value == "Software (SW)" || value == "Software (HW)" ||
           (hardware_available && value == "D3D11");
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
  temporary += L".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  file.close();
  if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("Could not write the PlayStation memory card");
  }
}
}  // namespace

struct Ps1OrdinaryRecovery {
  int64_t source;
  std::string core, disc, system, saves;
  std::map<std::string, std::string> options;
  Ps1DiscHash digest;
  CheckpointBytes state;
  CheckpointBytes continuation;
};

class RetroCoreSession {
 public:
  enum class CallbackSlot : size_t { kPrimary, kPresentation };
  using AudioSink = std::function<void(const int16_t*, size_t)>;
  struct PresentationState {
    uint32_t frame;
    std::string disc;
    CheckpointBytes bytes;
  };
  int64_t rollback_audio_drained() const { return rollback_audio_drained_; }
  int64_t trace_max_work_us() const { return trace_max_work_us_; }
  int64_t trace_work_overruns() const { return trace_work_overruns_; }
  enum TraceStage : size_t { kAudio, kRestore, kSave, kStep, kFingerprint, kOther, kTraceStageCount };
  struct TraceBreakdown {
    std::array<int64_t, kTraceStageCount> worst_us{};
    std::array<uint32_t, kTraceStageCount> dominant_overruns{};
    int64_t total_us = 0;
    uint32_t frame = 0;
  };
  TraceBreakdown trace_breakdown() const {
    std::lock_guard<std::mutex> lock(trace_breakdown_mutex_);
    return trace_breakdown_;
  }
  struct TraceStep {
    uint32_t frame = 0;
    int64_t elapsed_us = 0;
    int64_t video_us = 0;
    std::optional<uint64_t> cycles;
    uint64_t total_cycles = 0;
    uint32_t cycle_samples = 0;
  };
  TraceStep trace_step() const {
    std::lock_guard<std::mutex> lock(trace_breakdown_mutex_);
    return trace_step_;
  }
  std::array<int64_t, 4> trace_pacing() const {
    return {trace_blocked_polls_, trace_blocked_us_, trace_late_wakes_, trace_peak_wake_us_};
  }
  uint32_t presentation_steps() const { return presentation_steps_; }
  uint32_t presentation_frames() const { return presentation_frames_; }
  uint32_t peak_prediction_depth() const { return peak_prediction_depth_; }
  std::pair<size_t, size_t> trace_audio_pacing() const { return {audio_underruns_, audio_queue_peak_}; }

  RetroCoreSession(anyps5::retro_host::Surface* registrar, std::string core,
                   std::string disc, std::string system, std::string saves,
                   std::string initial_resolution = {},
                   std::map<std::string, std::string> initial_graphics = {},
                   std::optional<unsigned> netplay_seat = std::nullopt,
                   const std::string& netplay_bios = {}, bool local_staging = false,
                   std::optional<Ps2VirtualLan::Mac> lan_mac = std::nullopt,
                   std::optional<Ps3NetworkPolicy> online = std::nullopt,
                   std::vector<std::string> packages = {},
                   Ps1MatchCpu match_cpu = Ps1MatchCpu::kCachedInterpreter,
                   std::shared_ptr<const Ps1OrdinaryRecovery> returning = {},
                   bool rollback = false, AudioSink audio_sink = {},
                   std::vector<ps1_netplay::PadInput> input_trace = {},
                   CallbackSlot callback_slot = CallbackSlot::kPrimary,
                   std::optional<InitialDisc> boot_disc = std::nullopt, bool netplay_dualshock = false,
                   bool detailed_step_trace = false)
      : registrar_(registrar), core_(std::move(core)), disc_(std::move(disc)),
        netplay_(netplay_seat ? std::make_unique<ps1_netplay::Timeline>(
                                   *netplay_seat, rollback ? ps1_netplay::Timeline::kStandardPrediction : 0)
                              : nullptr),
        rollback_(rollback),
        netplay_host_(netplay_seat == 0u),
        audio_sink_(std::move(audio_sink)),
        input_trace_(std::move(input_trace)),
        detailed_step_trace_(detailed_step_trace),
        callback_slot_(callback_slot),
        match_storage_(netplay_ ? std::make_unique<Ps1MatchStorage>(netplay_bios) : nullptr),
        lan_(lan_mac ? std::make_unique<Ps2VirtualLan>(*lan_mac) : nullptr),
        system_(match_storage_ ? match_storage_->system() : std::move(system)),
        saves_(match_storage_ ? match_storage_->saves() : std::move(saves)),
        content_(match_storage_ ? match_storage_->content()
                                : std::filesystem::u8path(disc_).parent_path().u8string()),
        initial_resolution_(std::move(initial_resolution)),
        initial_graphics_(std::move(initial_graphics)),
        online_(std::move(online)),
        packages_(std::move(packages)),
        ps1_(std::filesystem::u8path(core_).filename() == L"swanstation_libretro.dll"),
        boot_disc_(std::move(boot_disc)),
        ps3_(std::filesystem::u8path(core_).filename() == L"rpcs3_libretro.dll"),
        local_staging_(local_staging),
        match_cpu_(match_cpu),
        netplay_dualshock_(netplay_dualshock),
        returning_(std::move(returning)),
        recovery_accepted_(!returning_) {
    if (local_staging_ && netplay_seat != 0u)
      throw std::invalid_argument("Only the PS1 host can start in local play.");
    if (rollback_ && (!netplay_ || local_staging_))
      throw std::invalid_argument("PS1 rollback requires an isolated match.");
    if (!input_trace_.empty() && !rollback_ && !netplay_dualshock_)
      throw std::invalid_argument("PS1 input traces require a headless rollback or DualShock match worker.");
    if (callback_slot_ == CallbackSlot::kPresentation && (!rollback_ || installing_packages()))
      throw std::invalid_argument("The presentation callback slot requires an isolated PS1 rollback worker.");
    if (returning_) paused_ = true;
  }

  ~RetroCoreSession() { Stop(); Join(); }

  bool AttachPresentationCore(std::unique_ptr<RetroCoreSession> core) {
    if (!core || !rollback_ || callback_slot_ != CallbackSlot::kPrimary || worker_.joinable() ||
        presentation_core_ || core->callback_slot_ != CallbackSlot::kPresentation ||
        core->worker_.joinable() || core->disc_ != disc_ || core->match_cpu_ != match_cpu_) return false;
    if (!netplay_->SetPredictionWindow(ps1_netplay::Timeline::kMaxPrediction)) return false;
    core->presentation_target_ = this;
    presentation_core_ = std::move(core);
    return true;
  }

  bool Start() {
    if (installing_packages()) {
      worker_ = std::thread([this] { RunPackageInstall(); });
      return true;
    }
    if (!presentation_target_) {
      texture_id_ = registrar_->Register();
      if (texture_id_ < 0) return false;
    }
    if (presentation_core_ && !presentation_core_->Start()) return false;
    worker_ = std::thread([this] { Run(); });
    return true;
  }

  int64_t texture_id() const { return texture_id_; }
  bool ended() const { return ended_; }
  bool installing_packages() const { return !packages_.empty(); }

  Map PackageStatus() {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    const bool ended = ended_.load();
    return {
        {Value("ended"), Value(ended)},
        {Value("error"), Value(ended ? error_ : std::string())},
        {Value("completed"), Value(package_completed_.load())},
        {Value("rejected"), Value(package_rejected_.load())},
    };
  }
  void Stop() {
    stop_ = true;
    if (presentation_core_) presentation_core_->Stop();
    if (callback_slot_ == CallbackSlot::kPresentation) {
      std::lock_guard<std::mutex> lock(netplay_mutex_);
      presentation_ready_.notify_one();
    }
    if (lan_) lan_->Close();
  }
  void Join() {
    if (worker_.joinable()) worker_.join();
    if (presentation_core_) presentation_core_->Join();
  }

  std::optional<List> ExchangeLan(const List& input) {
    if (!lan_ || !lan_ready_ || stop_ || ended_ || input.size() > Ps2VirtualLan::kBatchSize)
      return std::nullopt;
    for (const auto& item : input) {
      const auto* frame = std::get_if<CheckpointBytes>(&item);
      if (!frame || !lan_->Accepts(frame->data(), frame->size())) return std::nullopt;
    }
    for (const auto& item : input) {
      const auto& frame = std::get<CheckpointBytes>(item);
      lan_->Inject(frame.data(), frame.size());
    }
    List output;
    std::array<uint8_t, Ps2VirtualLan::kMaxFrame> frame{};
    while (output.size() < Ps2VirtualLan::kBatchSize) {
      const auto size = lan_->Drain(frame.data(), frame.size());
      if (!size) break;
      output.emplace_back(CheckpointBytes(frame.data(), frame.data() + size));
    }
    return output;
  }

  bool ClearLanFrames() {
    if (!lan_ || !lan_ready_ || stop_ || ended_) return false;
    lan_->ClearFrames();
    return !stop_ && !ended_;
  }

  void Pause(bool pause) {
    if (netplay_ || lan_) {
      if (pause) ClearInput();
      return;
    }
    if (paused_.exchange(pause) == pause) return;
    if (pause) {
      ClearInput();
      audio_.Flush();
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      last_frame_at_ = {};
    }
  }

  bool ChangeDisc(unsigned index) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (netplay_ || !recovery_accepted_ || ordinary_settings_frozen_ ||
        graphics_queued_ || graphics_applying_ || stop_ || ended_ || !discs_ || discs_->busy ||
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

  // The PS1 ports with a controller in, from port 1. Port 2 stays connected until the launcher
  // says otherwise, as it did before the launcher could say it.
  bool ConnectPorts(unsigned count) {
    if (!ps1_ || netplay_ || installing_packages() || stop_ || ended_ || count < 1 || count > kLocalPorts)
      return false;
    if (connected_ports_.exchange(count) != count) ports_changed_ = true;
    return true;
  }

  unsigned SecondPortDevice() const { return connected_ports_ >= 2 ? kPs1DualShockDevice : kNoDevice; }

  void SetInput(unsigned port, uint16_t buttons, const std::array<int16_t, 4>& axes) {
    if (!recovery_accepted_ || port >= kLocalPorts) return;
    std::lock_guard<std::mutex> lock(input_mutex_);
    buttons_[port] = buttons;
    axes_[port] = axes;
  }

  bool StartNetplay() {
    if (!netplay_ || !netplay_ready_ || stop_ || ended_) return false;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (checkpoint_ || presentation_only_ || confirmed_return_requested_ || (StagingLocal() && !netplay_held_))
      return false;
    if (netplay_held_) {
      netplay_->Reset();
      checkpoint_history_.clear();
      netplay_fingerprints_.clear();
      netplay_pending_fingerprint_.reset();
      netplay_held_ = false;
      netplay_epoch_reset_ = true;
    }
    netplay_->Start();
    netplay_started_ = true;
    presentation_ready_.notify_one();
    return true;
  }

  std::future<std::optional<PresentationState>> CapturePresentation() {
    std::lock_guard<std::mutex> lock(netplay_mutex_);
    if (!rollback_ || callback_slot_ != CallbackSlot::kPrimary || !netplay_started_ || stop_ || ended_ ||
        presentation_busy_) return {};
    presentation_busy_ = true;
    return presentation_capture_.emplace().get_future();
  }

  std::future<bool> RenderPresentation(std::optional<PresentationState> snapshot, std::vector<Ps1Ports> inputs) {
    if (inputs.empty() || inputs.size() > 2 ||
        (snapshot && (snapshot->disc != disc_ || snapshot->bytes.empty() ||
                      snapshot->bytes.size() > kMaxPs1Checkpoint ||
                      snapshot->frame > ps1_netplay::Timeline::kMaxFrame - inputs.size()))) return {};
    std::lock_guard<std::mutex> lock(netplay_mutex_);
    if (!rollback_ || callback_slot_ != CallbackSlot::kPresentation || !netplay_ready_ || netplay_started_ ||
        stop_ || ended_ || presentation_busy_ || (!snapshot && !presentation_only_)) return {};
    presentation_only_ = true;
    presentation_busy_ = true;
    presentation_render_.emplace(PresentationRender{std::move(snapshot), std::move(inputs), {}});
    presentation_ready_.notify_one();
    return presentation_render_->result.get_future();
  }

  std::optional<uint32_t> HoldNetplay() {
    if (rollback_ || !CheckpointReady()) return std::nullopt;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (checkpoint_) return std::nullopt;
    if (StagingLocal()) {
      if (local_prepared_) return std::nullopt;
      netplay_held_ = true;
      return local_frame_;
    }
    if (!netplay_started_ || !netplay_->Hold()) return std::nullopt;
    netplay_held_ = true;
    return netplay_->frame();
  }

  bool ResumeLocalNetplay() {
    if (!CheckpointReady()) return false;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (!local_staging_ || !netplay_held_ || checkpoint_) return false;
    if (netplay_started_) {
      netplay_started_ = false;
      local_frame_ = 0;
      checkpoint_history_.clear();
    }
    netplay_held_ = false;
    return true;
  }

  std::optional<int64_t> RequestCheckpoint(const Ps1DiscHash& digest, CheckpointBytes restore = {},
                                           std::optional<uint32_t> frame = std::nullopt) {
    if (!CheckpointReady() || restore.size() > kMaxPs1Checkpoint ||
        frame.has_value() != !restore.empty() || (frame && *frame > ps1_netplay::Timeline::kMaxFrame) ||
        (rollback_ && frame != 0u))
      return std::nullopt;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (checkpoint_ || (frame && (netplay_started_ || StagingLocal())) ||
        (replay_cursor_ && (!frame || rollback_)))
      return std::nullopt;
    return QueueCheckpoint(digest, std::move(restore), frame, {});
  }

  std::optional<int64_t> RequestBlankCardCheckpoint(int64_t source, const Ps1DiscHash& digest, bool park = false) {
    if (source != local_checkpoint_source_ || !LocalBlankCardCheckpointReady()) return std::nullopt;
    std::scoped_lock lock(settings_mutex_, checkpoint_mutex_);
    if (checkpoint_ || ordinary_parked_ || (park && !ordinary_recovery_supported_)) return std::nullopt;
    if (park && (options_changed_ || graphics_queued_ || graphics_applying_)) return std::nullopt;
    const auto id = QueueCheckpoint(digest, {}, std::nullopt, {},
                                    park ? CheckpointAction::kParkOrdinary : CheckpointAction::kExchange);
    if (park) ordinary_settings_frozen_ = true;
    return id;
  }

  std::optional<int64_t> RequestOrdinaryCheckpointResume(int64_t source) {
    if (source != local_checkpoint_source_ || !LocalBlankCardCheckpointReady()) return std::nullopt;
    std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    if (checkpoint_ || !ordinary_parked_) return std::nullopt;
    return QueueCheckpoint(ordinary_recovery_digest_, {}, std::nullopt, {}, CheckpointAction::kResumeOrdinary);
  }

  std::shared_ptr<Ps1OrdinaryRecovery> DetachOrdinaryRecovery(int64_t source) {
    if (source != local_checkpoint_source_ || !LocalBlankCardCheckpointReady()) return {};
    std::scoped_lock lock(settings_mutex_, checkpoint_mutex_);
    if (checkpoint_ || !ordinary_parked_ || ordinary_recovery_.empty() || stop_) return {};
    std::map<std::string, std::string> options;
    for (const auto& [key, option] : options_) options.emplace(key, option.values[option.selected]);
    auto recovery = std::make_shared<Ps1OrdinaryRecovery>(Ps1OrdinaryRecovery{
        source, core_, disc_, system_, saves_, std::move(options), ordinary_recovery_digest_, {}, {}});
    recovery->state = std::move(ordinary_recovery_);
    stop_ = true;
    return recovery;
  }

  std::optional<int64_t> RequestConfirmedReturn(const Ps1OrdinaryRecovery& recovery) {
    if (!rollback_ || !netplay_host_ || !CheckpointReady())
      return std::nullopt;
    std::error_code error;
    if (!std::filesystem::equivalent(std::filesystem::u8path(disc_), std::filesystem::u8path(recovery.disc), error))
      return std::nullopt;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (checkpoint_ || !netplay_started_ || confirmed_return_requested_ || replay_cursor_ != 0u)
      return std::nullopt;
    const auto id = QueueCheckpoint(recovery.digest, {}, std::nullopt, {}, CheckpointAction::kReturnOrdinary);
    confirmed_return_requested_ = true;
    return id;
  }

  int64_t recovered_source() const { return recovered_source_.load(); }
  bool AcceptOrdinaryRecovery(int64_t source) {
    if (recovered_source() != source || !startup_visible_ || stop_ || ended_) return false;
    recovery_accepted_ = true;
    return true;
  }

  std::optional<int64_t> RequestCatchup(uint32_t first, std::vector<Ps1Ports> frames) {
    if (rollback_ || !CheckpointReady() || frames.empty() || frames.size() > kPs1CatchupBatch ||
        first > ps1_netplay::Timeline::kMaxFrame - frames.size())
      return std::nullopt;
    std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
    if (checkpoint_ || netplay_started_ || replay_cursor_ != first) return std::nullopt;
    return QueueCheckpoint({}, {}, first, std::move(frames));
  }

  std::optional<Map> CheckpointHistory(uint32_t first) {
    if (rollback_ || !CheckpointReady()) return std::nullopt;
    std::lock_guard<std::mutex> lock(netplay_mutex_);
    const auto next = StagingLocal() ? local_frame_ : netplay_->frame();
    const auto oldest = next - static_cast<uint32_t>(checkpoint_history_.size());
    if ((!netplay_started_ && !StagingLocal()) || first < oldest || first > next) return std::nullopt;
    List frames;
    const auto count = std::min(next - first, kPs1CatchupBatch);
    for (uint32_t frame = first; frame < first + count; ++frame) {
      List ports;
      for (const auto& pad : checkpoint_history_[frame - oldest]) {
        List values{Value(static_cast<int32_t>(pad.buttons))};
        for (const auto axis : pad.axes) values.emplace_back(static_cast<int32_t>(axis));
        ports.emplace_back(std::move(values));
      }
      frames.emplace_back(std::move(ports));
    }
    return Map{{Value("nextFrame"), Value(static_cast<int64_t>(next))},
               {Value("frames"), Value(std::move(frames))}};
  }

  Value TakeCheckpoint(int64_t id) {
    std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    if (!checkpoint_ || checkpoint_->id != id || checkpoint_->cancelled || stop_ || ended_) return Value(false);
    if (!checkpoint_->result) return Value();
    auto result = std::move(*checkpoint_->result);
    checkpoint_.reset();
    return result;
  }

  void CancelCheckpoint(int64_t id) {
    std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    if (!checkpoint_ || checkpoint_->id != id) return;
    if (checkpoint_->frame && checkpoint_->running) stop_ = true;
    if (checkpoint_->running && !checkpoint_->result) {
      checkpoint_->cancelled = true;
    } else {
      if (checkpoint_->action == CheckpointAction::kParkOrdinary && ordinary_parked_) {
        ordinary_recovery_.clear();
        ordinary_parked_ = false;
      }
      if (checkpoint_->action == CheckpointAction::kParkOrdinary) ordinary_settings_frozen_ = false;
      checkpoint_.reset();
    }
  }


  std::optional<Map> SyncNetplay(const std::vector<ps1_netplay::FrameInput>& remote,
                                const std::optional<uint32_t>& acknowledged) {
    if (!netplay_ || stop_ || ended_) return std::nullopt;
    std::lock_guard<std::mutex> lock(netplay_mutex_);
    if (StagingLocal() || presentation_only_ || confirmed_return_requested_ ||
        !netplay_->Receive(remote, acknowledged)) return std::nullopt;
    PublishConfirmedFingerprint();
    List local;
    for (const auto& sample : netplay_->LocalPacket()) {
      List input{Value(static_cast<int64_t>(sample.frame)), Value(static_cast<int32_t>(sample.input.buttons))};
      for (const auto axis : sample.input.axes) input.emplace_back(static_cast<int32_t>(axis));
      local.emplace_back(std::move(input));
    }
    List fingerprints;
    for (const auto& [frame, hash] : netplay_fingerprints_)
      fingerprints.emplace_back(List{Value(static_cast<int64_t>(frame)), Value(hash)});
    return Map{
        {Value("local"), Value(std::move(local))},
        {Value("frame"), Value(static_cast<int64_t>(netplay_->frame()))},
        {Value("confirmedFrame"), FrameValue(netplay_->confirmed())},
        {Value("receivedFrame"), FrameValue(netplay_->received())},
        {Value("waiting"), Value(netplay_->waiting())},
        {Value("fingerprints"), Value(std::move(fingerprints))},
    };
  }

  Map Status() {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    const bool show_progress = std::chrono::steady_clock::now() < progress_until_;
    Map options;
    for (const auto& entry : options_) {
      if (!entry.second.graphics && !entry.second.boot &&
          !LiveCoreOption(entry.first, hardware_frame_)) continue;
      List values;
      for (const auto& value : entry.second.values) values.emplace_back(value);
      const auto selected = entry.second.pending.value_or(entry.second.selected);
      List labels;
      for (const auto& label : entry.second.labels) labels.emplace_back(label);
      options.emplace(Value(entry.first), Value(Map{
          {Value("values"), Value(std::move(values))},
          {Value("labels"), Value(std::move(labels))},
          {Value("title"), Value(entry.second.title)},
          {Value("restart"), Value(entry.second.boot || (entry.second.graphics && !TrialOption(entry.first)))},
          {Value("selected"), Value(static_cast<int32_t>(selected))},
          {Value("activeValue"), Value(entry.second.values[entry.second.selected])},
      }));
    }
    Map status{
        {Value("ready"), Value(frame_ready_.load())},
        {Value("presented"), Value(presented_.load())},
        {Value("startupVisible"), Value(startup_visible_.load())},
        {Value("ended"), Value(ended_.load())},
        {Value("error"), Value(error_)},
        {Value("aspect"), Value(static_cast<double>(aspect_.load()))},
        {Value("hardware"), Value(hardware_frame_.load())},
        {Value("frameWidth"), Value(static_cast<int32_t>(frame_width_.load()))},
        {Value("frameHeight"), Value(static_cast<int32_t>(frame_height_.load()))},
        {Value("resolutionError"), Value(resolution_error_)},
        {Value("graphicsPending"), Value(graphics_queued_ || graphics_applying_)},
        {Value("progressLabel"), show_progress ? Value(progress_label_) : Value()},
        {Value("progressPercent"), show_progress && progress_percent_ >= 0 ? Value(progress_percent_) : Value()},
        {Value("options"), Value(std::move(options))},
        {Value("tuningTelemetry"), Value(TuningTelemetry())},
    };
    if (discs_) {
      const auto& discs = *discs_;
      const bool played = discs.inserted && steps_ > disc_inserted_at_step_;
      status.emplace(Value("discs"), Value(Map{
          {Value("labels"), Value(List(discs.labels.begin(), discs.labels.end()))},
          {Value("inserted"), discs.inserted ? Value(static_cast<int32_t>(*discs.inserted)) : Value()},
          {Value("insertedDiscPath"), discs.inserted_path.empty() ? Value() : Value(discs.inserted_path)},
          {Value("playedDiscPath"), Value(played ? discs.inserted_path : std::string{})},
          {Value("busy"), Value(discs.busy)},
          {Value("failed"), Value(discs.failed)},
      }));
    }
    if (netplay_) status.emplace(Value("netplayReady"), Value(netplay_ready_.load()));
    if (netplay_) status.emplace(Value("portableCheckpoint"), Value(checkpoint_supported_.load()));
    if (LocalBlankCardCheckpointReady()) {
      status.emplace(Value("localBlankCardCheckpoint"), Value(local_checkpoint_source_));
      status.emplace(Value("ordinaryCheckpointParked"), Value(ordinary_parked_.load()));
    }
    if (const auto source = recovered_source()) {
      status.emplace(Value("ps1RecoveredSource"), Value(source));
      status.emplace(Value("ps1ContinuedFromMatch"), Value(continued_from_match_.load()));
    }
    const auto provider_status = ps3_provider_status_.load();
    if (provider_status >= 0) status.emplace(Value("ps3ProviderStatus"), Value(provider_status));
    return status;
  }

  bool SetOption(const std::string& key, const std::string& value) {
    if (!recovery_accepted_ || online_ || netplay_ || (lan_ && key == kPs2Resolution)) return false;
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (ordinary_settings_frozen_ || (discs_ && (discs_->busy || !discs_->inserted))) return false;
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

  bool SetTuningOptions(const Map& expected, const Map& values) {
    if (!recovery_accepted_ || online_ || netplay_ || !presented_ || !hardware_frame_ ||
        values.empty() || expected.size() != values.size())
      return false;
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (ordinary_settings_frozen_ || graphics_applying_ || graphics_queued_ ||
        (discs_ && (discs_->busy || !discs_->inserted))) return false;
    std::vector<std::pair<Option*, size_t>> changes;
    bool checkpoint = false;
    for (const auto& entry : values) {
      const auto* key = std::get_if<std::string>(&entry.first);
      const auto* value = std::get_if<std::string>(&entry.second);
      if (!key || !value || !TrialOption(*key)) return false;
      checkpoint |= Ps1GraphicsOption(*key) || *key == kPs2Resolution;
      if (lan_ && checkpoint) return false;
      const auto* current = StringField(expected, key->c_str());
      const auto option = options_.find(*key);
      if (!current || option == options_.end()) return false;
      auto& data = option->second;
      if (data.values[data.selected] != *current) return false;
      const auto choice = std::find(data.values.begin(), data.values.end(), *value);
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
      if (option.graphics || option.boot) {
        const auto saved = initial_graphics_.find(key);
        if (saved != initial_graphics_.end()) preferred = saved->second;
      }
      for (const auto& value : row->values) {
        if (!value.value) break;
        if (!SupportedBootValue(key, value.value, hardware_available_)) continue;
        if (key == kPs3Resolution && std::atoi(value.value) > 200) continue;
        option.values.emplace_back(value.value);
        option.labels.emplace_back(value.label ? value.label : value.value);
      }
      auto choice = std::find(option.values.begin(), option.values.end(), preferred);
      if (choice == option.values.end() && row->default_value) {
        choice = std::find(option.values.begin(), option.values.end(), row->default_value);
      }
      if (choice != option.values.end()) option.selected = static_cast<size_t>(choice - option.values.begin());
      if (!option.values.empty()) options_.emplace(key, std::move(option));
    }
  }

  template <typename Reopen>
  void ApplyCheckpointGraphics(HMODULE dll, const Reopen& reopen, void (*step)(), bool ps1,
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
    const auto serialize_size = Symbol<size_t (*)()>(dll, "retro_serialize_size");
    const auto serialize = Symbol<bool (*)(void*, size_t)>(dll, "retro_serialize");
    const auto unserialize = Symbol<bool (*)(const void*, size_t)>(dll, "retro_unserialize");
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
      audio_.Flush();
      applied = restore(true);
      if (!applied && !restore(false))
        throw std::runtime_error("The emulator could not restore the game after changing graphics.");
      if (ps1) {
        card = Symbol<void* (*)(unsigned)>(dll, "retro_get_memory_data")(0);
        card_size = Symbol<size_t (*)(unsigned)>(dll, "retro_get_memory_size")(0);
      }
    }
    std::lock_guard<std::mutex> lock(settings_mutex_);
    graphics_applying_ = false;
    if (!applied) resolution_error_ = std::move(failure);
  }

 public:
  void MarkPresented(bool visible) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!frame_) return;
    presented_ = true;
    if (!startup_visible_ && (ps3_ || visible)) startup_visible_ = true;
  }

 private:

  void Publish(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (suppress_frame_ || (presentation_target_ && !rendering_on_target_) ||
        !data || width == 0 || height == 0 || width > kMaxFrameSize || height > kMaxFrameSize)
      return;
    if (data == reinterpret_cast<const void*>(UINTPTR_MAX)) {
      if (hardware_callbacks_.context_type == kD3D11) {
        if (auto frame = hardware_.Read(width, height)) SubmitFrame(std::move(frame), true);
      } else if (hardware_callbacks_.context_type == kVulkan) {
        auto frame = std::make_shared<Frame>();
        frame->width = width;
        frame->height = height;
        frame->pixels.resize(static_cast<size_t>(width) * height * 4);
        if (vulkan_.Read(frame->pixels.data(), width, height)) {
          SubmitFrame(std::move(frame), true);
        } else {
          std::lock_guard<std::mutex> lock(settings_mutex_);
          error_ = "The PS3 Vulkan frame could not be read.";
          stop_ = true;
        }
      }
      return;
    }
    const int format = pixel_format_;
    const size_t bytes = format == 1 ? 4 : 2;
    if (pitch < width * bytes) return;
    auto frame = std::make_shared<Frame>();
    frame->width = width;
    frame->height = height;
    frame->pixels.resize(static_cast<size_t>(width) * height * 4);
    auto* output = frame->pixels.data();
    const auto* input = static_cast<const uint8_t*>(data);
    for (unsigned row = 0; row < height; ++row) {
      const auto* source = input + row * pitch;
      for (unsigned column = 0; column < width; ++column, source += bytes, output += 4) {
        if (format == 1) {
          output[0] = source[2];
          output[1] = source[1];
          output[2] = source[0];
        } else {
          const unsigned pixel = source[0] | (source[1] << 8);
          const unsigned red = (pixel >> (format == 2 ? 11 : 10)) & 31;
          const unsigned green = (pixel >> 5) & (format == 2 ? 63 : 31);
          const unsigned blue = pixel & 31;
          output[0] = static_cast<uint8_t>((red << 3) | (red >> 2));
          output[1] = static_cast<uint8_t>(format == 2 ? (green << 2) | (green >> 4)
                                                       : (green << 3) | (green >> 2));
          output[2] = static_cast<uint8_t>((blue << 3) | (blue >> 2));
        }
        output[3] = 255;
      }
    }
    if (rendering_on_target_) {
      presentation_target_->SubmitFrame(std::move(frame), false);
      ++presentation_target_->presentation_frames_;
    } else {
      SubmitFrame(std::move(frame), false);
    }
  }

  Map TuningTelemetry() {
    using Clock = std::chrono::steady_clock;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    List intervals;
    for (const auto count : frame_intervals_) intervals.emplace_back(count);
    return {
        {Value("clockMs"), Value(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()))},
        {Value("frames"), Value(video_frames_)},
        {Value("steps"), Value(steps_.load())},
        {Value("audioFrames"), Value(audio_frames_.load())},
        {Value("droppedAudioFrames"), Value(audio_.dropped_frames())},
        {Value("frameIntervals"), Value(std::move(intervals))},
        {Value("targetFps"), Value(target_fps_)},
        {Value("audioRate"), Value(audio_rate_)},
    };
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
    const bool visible = startup_visible_ || ps3_ || frame->HasVisiblePixels();
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      frame_ = frame;
    }
    frame_ready_ = true;
    registrar_->Publish(texture_id_, frame->pixels.data(), frame->width, frame->height, visible);
  }

  void SetAspect(float aspect) {
    if (std::isfinite(aspect) && aspect > 0.5f && aspect <= 4.f) aspect_ = aspect;
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
        if (!SupportedBootValue(key, value, hardware_available_)) continue;
        if (ResolutionOption(key) && !value.empty() && value[0] >= '0' && value[0] <= '9' &&
            std::stoi(value) > 4) continue;
        if (!netplay_) {
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
        const auto hardware = std::find(option.values.begin(), option.values.end(), "D3D11");
        if (hardware != option.values.end())
          option.selected = static_cast<size_t>(hardware - option.values.begin());
      }
      if (hardware_available_ && ResolutionOption(key)) {
        const auto saved = std::find(option.values.begin(), option.values.end(), initial_resolution_);
        if (saved != option.values.end())
          option.selected = static_cast<size_t>(saved - option.values.begin());
      }
      if (returning_ ||
          (!netplay_ && !ResolutionOption(key) && (option.boot || (hardware_available_ && TrialOption(key))))) {
        const auto saved = initial_graphics_.find(key);
        if (saved != initial_graphics_.end()) {
          const auto choice = std::find(option.values.begin(), option.values.end(), saved->second);
          if (choice != option.values.end())
            option.selected = static_cast<size_t>(choice - option.values.begin());
        }
      }
      if (netplay_) {
        for (const auto& [name, value] : kPs1NetplayOptions) {
          if (name != key) continue;
          const auto selected = std::find(option.values.begin(), option.values.end(), NetplayOption(name, value));
          if (selected != option.values.end())
            option.selected = static_cast<size_t>(selected - option.values.begin());
          break;
        }
      }
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
        disk.set_image_index && disk.get_num_images && disk_callbacks_.get_image_path &&
        disk_callbacks_.set_initial_image) {
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
    const auto requested_path = DiscPath(*requested);
    const bool changed = !requested_path.empty() && (was_open || disk.set_eject_state(true)) &&
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
    if (changed) boot_disc_ = InitialDisc{*requested, requested_path};
    RefreshDiscs(!changed);
  }

  bool Environment(unsigned command, void* data) {
    if (command == kSavestateContext) {
      const auto context = rollback_context_.load();
      if (context == RollbackContext::kInactive) return false;
      if (data) {
        *static_cast<int*>(data) = static_cast<int>(context);
        rollback_context_queried_ = true;
      }
      return true;
    }
    if (command == kShutdown) {
      if (callback_slot_ == CallbackSlot::kPresentation) {
        Stop();
      } else {
        stop_ = true;
      }
      return true;
    }
    if (command == kGetInputBitmasks) return netplay_ != nullptr;
    if (!data) return false;
    switch (command) {
      case kDiskControlVersion:
        if (!ps1_ || netplay_) return false;
        *static_cast<unsigned*>(data) = 1;
        return true;
      case kSetDiskControl:
        if (!ps1_ || netplay_) return false;
        disk_callbacks_ = {};
        disk_callbacks_.basic = *static_cast<const DiskControlCallbacks*>(data);
        return true;
      case kSetDiskControlExt:
        if (!ps1_ || netplay_) return false;
        disk_callbacks_ = *static_cast<const DiskControlExtCallbacks*>(data);
        return true;
      case kMessageVersion:
        *static_cast<unsigned*>(data) = 1;
        return true;
      case kFrontendCompileProgress:
        *static_cast<bool*>(data) = ps3_;
        return ps3_;
      case kExtendedMessage: {
        const auto& message = *static_cast<const ExtendedMessage*>(data);
        if (online_ && message.type == 0 && message.level == 3 &&
            message.target != 2 && message.text) {
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
      case kSetHardware: {
        auto& callbacks = *static_cast<HardwareCallbacks*>(data);
        if (!hardware_available_ || callbacks.context_type != (ps3_ ? kVulkan : kD3D11))
          return false;
        callbacks.framebuffer = []() -> uintptr_t { return 0; };
        callbacks.proc_address = [](const char*) -> void (*)() { return nullptr; };
        hardware_callbacks_ = callbacks;
        return true;
      }
      case kHardwareNegotiation:
        return ps3_ && vulkan_.SetNegotiation(*static_cast<const RetroVulkanNegotiation*>(data));
      case kHardwareInterface:
        if (hardware_callbacks_.context_type == kVulkan) {
          *static_cast<const RetroVulkanInterface**>(data) = &vulkan_.render_interface();
          return true;
        }
        if (hardware_callbacks_.context_type == kD3D11) {
          *static_cast<const D3D11Interface**>(data) = &hardware_.render_interface;
          return true;
        }
        return false;
      case kPreferredHardware:
        if (!hardware_available_) return false;
        *static_cast<int*>(data) = ps3_ ? kVulkan : kD3D11;
        return true;
      case kCanDupe:
        *static_cast<bool*>(data) = true;
        return true;
      case kSystemDirectory:
        *static_cast<const char**>(data) = system_.c_str();
        return true;
      case kCorePath:
        *static_cast<const char**>(data) = core_.c_str();
        return true;
      case kContentDirectory:
        if (installing_packages()) return false;
        *static_cast<const char**>(data) = content_.c_str();
        return true;
      case kSaveDirectory:
        *static_cast<const char**>(data) = saves_.c_str();
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
        if (lan_ && variable.key && std::strcmp(variable.key, "pcsx2_dev9_eth") == 0) {
          variable.value = "enabled";
          return true;
        }
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
      case kSetAv:
        SetAspect(static_cast<const AvInfo*>(data)->geometry.aspect);
        return true;
      case kSetGeometry:
        SetAspect(static_cast<const Geometry*>(data)->aspect);
        return true;
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
    if (netplay_) {
      if (port >= netplay_ports_.size()) return 0;
      const auto& input = netplay_ports_[port];
      if (device == 1 && id == 256) return static_cast<int16_t>(input.buttons);
      if (device == 1 && id < 16) return (input.buttons & (1u << id)) != 0;
      if (device == 5 && index < 2 && id < 2) return input.axes[index * 2 + id];
      return 0;
    }
    if (suppress_frame_ || port >= kLocalPorts || paused_) return 0;
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (device == 1 && id < 16) return (buttons_[port] & (1u << id)) != 0;
    if (device == 5 && index < 2 && id < 2) return axes_[port][index * 2 + id];
    return 0;
  }

  std::string_view NetplayOption(std::string_view key, std::string_view default_value) const {
    if (key == "swanstation_CPU_ExecutionMode" && match_cpu_ == Ps1MatchCpu::kRecompiler)
      return "Recompiler";
    return default_value;
  }

  void ValidateNetplay(const uint8_t* memory, size_t memory_size) {
    if (!memory || memory_size != 2 * 1024 * 1024)
      throw std::runtime_error("PS1 netplay could not access the core's main RAM.");
    {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      for (const auto& [name, value] : kPs1NetplayOptions) {
        const auto option = options_.find(std::string(name));
        if (option == options_.end() || option->second.values[option->second.selected] != NetplayOption(name, value))
          throw std::runtime_error("The PS1 core could not configure the required netplay profile.");
      }
    }
  }

  enum class CheckpointAction { kExchange, kParkOrdinary, kResumeOrdinary, kReturnOrdinary };

  struct CheckpointJob {
    int64_t id;
    Ps1DiscHash digest;
    CheckpointBytes restore;
    std::optional<uint32_t> frame;
    std::vector<Ps1Ports> replay;
    CheckpointAction action;
    bool running = false;
    std::optional<Value> result;
    bool cancelled = false;
  };

  using SavePortable = size_t (*)(void*, size_t, const uint8_t*);
  using LoadPortable = bool (*)(const void*, size_t, const uint8_t*);

  bool CheckpointReady() const { return checkpoint_supported_ && netplay_ready_ && !stop_ && !ended_; }
  bool LocalBlankCardCheckpointReady() const {
    return recovery_accepted_ && local_blank_checkpoint_supported_ && startup_visible_ && !stop_ && !ended_;
  }

  // Caller holds netplay_mutex_.
  bool StagingLocal() const { return local_staging_ && !netplay_started_; }

  // Caller holds checkpoint_mutex_.
  int64_t QueueCheckpoint(const Ps1DiscHash& digest, CheckpointBytes restore,
                          std::optional<uint32_t> frame, std::vector<Ps1Ports> replay,
                          CheckpointAction action = CheckpointAction::kExchange) {
    static std::atomic<int64_t> next_id{0};
    const auto id = ++next_id;
    checkpoint_ = CheckpointJob{id, digest, std::move(restore), frame, std::move(replay), action, false, std::nullopt};
    return id;
  }

  // Returns true when a private restore may have replaced the card buffer.
  bool ServiceCheckpoint(SavePortable save, LoadPortable load, size_t capacity, void (*step)(),
                         SavePortable save_private = nullptr) {
    std::optional<CheckpointJob> work;
    {
      std::lock_guard<std::mutex> lock(checkpoint_mutex_);
      if (!checkpoint_ || checkpoint_->running || checkpoint_->action == CheckpointAction::kReturnOrdinary)
        return false;
      work = std::move(*checkpoint_);
      // Keep the request identity and kind visible while the core call runs.
      checkpoint_->frame = work->frame;
      checkpoint_->running = true;
      if (work->action == CheckpointAction::kResumeOrdinary) work->restore = std::move(ordinary_recovery_);
    }
    Value result(false);
    std::optional<uint32_t> cursor;
    CheckpointBytes recovery;
    bool resumed = false;
    if (work->action == CheckpointAction::kResumeOrdinary) {
      resumed = load(work->restore.data(), work->restore.size(), work->digest.data());
      result = Value(resumed);
    } else if (!work->frame) {
      int64_t frame = steps_.load();
      if (netplay_) {
        std::lock_guard<std::mutex> lock(netplay_mutex_);
        frame = StagingLocal() ? local_frame_ : netplay_->frame();
      }
      auto captured = anyps5::retro::ps1_checkpoint::CaptureState(
          save, save_private, capacity, work->digest, static_cast<uint64_t>(frame),
          work->action == CheckpointAction::kParkOrdinary);
      if (captured) {
        recovery = std::move(captured->private_state);
        result = Value(Map{{Value("frame"), Value(frame)},
                           {Value("state"), Value(std::move(captured->snapshot.state))}});
      }
    } else if (!work->restore.empty()) {
      const bool imported = load(work->restore.data(), work->restore.size(), work->digest.data());
      if (imported) cursor = work->frame;
      result = Value(imported);
    } else {
      suppress_frame_ = true;
      for (const auto& ports : work->replay) {
        if (stop_) break;
        netplay_ports_ = ports;
        step();
      }
      suppress_frame_ = false;
      netplay_ports_ = {};
      if (!stop_) cursor = *work->frame + static_cast<uint32_t>(work->replay.size());
      result = Value(cursor.has_value());
    }
    std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    if (work->action == CheckpointAction::kResumeOrdinary) {
      if (resumed) {
        ordinary_parked_ = false;
        ordinary_settings_frozen_ = false;
      } else {
        ordinary_recovery_ = std::move(work->restore);
      }
    }
    if (checkpoint_->cancelled) {
      checkpoint_.reset();
    } else {
      if (!recovery.empty()) {
        ordinary_recovery_ = std::move(recovery);
        ordinary_recovery_digest_ = work->digest;
        ordinary_parked_ = true;
        audio_.Flush();
      }
      if (cursor) replay_cursor_ = cursor;
      checkpoint_->result = std::move(result);
    }
    if (work->action == CheckpointAction::kParkOrdinary && !ordinary_parked_) ordinary_settings_frozen_ = false;
    return work->action == CheckpointAction::kResumeOrdinary;
  }

  // Caller holds netplay_mutex_. At most one fingerprint can be pending
  // because the sampling interval exceeds the prediction window.
  void PublishConfirmedFingerprint() {
    if (!netplay_pending_fingerprint_ ||
        netplay_->confirmed() < netplay_pending_fingerprint_->first) return;
    if (netplay_fingerprints_.size() == 16) netplay_fingerprints_.erase(netplay_fingerprints_.begin());
    netplay_fingerprints_.push_back(std::move(*netplay_pending_fingerprint_));
    netplay_pending_fingerprint_.reset();
  }

  struct RollbackAudioFrame {
    uint32_t frame = 0;
    size_t samples = 0;
    bool overflow = false;
    std::array<int16_t, kMaxPs1AudioSamplesPerFrame> pcm{};
  };

  void EmitAudio(const int16_t* stereo, size_t frames) {
    if (!frames) return;
    audio_frames_ += frames;
    if (audio_sink_) {
      audio_sink_(stereo, frames);
    } else {
      audio_.Write(stereo, frames);
    }
  }

  void OnAudio(const int16_t* stereo, size_t frames) {
    if (paused_ || !frames) return;
    if (rollback_) {
      // The software PS1 core submits PCM synchronously during retro_run.
      auto* capture = rollback_audio_capture_;
      if (!capture) return;
      if (!stereo || frames > (capture->pcm.size() - capture->samples) / 2) {
        capture->overflow = true;
        return;
      }
      std::copy_n(stereo, frames * 2, capture->pcm.data() + capture->samples);
      capture->samples += frames * 2;
    } else if (!suppress_frame_) {
      EmitAudio(stereo, frames);
    }
  }

  enum class RollbackContext { kInactive, kSameInstance = 1, kSameBinary = 2 };
  using SerializeRollback = bool (*)(void*, size_t);
  using UnserializeRollback = bool (*)(const void*, size_t);

  bool SaveRollback(SerializeRollback serialize, CheckpointBytes& bytes) {
    rollback_context_ = RollbackContext::kSameInstance;
    const bool saved = serialize(bytes.data(), bytes.size());
    rollback_context_ = RollbackContext::kInactive;
    return saved;
  }

  bool RestoreRollback(UnserializeRollback unserialize, const CheckpointBytes& bytes, RollbackContext context) {
    suppress_frame_ = true;
    rollback_context_queried_ = false;
    rollback_context_ = context;
    const bool restored = unserialize(bytes.data(), bytes.size());
    rollback_context_ = RollbackContext::kInactive;
    suppress_frame_ = false;
    return restored && rollback_context_queried_;
  }

  struct PresentationRender {
    std::optional<PresentationState> snapshot;
    std::vector<Ps1Ports> inputs;
    std::promise<bool> result;
  };

  void ServicePresentation(SerializeRollback serialize, UnserializeRollback unserialize,
                           size_t state_size, void (*step)()) {
    std::optional<std::promise<std::optional<PresentationState>>> capture;
    std::optional<PresentationRender> render;
    uint32_t cursor;
    {
      std::lock_guard<std::mutex> lock(netplay_mutex_);
      if (!presentation_capture_ && !presentation_render_) return;
      capture = std::exchange(presentation_capture_, std::nullopt);
      render = std::exchange(presentation_render_, std::nullopt);
      cursor = netplay_->cursor();
    }
    std::optional<PresentationState> snapshot;
    bool rendered = false;
    if (capture) {
      CheckpointBytes bytes(state_size);
      if (SaveRollback(serialize, bytes) && !stop_) snapshot.emplace(PresentationState{cursor, disc_, std::move(bytes)});
    } else {
      if (const auto& source = render->snapshot) {
        presentation_cursor_.reset();
        if (source->bytes.size() == state_size &&
            RestoreRollback(unserialize, source->bytes, RollbackContext::kSameBinary)) {
          presentation_cursor_ = source->frame;
        }
      }
      if (presentation_cursor_ &&
          *presentation_cursor_ <= ps1_netplay::Timeline::kMaxFrame - render->inputs.size()) {
        for (const auto& ports : render->inputs) {
          if (stop_) break;
          netplay_ports_ = ports;
          rendering_on_target_ = presentation_target_ != nullptr;
          step();
          rendering_on_target_ = false;
          ++*presentation_cursor_;
        }
        rendered = !stop_;
      }
    }
    {
      std::lock_guard<std::mutex> lock(netplay_mutex_);
      presentation_busy_ = false;
    }
    if (capture) capture->set_value(std::move(snapshot));
    if (render) render->result.set_value(rendered);
  }

  struct RollbackState {
    std::optional<uint32_t> frame;
    CheckpointBytes bytes;
  };

  // Run only on the emulator thread at a frame boundary. The transport has
  // stopped feeding inputs, so the first unconfirmed frame cannot move.
  void ServiceConfirmedReturn(const std::vector<RollbackState>& checkpoints, UnserializeRollback restore,
                              SavePortable export_blank, size_t capacity) {
    std::optional<uint32_t> confirmed;
    uint32_t cursor;
    Ps1DiscHash digest;
    {
      std::scoped_lock lock(netplay_mutex_, checkpoint_mutex_);
      if (!checkpoint_ || checkpoint_->action != CheckpointAction::kReturnOrdinary || checkpoint_->running)
        return;
      checkpoint_->running = true;
      confirmed = netplay_->confirmed();
      cursor = netplay_->cursor();
      digest = checkpoint_->digest;
    }
    if (presentation_core_) presentation_core_->Stop();
    Value result(false);
    if (confirmed && export_blank) {
      const auto next = *confirmed + 1;
      const auto& snapshot = checkpoints[next % checkpoints.size()];
      if (cursor == next ||
          (snapshot.frame == next && RestoreRollback(restore, snapshot.bytes, RollbackContext::kSameInstance))) {
        CheckpointBytes state(capacity);
        const auto size = export_blank(state.data(), state.size(), digest.data());
        if (size && size <= state.size()) {
          state.resize(size);
          result = Value(std::move(state));
        }
      }
    }
    std::lock_guard<std::mutex> lock(checkpoint_mutex_);
    if (checkpoint_->cancelled) {
      checkpoint_.reset();
    } else {
      checkpoint_->result = std::move(result);
    }
  }

  void RunNetplay(HMODULE dll, void (*step)(), std::chrono::steady_clock::duration frame_time, double sample_rate,
                  const uint8_t* memory, size_t memory_size) {
    using Clock = std::chrono::steady_clock;
    const auto save = reinterpret_cast<SavePortable>(GetProcAddress(dll, "retro_anyps5_export_checkpoint_v1"));
    const auto load = reinterpret_cast<LoadPortable>(GetProcAddress(dll, "retro_anyps5_import_checkpoint_v1"));
    const size_t maximum = save && load ? Symbol<size_t (*)()>(dll, "retro_serialize_size")() : 0;
    const bool can_import_handoff =
        callback_slot_ == CallbackSlot::kPrimary && (match_cpu_ == Ps1MatchCpu::kRecompiler || !presentation_core_);
    checkpoint_supported_ =
        (!rollback_ || can_import_handoff) && maximum && maximum <= kMaxPs1Checkpoint - kPs1CheckpointPrefix;
    if (local_staging_ && !checkpoint_supported_)
      throw std::runtime_error("Local PS1 hosting requires the portable checkpoint core.");
    std::vector<RollbackState> checkpoints(netplay_->prediction_window());
    // The extra slot holds the frame being stepped before its confirmation.
    std::vector<RollbackAudioFrame> pending_audio(rollback_ ? checkpoints.size() + 1 : 0);
    std::unique_ptr<ConfirmedPcmPlayout> playout;
    if (presentation_core_) {
      if (!std::isfinite(sample_rate) || sample_rate < 8000 || sample_rate > 192000)
        throw std::runtime_error("The PS1 core supplied an invalid audio sample rate.");
      playout = std::make_unique<ConfirmedPcmPlayout>(sample_rate, [this](uint32_t frame, const int16_t* pcm,
                                                                        size_t samples) {
        EmitAudio(pcm, samples);
        rollback_audio_drained_ = frame;
      });
    }
    uint32_t next_audio_frame = 0;
    const auto release_audio = [&] {
      if (!rollback_) return;
      std::optional<uint32_t> confirmed;
      {
        std::lock_guard<std::mutex> lock(netplay_mutex_);
        confirmed = netplay_->confirmed();
      }
      while (confirmed && next_audio_frame <= *confirmed) {
        const auto& audio = pending_audio[next_audio_frame % pending_audio.size()];
        if (audio.frame != next_audio_frame)
          throw std::runtime_error("The PS1 rollback audio frame is unavailable.");
        if (playout) {
          playout->Submit(next_audio_frame, audio.pcm.data(), audio.samples);
        } else {
          EmitAudio(audio.pcm.data(), audio.samples / 2);
          rollback_audio_drained_ = next_audio_frame;
        }
        ++next_audio_frame;
      }
      if (playout) {
        audio_underruns_ = playout->underruns();
        audio_queue_peak_ = playout->peak();
      }
    };
    const auto serialize = rollback_ ? Symbol<SerializeRollback>(dll, "retro_serialize") : nullptr;
    const auto unserialize = rollback_ ? Symbol<UnserializeRollback>(dll, "retro_unserialize") : nullptr;
    const auto export_blank = rollback_ ? reinterpret_cast<SavePortable>(
        GetProcAddress(dll, "retro_anyps5_export_blank_card_checkpoint_v1")) : nullptr;
    const auto state_size = rollback_ ? Symbol<size_t (*)()>(dll, "retro_serialize_size")() : 0;
    const auto memory_data = rollback_ ? Symbol<void* (*)(unsigned)>(dll, "retro_get_memory_data") : nullptr;
    if (rollback_ && (!state_size || state_size > kMaxPs1Checkpoint))
      throw std::runtime_error("The PS1 core cannot provide bounded rollback checkpoints.");
    if (presentation_core_) {
      const auto deadline = Clock::now() + std::chrono::seconds(20);
      while (!presentation_core_->netplay_ready_ && !stop_) {
        if (presentation_core_->ended_ || Clock::now() >= deadline)
          throw std::runtime_error("The PS1 rollback presentation core did not start.");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (stop_) return;
    }
    netplay_ready_ = true;
    auto next = Clock::now();
    auto last_advance = next;
    bool started_before = false;
    std::optional<uint32_t> presented_cursor;
    std::future<bool> presentation_render;
    bool replay_slice = false;
    auto tick_begin = Clock::now();
    const bool measure_trace = !input_trace_.empty();
    std::array<Clock::duration, kTraceStageCount> stage_times{};
    uint32_t trace_frame = 0;
    const auto trace_stage = [&](TraceStage stage, auto&& operation) {
      if (!measure_trace) {
        operation();
        return;
      }
      const auto begin = Clock::now();
      operation();
      stage_times[stage] += Clock::now() - begin;
    };
    while (!stop_) {
      if (confirmed_return_requested_) {
        playout.reset();
        ServiceConfirmedReturn(checkpoints, unserialize, export_blank, maximum + kPs1CheckpointPrefix);
        while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return;
      }
      if (!replay_slice && (presentation_core_ || measure_trace)) {
        tick_begin = Clock::now();
        stage_times.fill(Clock::duration::zero());
      }
      const auto record_trace_work = [&] {
        if (!measure_trace) return;
        const auto elapsed = Clock::now() - tick_begin;
        using Micros = std::chrono::microseconds;
        const auto work = std::chrono::duration_cast<Micros>(elapsed).count();
        const bool overrun = work > 16667;
        if (!overrun && work <= trace_max_work_us_) return;
        std::array<int64_t, kTraceStageCount> measured{};
        auto other = elapsed;
        for (size_t stage = 0; stage < kOther; ++stage) {
          measured[stage] = std::chrono::duration_cast<Micros>(stage_times[stage]).count();
          other -= stage_times[stage];
        }
        measured[kOther] = std::chrono::duration_cast<Micros>(other).count();
        std::lock_guard<std::mutex> lock(trace_breakdown_mutex_);
        if (overrun) {
          ++trace_work_overruns_;
          const auto dominant = std::max_element(measured.begin(), measured.end()) - measured.begin();
          ++trace_breakdown_.dominant_overruns[dominant];
        }
        if (work > trace_breakdown_.total_us) {
          trace_max_work_us_ = work;
          trace_breakdown_.total_us = work;
          trace_breakdown_.frame = trace_frame;
          trace_breakdown_.worst_us = measured;
        }
      };
      trace_stage(kAudio, release_audio);
      if (rollback_) ServicePresentation(serialize, unserialize, state_size, step);
      if (checkpoint_supported_) ServiceCheckpoint(save, load, maximum + kPs1CheckpointPrefix, step);
      ps1_netplay::PadInput local;
      {
        std::lock_guard<std::mutex> lock(input_mutex_);
        local = {buttons_[0], axes_[0]};
      }
      if (presentation_core_) {
        if (presentation_render.valid() &&
            presentation_render.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
          bool rendered = false;
          try {
            rendered = presentation_render.get();
          } catch (const std::future_error&) {
            // The spare can exit independently. The primary still owns the match.
          }
          if (rendered) {
            ++presentation_steps_;
          } else {
            presented_cursor.reset();
          }
        }
        if (!presentation_render.valid() && !replay_slice) {
          uint32_t cursor;
          uint32_t frame;
          Ps1Ports ports{};
          bool render = false;
          {
            std::lock_guard<std::mutex> lock(netplay_mutex_);
            cursor = netplay_->cursor();
            if (presented_cursor && cursor >= *presented_cursor) presented_cursor.reset();
            const auto correction = netplay_->correction();
            frame = presented_cursor.value_or(cursor);
            const bool handoff = correction && cursor - *correction >= 2 && cursor == netplay_->frame();
            const bool keep_rendering = presented_cursor && cursor + 1 < *presented_cursor;
            if ((handoff || keep_rendering) && (input_trace_.empty() || frame < input_trace_.size())) {
              auto sample = local;
              if (!input_trace_.empty()) {
                const auto target = frame + ps1_netplay::Timeline::kDelay;
                sample = target < input_trace_.size() ? input_trace_[target] : ps1_netplay::PadInput{};
              }
              render = netplay_->Preview(frame, sample, ports);
            }
          }
          if (render) {
            std::optional<PresentationState> snapshot;
            if (!presented_cursor) {
              CheckpointBytes bytes(state_size);
              if (!SaveRollback(serialize, bytes))
                throw std::runtime_error("The PS1 core could not save a presentation checkpoint.");
              snapshot.emplace(PresentationState{cursor, disc_, std::move(bytes)});
            }
            presentation_render = presentation_core_->RenderPresentation(std::move(snapshot), {ports});
            if (presentation_render.valid()) presented_cursor = frame + 1;
          }
        }
      }
      while (rollback_ && !stop_) {
        std::optional<uint32_t> correction;
        {
          std::lock_guard<std::mutex> lock(netplay_mutex_);
          correction = netplay_->correction();
        }
        if (!correction) break;
        const auto& checkpoint = checkpoints[*correction % checkpoints.size()];
        if (checkpoint.frame != correction)
          throw std::runtime_error("The PS1 rollback checkpoint is unavailable.");
        trace_stage(kRestore, [&] {
          if (!RestoreRollback(unserialize, checkpoint.bytes, RollbackContext::kSameInstance))
            throw std::runtime_error("The PS1 core rejected the in-memory rollback checkpoint.");
        });
        memory = static_cast<const uint8_t*>(memory_data(2));
        if (!memory) throw std::runtime_error("The PS1 core lost main RAM during rollback.");
        std::lock_guard<std::mutex> lock(netplay_mutex_);
        if (netplay_->Rewind(*correction) && netplay_pending_fingerprint_ &&
            netplay_pending_fingerprint_->first >= *correction)
          netplay_pending_fingerprint_.reset();
      }
      if (stop_) break;
      bool started;
      bool staging;
      bool epoch_reset;
      bool advance;
      bool checkpoint_needed = false;
      bool replaying = false;
      uint32_t frame;
      {
        std::lock_guard<std::mutex> lock(netplay_mutex_);
        started = netplay_started_ && !netplay_held_;
        staging = StagingLocal();
        epoch_reset = std::exchange(netplay_epoch_reset_, false);
        frame = staging ? local_frame_ : netplay_->cursor();
        if (staging) {
          advance = !netplay_held_ && frame < ps1_netplay::Timeline::kMaxFrame;
          local_prepared_ = advance;
          if (advance) netplay_ports_ = {local, {}};
        } else if (input_trace_.empty() || frame < input_trace_.size()) {
          if (!input_trace_.empty()) {
            const auto sample_frame = frame + ps1_netplay::Timeline::kDelay;
            local = sample_frame < input_trace_.size() ? input_trace_[sample_frame] : ps1_netplay::PadInput{};
          }
          advance = netplay_->Prepare(local, netplay_ports_);
          checkpoint_needed = netplay_->needs_checkpoint();
          replaying = frame < netplay_->frame();
        } else {
          advance = false;
        }
      }
      trace_frame = frame;
      if (started && (!started_before || epoch_reset)) {
        last_advance = Clock::now();
      }
      started_before = started;
      if (!advance) {
        if (callback_slot_ == CallbackSlot::kPresentation && !started) {
          std::unique_lock<std::mutex> lock(netplay_mutex_);
          presentation_ready_.wait(lock, [this] { return stop_ || netplay_started_ || presentation_render_.has_value(); });
          next = Clock::now();
          continue;
        }
        // The invitation owner releases a staged host on timeout or disconnect.
        if (!local_staging_ &&
            Clock::now() - last_advance > (started ? std::chrono::seconds(15) : std::chrono::seconds(60)))
          throw std::runtime_error(started ? "PS1 netplay stopped waiting for the other player."
                                           : "PS1 netplay did not start in time.");
        if (started && frame < input_trace_.size()) record_trace_work();
        replay_slice = false;
        const auto wait_started = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        next = Clock::now();
        if (started && frame < input_trace_.size()) {
          ++trace_blocked_polls_;
          trace_blocked_us_ += std::chrono::duration_cast<std::chrono::microseconds>(next - wait_started).count();
        }
        continue;
      }
      if (checkpoint_needed) {
        trace_stage(kSave, [&] {
          auto& checkpoint = checkpoints[frame % checkpoints.size()];
          checkpoint.bytes.resize(state_size);
          if (!SaveRollback(serialize, checkpoint.bytes))
            throw std::runtime_error("The PS1 core could not save a rollback checkpoint.");
          checkpoint.frame = frame;
        });
      }
      suppress_frame_ = replaying || presentation_render.valid() || (presented_cursor && frame < *presented_cursor);
      if (rollback_) {
        auto& audio = pending_audio[frame % pending_audio.size()];
        audio.frame = frame;
        audio.samples = 0;
        audio.overflow = false;
        rollback_audio_capture_ = &audio;
      }
      const auto* captured_audio = rollback_audio_capture_;
      const auto run_step = [&] {
        struct ClearAudioCapture {
          ~ClearAudioCapture() { rollback_audio_capture_ = nullptr; }
        } clear;
        step();
      };
      if (measure_trace && detailed_step_trace_) {
        struct VideoTimer {
          VideoTimer() { trace_video_time_ = &elapsed; }
          ~VideoTimer() { trace_video_time_ = nullptr; }
          Clock::duration elapsed{};
        } video;
        ULONG64 before_cycles = 0, after_cycles = 0;
        const auto thread = GetCurrentThread();
        const bool measured_before = QueryThreadCycleTime(thread, &before_cycles);
        const auto begin = Clock::now();
        run_step();
        const auto elapsed = Clock::now() - begin;
        const bool measured = measured_before && QueryThreadCycleTime(thread, &after_cycles) &&
                              after_cycles >= before_cycles;
        const auto cycles = measured ? after_cycles - before_cycles : 0;
        stage_times[kStep] += elapsed;
        using Micros = std::chrono::microseconds;
        const auto elapsed_us = std::chrono::duration_cast<Micros>(elapsed).count();
        std::lock_guard<std::mutex> lock(trace_breakdown_mutex_);
        if (measured) {
          trace_step_.total_cycles += cycles;
          ++trace_step_.cycle_samples;
        }
        if (elapsed_us > trace_step_.elapsed_us) {
          trace_step_.frame = frame;
          trace_step_.elapsed_us = elapsed_us;
          trace_step_.video_us = std::chrono::duration_cast<Micros>(video.elapsed).count();
          trace_step_.cycles = measured ? std::optional<uint64_t>(cycles) : std::nullopt;
        }
      } else {
        trace_stage(kStep, run_step);
      }
      suppress_frame_ = false;
      if (captured_audio && captured_audio->overflow)
        throw std::runtime_error("The PS1 rollback audio frame exceeded its PCM capacity.");
      ++steps_;
      std::string fingerprint;
      if (!staging && (frame + 1) % 120 == 0) {
        trace_stage(kFingerprint, [&] { fingerprint = NetplayFingerprint(memory, memory_size); });
      }
      {
        std::lock_guard<std::mutex> lock(netplay_mutex_);
        if (staging) {
          ++local_frame_;
          local_prepared_ = false;
        } else {
          netplay_->Commit();
          if (rollback_)
            peak_prediction_depth_ = std::max(peak_prediction_depth_.load(), netplay_->prediction_depth());
        }
        if (checkpoint_supported_ && !rollback_) {
          if (checkpoint_history_.size() == kPs1CatchupHistory) checkpoint_history_.pop_front();
          checkpoint_history_.push_back(netplay_ports_);
        }
        if (!fingerprint.empty()) netplay_pending_fingerprint_.emplace(frame, std::move(fingerprint));
        PublishConfirmedFingerprint();
      }
      trace_stage(kAudio, release_audio);
      last_advance = Clock::now();
      // The spare can move past the old simulation high-water mark. The
      // primary still needs catch-up work until it reaches the spare's tip.
      if (!replay_slice && presented_cursor && frame + 1 < *presented_cursor &&
          last_advance - tick_begin < std::chrono::microseconds(7500)) {
        replay_slice = true;
        continue;
      }
      replay_slice = false;
      next += frame_time;
      if (next < last_advance - frame_time) next = last_advance;
      record_trace_work();
      const bool measure_wake = !input_trace_.empty() && next > Clock::now();
      std::this_thread::sleep_until(next);
      if (measure_wake) {
        const auto late = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - next).count();
        trace_peak_wake_us_ = std::max(trace_peak_wake_us_.load(), late);
        if (late > 2000) ++trace_late_wakes_;
      }
    }
  }

  // Libretro callbacks carry no user pointer. Each independently loaded DLL
  // receives functions specialized for its slot, including on core threads.
  class CallbackLease {
   public:
    explicit CallbackLease(CallbackSlot slot) : slot_(static_cast<size_t>(slot)) {}
    ~CallbackLease() { Release(); }

    void Acquire(RetroCoreSession* session) {
      RetroCoreSession* vacant = nullptr;
      if (!active_[slot_].compare_exchange_strong(vacant, session))
        throw std::runtime_error("The emulator callback slot is already in use.");
      owns_ = true;
    }

    void Attach(HMODULE dll) {
      std::lock_guard<std::mutex> lock(module_mutex_);
      if (std::find(modules_.begin(), modules_.end(), dll) != modules_.end())
        throw std::runtime_error("The emulator DLL instance already has a callback owner.");
      modules_[slot_] = dll;
    }

    // Release after unload/deinit so their callbacks can still find the owner.
    void Release() {
      if (!owns_) return;
      {
        std::lock_guard<std::mutex> lock(module_mutex_);
        modules_[slot_] = nullptr;
      }
      active_[slot_] = nullptr;
      owns_ = false;
    }

   private:
    const size_t slot_;
    bool owns_ = false;
  };

  template <size_t Slot>
  void BindGameplayCallbacks(HMODULE dll) {
    Symbol<void (*)(bool (*)(unsigned, void*))>(dll, "retro_set_environment")(
        [](unsigned command, void* data) { return active_[Slot].load()->Environment(command, data); });
    Symbol<void (*)(void (*)(const void*, unsigned, unsigned, size_t))>(dll, "retro_set_video_refresh")(
        [](const void* data, unsigned width, unsigned height, size_t pitch) {
          auto* session = active_[Slot].load();
          if (trace_video_time_) {
            using Clock = std::chrono::steady_clock;
            const auto begin = Clock::now();
            session->Publish(data, width, height, pitch);
            *trace_video_time_ += Clock::now() - begin;
          } else {
            session->Publish(data, width, height, pitch);
          }
        });
    Symbol<void (*)(void (*)(int16_t, int16_t))>(dll, "retro_set_audio_sample")(
        [](int16_t left, int16_t right) {
          const int16_t stereo[]{left, right};
          active_[Slot].load()->OnAudio(stereo, 1);
        });
    Symbol<void (*)(size_t (*)(const int16_t*, size_t))>(dll, "retro_set_audio_sample_batch")(
        [](const int16_t* samples, size_t frames) {
          active_[Slot].load()->OnAudio(samples, frames);
          return frames;
        });
    Symbol<void (*)(void (*)())>(dll, "retro_set_input_poll")([] {});
    Symbol<void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned))>(dll, "retro_set_input_state")(
        [](unsigned port, unsigned device, unsigned index, unsigned id) {
          return active_[Slot].load()->Input(port, device, index, id);
        });
  }

  void RunPackageInstall() {
    using VoidFunction = void (*)();
    HMODULE dll = nullptr;
    VoidFunction deinit = nullptr;
    bool initialized = false;
    CallbackLease callbacks(callback_slot_);
    try {
      callbacks.Acquire(this);
      const auto path = std::filesystem::u8path(core_);
      dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (!dll) throw std::runtime_error("Could not load the RPCS3 package installer.");
      callbacks.Attach(dll);
      if (Symbol<unsigned (*)()>(dll, "retro_api_version")() != 1)
        throw std::runtime_error("Could not load the RPCS3 package installer.");
      SelectPs3NetworkPolicy(dll, std::nullopt, true);
      const auto init = Symbol<VoidFunction>(dll, "retro_init");
      deinit = Symbol<VoidFunction>(dll, "retro_deinit");
      const auto load = Symbol<bool (*)(const GameInfo*)>(dll, "retro_load_game");
      const auto unload = Symbol<VoidFunction>(dll, "retro_unload_game");
      Symbol<void (*)(bool (*)(unsigned, void*))>(dll, "retro_set_environment")(
          [](unsigned command, void* data) { return active_[0].load()->Environment(command, data); });
      Symbol<void (*)(void (*)(const void*, unsigned, unsigned, size_t))>(dll, "retro_set_video_refresh")(
          [](const void*, unsigned, unsigned, size_t) {});
      Symbol<void (*)(void (*)(int16_t, int16_t))>(dll, "retro_set_audio_sample")(
          [](int16_t, int16_t) {});
      Symbol<void (*)(size_t (*)(const int16_t*, size_t))>(dll, "retro_set_audio_sample_batch")(
          [](const int16_t*, size_t frames) { return frames; });
      Symbol<void (*)(void (*)())>(dll, "retro_set_input_poll")([] {});
      Symbol<void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned))>(dll, "retro_set_input_state")(
          [](unsigned, unsigned, unsigned, unsigned) -> int16_t { return 0; });
      init();
      initialized = true;
      const bool installed = InstallPs3PackageBatch(
          packages_, [&](const std::string& package) {
            const GameInfo info{package.c_str(), nullptr, 0, nullptr};
            const bool accepted = load(&info);
            package_rejected_ = !accepted;
            return accepted;
          }, unload, [&] { return stop_.load(); }, [&](size_t count) {
            package_completed_ = static_cast<int32_t>(count);
          });
      if (!installed)
        throw std::runtime_error(stop_ ? "PS3 update installation was cancelled."
                                       : "RPCS3 rejected a package in the update chain.");
    } catch (const std::exception& failure) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      error_ = failure.what();
    }
    if (initialized) deinit();
    if (dll) FreeLibrary(dll);
    callbacks.Release();
    ended_ = true;
  }

  void Run() {
    using Clock = std::chrono::steady_clock;
    using VoidFunction = void (*)();
    HMODULE dll = nullptr;
    VoidFunction deinit = nullptr;
    VoidFunction unload = nullptr;
    bool initialized = false;
    bool loaded = false;
    CallbackLease callbacks(callback_slot_);
    try {
      callbacks.Acquire(this);
      hardware_available_ = !netplay_ && (ps3_ ? vulkan_.Load() : hardware_.Open());
      if (ps3_ && !hardware_available_)
        throw std::runtime_error("PS3 requires a working Vulkan driver.");
      const auto path = std::filesystem::u8path(core_);
      dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
      if (!dll) throw std::runtime_error("Could not load the configured emulator core");
      callbacks.Attach(dll);
      if (Symbol<unsigned (*)()>(dll, "retro_api_version")() != 1)
        throw std::runtime_error("Unsupported emulator core API");
      if (match_cpu_ == Ps1MatchCpu::kRecompiler &&
          !GetProcAddress(dll, "retro_anyps5_export_blank_card_checkpoint_v1"))
        throw std::runtime_error("The Recompiler match requires the blank-card checkpoint core.");
      if (ps3_) SelectPs3NetworkPolicy(dll, online_, online_.has_value());
      using ReadPs3ProviderStatus = int32_t (*)();
      ReadPs3ProviderStatus read_provider_status = nullptr;
      if (online_) {
        const auto address = GetProcAddress(dll, "retro_anyps5_ps3_provider_status_v1");
        static_assert(sizeof(read_provider_status) == sizeof(address));
        std::memcpy(&read_provider_status, &address, sizeof(read_provider_status));
        if (read_provider_status) ps3_provider_status_ = 0;
      }
      const auto init = Symbol<VoidFunction>(dll, "retro_init");
      deinit = Symbol<VoidFunction>(dll, "retro_deinit");
      unload = Symbol<VoidFunction>(dll, "retro_unload_game");
      const auto load = Symbol<bool (*)(const GameInfo*)>(dll, "retro_load_game");
      const auto step = Symbol<VoidFunction>(dll, "retro_run");
      const auto av_info = Symbol<void (*)(AvInfo*)>(dll, "retro_get_system_av_info");
      const auto bind_lan = lan_
          ? reinterpret_cast<Ps2VirtualLan::Bind>(GetProcAddress(dll, "retro_anyps5_dev9_bind_v1"))
          : nullptr;
      if (lan_ && !bind_lan) throw std::runtime_error("This PS2 core does not support the private LAN adapter.");
      if (callback_slot_ == CallbackSlot::kPresentation) {
        BindGameplayCallbacks<1>(dll);
      } else {
        BindGameplayCallbacks<0>(dll);
      }
      const GameInfo game{disc_.c_str(), nullptr, 0, nullptr};
      const auto initial_disk_callbacks = disk_callbacks_;
      const auto reopen = [&] {
        lan_ready_ = false;
        if (loaded) unload();
        loaded = false;
        if (lan_ && !lan_->Detach()) throw std::runtime_error("Could not detach the private PS2 network.");
        if (initialized) deinit();
        hardware_.Clear();
        hardware_callbacks_ = {};
        disk_callbacks_ = initial_disk_callbacks;
        init();
        initialized = true;
        if (ps3_ && hardware_callbacks_.context_type != kVulkan)
          throw std::runtime_error("The PS3 core did not request a Vulkan context.");
        if (ps3_) {
          const auto controller = Symbol<void (*)(unsigned, unsigned)>(dll, "retro_set_controller_port_device");
          controller(0, 1);
          for (unsigned port = 1; port < 7; ++port) controller(port, 0);
        }
        if (lan_ && !lan_->Attach(bind_lan)) throw std::runtime_error("Could not attach the private PS2 network.");
        const auto& initial_disc = boot_disc_;
        if (initial_disc &&
            (!disk_callbacks_.set_initial_image ||
             !disk_callbacks_.set_initial_image(initial_disc->index, initial_disc->path.c_str()))) {
          throw std::runtime_error("The core cannot boot the selected disc.");
        }
        loaded = load(&game);
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
        lan_ready_ = lan_ && loaded;
        if (loaded && !netplay_ && ps1_) {
          ports_changed_ = false;
          if (const auto controller = Symbol<void (*)(unsigned, unsigned)>(dll, "retro_set_controller_port_device", false))
            controller(1, SecondPortDevice());
        }
        if (loaded && netplay_) {
          const auto controller = Symbol<void (*)(unsigned, unsigned)>(dll, "retro_set_controller_port_device");
          const unsigned device = netplay_dualshock_ ? kPs1DualShockDevice : 1u;
          for (unsigned port = 0; port < 2; ++port) controller(port, device);
        }
        if (loaded && ps3_) {
          if (!vulkan_.Open()) throw std::runtime_error("The PS3 Vulkan device could not be initialized.");
        }
        if (loaded && hardware_callbacks_.reset) hardware_callbacks_.reset();
        if (loaded && ps1_ && !netplay_) RefreshDiscs();
        return loaded;
      };
      const bool ps2 = path.filename() == L"pcsx2_libretro.dll";
      const bool ps1 = path.filename() == L"swanstation_libretro.dll";
      const auto use_native_resolution = [&] {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        const auto scale = options_.find(ps2 ? "pcsx2_upscale_multiplier" : "swanstation_GPU_ResolutionScale");
        return scale != options_.end() && std::exchange(scale->second.selected, 0) != 0;
      };
      if (!reopen() && (returning_ || !use_native_resolution() || !reopen()))
        throw std::runtime_error("The core could not boot this disc. Check its BIOS and game files.");
      const auto save_blank = ps1 && !netplay_ ? reinterpret_cast<SavePortable>(
          GetProcAddress(dll, "retro_anyps5_export_blank_card_checkpoint_v1")) : nullptr;
      const auto save_private = save_blank ? reinterpret_cast<SavePortable>(
          GetProcAddress(dll, "retro_anyps5_export_checkpoint_v1")) : nullptr;
      const auto load_private = save_blank ? reinterpret_cast<LoadPortable>(
          GetProcAddress(dll, "retro_anyps5_import_checkpoint_v1")) : nullptr;
      const size_t maximum = save_blank ? Symbol<size_t (*)()>(dll, "retro_serialize_size")() : 0;
      ordinary_recovery_supported_ = save_private && load_private;
      local_blank_checkpoint_supported_ = maximum && maximum <= kMaxPs1Checkpoint - kPs1CheckpointPrefix;
      void* card = nullptr;
      size_t card_size = 0;
      std::filesystem::path card_path;
      const auto refresh_card = [&] {
        if (!ps1 || netplay_) return;
        card = Symbol<void* (*)(unsigned)>(dll, "retro_get_memory_data")(0);
        card_size = Symbol<size_t (*)(unsigned)>(dll, "retro_get_memory_size")(0);
      };
      const auto restore_card = [&] {
        if (!ps1 || netplay_) return;
        refresh_card();
        card_path = std::filesystem::u8path(saves_) / std::filesystem::u8path(disc_).stem();
        card_path += L".srm";
        RestoreCard(card_path, card, card_size);
      };
      restore_card();
      std::shared_ptr<const Ps1OrdinaryRecovery> continuation_fallback;
      if (returning_) {
        {
          std::lock_guard<std::mutex> lock(settings_mutex_);
          for (const auto& [key, value] : returning_->options) {
            const auto option = options_.find(key);
            if (option == options_.end() || option->second.values[option->second.selected] != value)
              throw std::runtime_error("The original PS1 settings are unavailable.");
          }
        }
        if (!load_private || !load_private(returning_->state.data(), returning_->state.size(),
                                           returning_->digest.data()))
          throw std::runtime_error("The emulator could not recover the original PS1 game.");
        refresh_card();
        if (!returning_->continuation.empty() && card && card_size == 128 * 1024) {
          const auto* bytes = static_cast<const uint8_t*>(card);
          const CheckpointBytes original_card(bytes, bytes + card_size);
          const bool loaded_match = load_private(returning_->continuation.data(), returning_->continuation.size(),
                                                 returning_->digest.data());
          refresh_card();
          if (loaded_match && card && card_size == original_card.size()) {
            std::memcpy(card, original_card.data(), original_card.size());
            continued_from_match_ = true;
            continuation_fallback = returning_;
          } else {
            if (!load_private(returning_->state.data(), returning_->state.size(), returning_->digest.data()))
              throw std::runtime_error("The emulator could not restore the original PS1 game.");
            refresh_card();
          }
        }
        recovered_source_ = returning_->source;
        returning_.reset();
      }
      AvInfo av{};
      av_info(&av);
      SetAspect(av.geometry.aspect);
      const bool audio_open = !presentation_target_ && (audio_sink_ || audio_.Open(av.sample_rate));
      const double fps = std::isfinite(av.fps) && av.fps > 30 && av.fps <= 120 ? av.fps : 60.0;
      {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        target_fps_ = fps;
        audio_rate_ = audio_open ? av.sample_rate : 0;
      }
      const auto frame_time = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
      if (netplay_) {
        const auto memory = static_cast<const uint8_t*>(
            Symbol<void* (*)(unsigned)>(dll, "retro_get_memory_data")(2));
        const auto size = Symbol<size_t (*)(unsigned)>(dll, "retro_get_memory_size")(2);
        ValidateNetplay(memory, size);
        RunNetplay(dll, step, frame_time, av.sample_rate, memory, size);
      }
      auto next = Clock::now();
      auto hardware_deadline = next + std::chrono::seconds(8);
      Clock::time_point recovery_deadline{};
      while (!netplay_ && !stop_) {
        if (ps1 && !ordinary_parked_) {
          ApplyDiscChange();
          if (ports_changed_.exchange(false)) {
            if (const auto controller =
                    Symbol<void (*)(unsigned, unsigned)>(dll, "retro_set_controller_port_device", false))
              controller(1, SecondPortDevice());
          }
        }
        if (recovered_source() && !startup_visible_ && !paused_) {
          const auto now = Clock::now();
          if (recovery_deadline == Clock::time_point{}) recovery_deadline = now + std::chrono::seconds(8);
          if (now >= recovery_deadline) {
            if (!continuation_fallback ||
                !load_private(continuation_fallback->state.data(), continuation_fallback->state.size(),
                              continuation_fallback->digest.data()))
              throw std::runtime_error("The restored PS1 game did not present a visible frame.");
            continuation_fallback.reset();
            refresh_card();
            audio_.Flush();
            continued_from_match_ = false;
            recovery_deadline = now + std::chrono::seconds(8);
          }
        } else {
          if (startup_visible_) continuation_fallback.reset();
          recovery_deadline = {};
        }
        if ((ps1 || ps2) && !ordinary_parked_)
          ApplyCheckpointGraphics(dll, reopen, step, ps1, card, card_size, card_path);
        if (!ps3_ && !recovered_source() && hardware_callbacks_.reset && !frame_ready_ &&
            Clock::now() > hardware_deadline) {
          if (!use_native_resolution()) {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            auto& renderer = options_.at(ps2 ? "pcsx2_renderer" : "swanstation_GPU_Renderer");
            const auto software = std::find(renderer.values.begin(), renderer.values.end(),
                                            ps2 ? "Software (HW)" : "Software");
            if (software == renderer.values.end())
              throw std::runtime_error("The core could not present hardware frames.");
            renderer.selected = static_cast<size_t>(software - renderer.values.begin());
          }
          audio_.Flush();
          if (card) StoreCard(card_path, card, card_size);
          if (!reopen()) throw std::runtime_error("The core could not restart after a graphics startup failure.");
          restore_card();
          hardware_deadline = Clock::now() + std::chrono::seconds(8);
        }
        if (local_blank_checkpoint_supported_ &&
            ServiceCheckpoint(save_blank, load_private, maximum + kPs1CheckpointPrefix, step, save_private))
          refresh_card();
        if ((paused_ && (frame_ready_ || recovered_source())) || ordinary_parked_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          next = Clock::now();
          continue;
        }
        step();
        if (read_provider_status) ps3_provider_status_ = read_provider_status();
        if (!paused_) ++steps_;
        if (ps3_ && vulkan_.unsupported_sync())
          throw std::runtime_error("This RPCS3 core uses Vulkan synchronization AnyPS5 does not support.");
        next += frame_time;
        const auto now = Clock::now();
        if (next < now - frame_time) next = now;
        std::this_thread::sleep_until(next);
      }
      if (card && recovery_accepted_) StoreCard(card_path, card, card_size);
    } catch (const std::exception& failure) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      error_ = failure.what();
    }
    lan_ready_ = false;
    if (lan_) lan_->Close();
    if (loaded) unload();
    const bool lan_detached = !lan_ || lan_->Detach();
    if (ps3_ && vulkan_.negotiated() && hardware_callbacks_.destroy) hardware_callbacks_.destroy();
    if (ps3_) vulkan_.Close();
    if (initialized) deinit();
    if (!lan_detached && !lan_->Detach()) {
      std::lock_guard<std::mutex> lock(settings_mutex_);
      if (error_.empty()) error_ = "The PS2 core did not release the private LAN adapter.";
    }
    audio_.Close();
    if (dll) FreeLibrary(dll);
    callbacks.Release();
    if (presentation_core_) presentation_core_->Stop();
    {
      std::lock_guard<std::mutex> lock(netplay_mutex_);
      ended_ = true;
      presentation_capture_.reset();
      presentation_render_.reset();
    }
  }

  static inline std::array<std::atomic<RetroCoreSession*>, 2> active_{nullptr, nullptr};
  static inline std::mutex module_mutex_;
  static inline std::array<HMODULE, 2> modules_{};
  anyps5::retro_host::Surface* registrar_;
  const std::string core_, disc_;
  const std::unique_ptr<ps1_netplay::Timeline> netplay_;
  const bool rollback_;
  const bool netplay_host_;
  const AudioSink audio_sink_;
  const std::vector<ps1_netplay::PadInput> input_trace_;
  const bool detailed_step_trace_;
  const CallbackSlot callback_slot_;
  std::unique_ptr<RetroCoreSession> presentation_core_;
  RetroCoreSession* presentation_target_ = nullptr;
  bool rendering_on_target_ = false;
  std::atomic<uint32_t> presentation_steps_{0};
  std::atomic<uint32_t> presentation_frames_{0};
  std::atomic<uint32_t> peak_prediction_depth_{0};
  std::atomic<int64_t> rollback_audio_drained_{-1};
  std::atomic<size_t> audio_underruns_{0};
  std::atomic<size_t> audio_queue_peak_{0};
  std::atomic<int64_t> trace_max_work_us_{0};
  std::atomic<int64_t> trace_work_overruns_{0};
  mutable std::mutex trace_breakdown_mutex_;
  TraceBreakdown trace_breakdown_;
  TraceStep trace_step_;
  static inline thread_local std::chrono::steady_clock::duration* trace_video_time_ = nullptr;
  std::atomic<int64_t> trace_blocked_polls_{0};
  std::atomic<int64_t> trace_blocked_us_{0};
  std::atomic<int64_t> trace_late_wakes_{0};
  std::atomic<int64_t> trace_peak_wake_us_{0};
  static inline thread_local RollbackAudioFrame* rollback_audio_capture_ = nullptr;
  const std::unique_ptr<Ps1MatchStorage> match_storage_;
  const std::unique_ptr<Ps2VirtualLan> lan_;
  std::atomic<bool> lan_ready_{false};
  const std::string system_, saves_, content_;
  const std::string initial_resolution_;
  const std::map<std::string, std::string> initial_graphics_;
  const std::optional<Ps3NetworkPolicy> online_;
  std::atomic<int32_t> ps3_provider_status_{-1};
  const std::vector<std::string> packages_;
  std::atomic<int32_t> package_completed_{0};
  std::atomic<bool> package_rejected_{false};
  const bool ps1_;
  std::optional<InitialDisc> boot_disc_;
  DiskControlExtCallbacks disk_callbacks_{};
  std::optional<unsigned> pending_disc_;
  std::optional<DiscStatus> discs_;
  int64_t disc_inserted_at_step_ = 0;
  const bool ps3_;
  const bool local_staging_;
  const Ps1MatchCpu match_cpu_;
  const bool netplay_dualshock_;
  std::shared_ptr<const Ps1OrdinaryRecovery> returning_;
  std::atomic<bool> recovery_accepted_;
  std::atomic<int64_t> recovered_source_{0};
  std::atomic<bool> continued_from_match_{false};
  int64_t texture_id_ = -1;
  std::thread worker_;
  StereoOutput audio_;
  HardwareVideo hardware_;
  RetroVulkanVideo vulkan_;
  HardwareCallbacks hardware_callbacks_{};
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
  std::atomic<bool> startup_visible_{false};
  std::atomic<int> pixel_format_{0};
  std::atomic<float> aspect_{4.f / 3.f};
  std::mutex frame_mutex_, input_mutex_, settings_mutex_;
  std::mutex netplay_mutex_;
  std::mutex checkpoint_mutex_;
  std::atomic<bool> checkpoint_supported_{false};
  std::atomic<bool> local_blank_checkpoint_supported_{false};
  // Initialized before publishing local_blank_checkpoint_supported_.
  bool ordinary_recovery_supported_ = false;
  inline static std::atomic<int64_t> next_checkpoint_source_{0};
  const int64_t local_checkpoint_source_ = ++next_checkpoint_source_;
  std::atomic<bool> ordinary_parked_{false};
  std::atomic<bool> ordinary_settings_frozen_{false};
  CheckpointBytes ordinary_recovery_;
  Ps1DiscHash ordinary_recovery_digest_{};
  std::optional<CheckpointJob> checkpoint_;
  std::optional<uint32_t> replay_cursor_;
  std::deque<Ps1Ports> checkpoint_history_;
  std::atomic<bool> netplay_ready_{false};
  std::atomic<bool> confirmed_return_requested_{false};
  bool netplay_started_ = false;
  bool netplay_held_ = false;
  bool netplay_epoch_reset_ = false;
  bool local_prepared_ = false;
  uint32_t local_frame_ = 0;
  std::array<ps1_netplay::PadInput, 2> netplay_ports_{};
  std::vector<std::pair<uint32_t, std::string>> netplay_fingerprints_;
  std::optional<std::pair<uint32_t, std::string>> netplay_pending_fingerprint_;
  std::atomic<RollbackContext> rollback_context_{RollbackContext::kInactive};
  std::atomic<bool> rollback_context_queried_{false};
  bool presentation_only_ = false;
  bool presentation_busy_ = false;
  std::condition_variable presentation_ready_;
  std::optional<uint32_t> presentation_cursor_;
  std::optional<std::promise<std::optional<PresentationState>>> presentation_capture_;
  std::optional<PresentationRender> presentation_render_;
  std::shared_ptr<Frame> frame_;
  static constexpr unsigned kLocalPorts = 4;
  static constexpr unsigned kNoDevice = 0;
  std::atomic<unsigned> connected_ports_{2};
  std::atomic<bool> ports_changed_{false};
  std::array<uint16_t, kLocalPorts> buttons_{};
  std::array<std::array<int16_t, 4>, kLocalPorts> axes_{};
  std::map<std::string, Option> options_;
  bool options_changed_ = false;
  bool graphics_queued_ = false;
  bool graphics_applying_ = false;
  std::atomic<bool> suppress_frame_{false};
  std::chrono::steady_clock::time_point progress_until_{};
  std::string progress_label_;
  int32_t progress_percent_ = -1;
  std::string error_, resolution_error_;
};

namespace anyps5::retro_host {
namespace {
using ::IntegerValue;
using ::StringField;

class ProbeSink {
 public:
  RetroCoreSession::AudioSink Audio() {
    return [this](const int16_t* samples, size_t frames) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (size_t i = 0; i < frames * 2; ++i) {
        hash_ ^= static_cast<uint16_t>(samples[i]);
        hash_ *= 1099511628211ull;
      }
      frames_ += frames;
    };
  }
  std::pair<uint64_t, uint64_t> Read() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {frames_, hash_};
  }

 private:
  std::mutex mutex_;
  uint64_t frames_ = 0, hash_ = 14695981039346656037ull;
};

class RetroCoreHost final : public Backend {
 public:
  RetroCoreHost(Surface& textures, const HostOptions& options) : textures_(&textures), probe_(options.probe) {}
  ~RetroCoreHost() override { Shutdown(); }

  void Handle(const MethodCall& call, std::unique_ptr<MethodResult> result) override;

  void Presented(bool visible) override {
    if (session_) session_->MarkPresented(visible);
  }

  bool HasSession() const override { return session_ != nullptr; }

  void Shutdown() override {
    if (!session_) return;
    session_->Stop();
    Release();
  }

 private:
  void Release();
  bool HandleProbe(const std::string& method, const Value* arguments, MethodResult& result);

  Surface* textures_;
  const bool probe_;
  std::shared_ptr<RetroCoreSession> session_;
  std::shared_ptr<Ps1OrdinaryRecovery> ordinary_recovery_;
  std::optional<int64_t> ps1_return_request_;
  std::unique_ptr<ProbeSink> probe_sink_;
};

void RetroCoreHost::Release() {
  ps1_return_request_.reset();
  auto session = std::move(session_);
  session->Join();
  const auto id = session->texture_id();
  if (id >= 0) textures_->Release(id);
}

void RetroCoreHost::Handle(const MethodCall& call, std::unique_ptr<MethodResult> result) {
  const auto& method = call.method_name();
  const auto* arguments = call.arguments();
  if (probe_ && HandleProbe(method, arguments, *result)) return;
  if (method == "installPs3Packages") {
    if (session_)
      return result->Error("busy", "An emulator operation is already running.");
    const auto* fields = std::get_if<Map>(arguments);
    const auto* core = fields ? StringField(*fields, "core") : nullptr;
    const auto* stage = fields ? StringField(*fields, "stage") : nullptr;
    const auto count_field = fields ? fields->find(Value("count")) : Map::const_iterator{};
    const auto count = fields && count_field != fields->end() ? IntegerValue(count_field->second) : std::nullopt;
    if (!core || !stage || core->find('\0') != std::string::npos ||
        stage->find('\0') != std::string::npos || !count || *count < 1 || *count > 64)
      return result->Error("argument", "An RPCS3 core, staging profile and update chain are required.");
    try {
      const auto core_path = std::filesystem::u8path(*core);
      const auto root = std::filesystem::u8path(*stage);
      const auto system = root / L"system";
      const auto saves = root / L"saves";
      const auto input = root / L"packages";
      const auto ordinary = [](const std::filesystem::path& path) {
        const auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
      };
      if (!core_path.is_absolute() || core_path.filename() != L"rpcs3_libretro.dll" ||
          !root.is_absolute() || !ordinary(root) || !ordinary(input) || !std::filesystem::is_directory(input) ||
          !ordinary(system) || !ordinary(saves) ||
          !std::filesystem::is_directory(system) || !std::filesystem::is_empty(system) ||
          !std::filesystem::is_directory(saves) || !std::filesystem::is_empty(saves))
        return result->Error("argument", "PS3 updates require a fresh staging profile.");
      std::vector<std::string> paths;
      for (int64_t index = 0; index < *count; ++index) {
        const auto path = input / (std::to_wstring(index) + L".pkg");
        if (!ordinary(path) || !std::filesystem::is_regular_file(path) ||
            std::filesystem::hard_link_count(path) != 1)
          return result->Error("argument", "A staged update package is missing or is a link.");
        paths.push_back(path.u8string());
      }
      const auto files = std::distance(std::filesystem::directory_iterator(input),
                                       std::filesystem::directory_iterator());
      if (files != *count)
        return result->Error("argument", "The staged update chain contains extra files.");
      session_ = std::make_shared<RetroCoreSession>(
          textures_, *core, "", system.u8string(), saves.u8string(), "",
          std::map<std::string, std::string>{}, std::nullopt, "", false, std::nullopt,
          std::nullopt, std::move(paths));
      session_->Start();
      result->Success(Value(true));
    } catch (const std::exception&) {
      if (session_) Release();
      result->Error("install", "Could not start PS3 update installation.");
    }
    return;
  }
  if (session_ && session_->installing_packages()) {
    if (method == "ps3PackageStatus") {
      result->Success(Value(session_->PackageStatus()));
    } else if (method == "stop") {
      session_->Stop();
      result->Success();
    } else if (method == "release" && session_->ended()) {
      Release();
      result->Success();
    } else {
      result->Error("busy", "PS3 update installation is using the emulator.");
    }
    return;
  }
  if (method == "ps1LocalCheckpointReopen" || method == "ps1LocalCheckpointDiscard") {
    const auto source = arguments ? IntegerValue(*arguments) : std::nullopt;
    if (!ordinary_recovery_ || source != ordinary_recovery_->source)
      return result->Error("checkpoint", "The private PS1 recovery checkpoint is unavailable.");
    if (session_ && session_->ended()) Release();
    if (session_) return result->Error("busy", "An embedded game is already running.");
    if (method == "ps1LocalCheckpointDiscard") {
      ordinary_recovery_.reset();
      return result->Success(Value(true));
    }
    try {
      const auto& recovery = *ordinary_recovery_;
      session_ = std::make_shared<RetroCoreSession>(
          textures_, recovery.core, recovery.disc, recovery.system, recovery.saves, "", recovery.options,
          std::nullopt, "", false, std::nullopt, std::nullopt, std::vector<std::string>{},
          Ps1MatchCpu::kCachedInterpreter, ordinary_recovery_);
      if (!session_->Start()) throw std::runtime_error("Could not register the original game surface.");
      return result->Success(Value(session_->texture_id()));
    } catch (const std::exception& failure) {
      if (session_) Release();
      return result->Error("checkpoint", failure.what());
    }
  }
  if (method == "start") {
    if (session_ && session_->ended()) Release();
    if (session_) {
      result->Error("busy", "An embedded game is already running");
      return;
    }
    const auto* fields = std::get_if<Map>(arguments);
    const auto* core = fields ? StringField(*fields, "core") : nullptr;
    const auto* disc = fields ? StringField(*fields, "disc") : nullptr;
    const auto* system = fields ? StringField(*fields, "system") : nullptr;
    const auto* saves = fields ? StringField(*fields, "saves") : nullptr;
    const auto* resolution = fields ? StringField(*fields, "internalResolution") : nullptr;
    if (!core || !disc || !system || !saves) {
      result->Error("argument", "The core, disc and storage paths are required");
      return;
    }
    try {
      std::optional<unsigned> netplay_seat;
      std::optional<Ps2VirtualLan::Mac> lan_mac;
      const auto seat = fields->find(Value("ps1NetplaySeat"));
      const auto* netplay_bios = StringField(*fields, "ps1NetplayBios");
      const auto local = fields->find(Value("ps1LocalStaging"));
      const auto* local_staging = local == fields->end() ? nullptr : std::get_if<bool>(&local->second);
      if (local != fields->end() && !local_staging) {
        result->Error("argument", "PS1 local staging must be a boolean");
        return;
      }
      if (seat != fields->end()) {
        const auto number = IntegerValue(seat->second);
        if (!number || *number < 0 || *number > 1 || !netplay_bios ||
            std::filesystem::u8path(*core).filename() != L"swanstation_libretro.dll" ||
            _wcsicmp(std::filesystem::u8path(*disc).extension().c_str(), L".cue") != 0) {
          result->Error("argument", "PS1 netplay requires a valid seat, SwanStation, a CUE disc and a verified BIOS");
          return;
        }
        netplay_seat = static_cast<unsigned>(*number);
      }
      auto match_cpu = Ps1MatchCpu::kCachedInterpreter;
      if (const auto cpu = fields->find(Value("ps1NetplayCpu")); cpu != fields->end()) {
        const auto* name = std::get_if<std::string>(&cpu->second);
        if (!netplay_seat || (local_staging && *local_staging) || !name || *name != "Recompiler")
          return result->Error("argument", "The Recompiler profile requires an isolated PS1 match seat.");
        match_cpu = Ps1MatchCpu::kRecompiler;
      }
      bool netplay_dualshock = false;
      if (const auto pad = fields->find(Value("ps1NetplayDualShock")); pad != fields->end()) {
        const auto* enabled = std::get_if<bool>(&pad->second);
        if (!enabled || !netplay_seat || (local_staging && *local_staging) ||
            match_cpu != Ps1MatchCpu::kCachedInterpreter || fields->count(Value("ps1PresentationCore"))) {
          return result->Error("argument", "DualShock input requires a fresh PS1 match.");
        }
        netplay_dualshock = *enabled;
      }
      bool rollback = false;
      if (const auto field = fields->find(Value("ps1NetplayRollback")); field != fields->end()) {
        const auto* enabled = std::get_if<bool>(&field->second);
        if (!enabled || !netplay_seat || (local_staging && *local_staging) ||
            match_cpu != Ps1MatchCpu::kCachedInterpreter || netplay_dualshock ||
            fields->count(Value("ps1PresentationCore"))) {
          return result->Error("argument", "Single-core rollback requires an isolated digital PS1 match.");
        }
        rollback = *enabled;
      }
      const std::string* presentation = nullptr;
      if (const auto found = fields->find(Value("ps1PresentationCore")); found != fields->end()) {
        presentation = std::get_if<std::string>(&found->second);
        if (!netplay_seat || (local_staging && *local_staging) || !presentation ||
            presentation->find('\0') != std::string::npos) {
          return result->Error("argument", "Rollback requires a separate PS1 presentation core and an isolated seat.");
        }
        const auto path = std::filesystem::u8path(*presentation);
        const auto primary = std::filesystem::u8path(*core);
        std::error_code error;
        if (!path.is_absolute() || !primary.is_absolute() || path.filename() != primary.filename() ||
            std::filesystem::equivalent(path, primary, error) || error ||
            !std::filesystem::is_regular_file(path) || !std::filesystem::is_regular_file(primary)) {
          return result->Error("argument", "Rollback requires two separate SwanStation libraries.");
        }
      }
      if (const auto lan = fields->find(Value("ps2LanMac")); lan != fields->end()) {
        const auto* bytes = std::get_if<CheckpointBytes>(&lan->second);
        Ps2VirtualLan::Mac mac{};
        if (!bytes || bytes->size() != mac.size() || netplay_seat ||
            std::filesystem::u8path(*core).filename() != L"pcsx2_libretro.dll") {
          return result->Error("argument", "A PS2 LAN session requires LRPS2 and a six-byte virtual MAC.");
        }
        std::copy(bytes->begin(), bytes->end(), mac.begin());
        if (!Ps2VirtualLan::ValidMac(mac))
          return result->Error("argument", "The PS2 virtual MAC must be a nonzero unicast address.");
        lan_mac = mac;
      }
      std::optional<Ps3NetworkPolicy> online;
      if (const auto found = fields->find(Value("ps3Online")); found != fields->end()) {
        const auto* policy = std::get_if<Map>(&found->second);
        const auto* xfloat = policy ? StringField(*policy, "xfloat") : nullptr;
        const auto* dns = policy ? StringField(*policy, "dns") : nullptr;
        const auto upnp = policy ? policy->find(Value("upnp")) : Map::const_iterator{};
        if (!policy || !xfloat || !dns || upnp == policy->end() ||
            !std::holds_alternative<bool>(upnp->second) ||
            std::filesystem::u8path(*core).filename() != L"rpcs3_libretro.dll")
          throw std::runtime_error("Invalid PS3 online profile.");
        const auto protection = policy->find(Value("patchProtection"));
        const auto identity = policy->find(Value("rpcnIdentity"));
        if (policy->size() != 5 || protection == policy->end() || identity == policy->end())
          throw std::runtime_error("Invalid PS3 online profile.");
        const auto* endpoint = std::get_if<Map>(&identity->second);
        const auto* host = endpoint ? StringField(*endpoint, "host") : nullptr;
        const auto* certificate = endpoint ? StringField(*endpoint, "certificateSha256") : nullptr;
        if (!endpoint || endpoint->size() != 2 || !host || !certificate ||
            host->find('\0') != std::string::npos || certificate->find('\0') != std::string::npos)
          throw std::runtime_error("Invalid PS3 provider identity.");
        online = Ps3NetworkPolicy{*xfloat, *dns, std::get<bool>(upnp->second), *host, *certificate, {}};
        {
          const auto* patch = std::get_if<Map>(&protection->second);
          const auto* title = patch ? StringField(*patch, "titleId") : nullptr;
          const auto* version = patch ? StringField(*patch, "updateVersion") : nullptr;
          const auto* digest = patch ? StringField(*patch, "sha256") : nullptr;
          const auto* ppu_hash = patch ? StringField(*patch, "ppuHash") : nullptr;
          const auto required = patch ? patch->find(Value("patches")) : Map::const_iterator{};
          const auto* entries = patch && required != patch->end() ? std::get_if<List>(&required->second) : nullptr;
          if (!patch || patch->size() != 5 || !title || !version || !digest || !ppu_hash ||
              !entries || entries->empty() || entries->size() > 8)
            throw std::runtime_error("Invalid PS3 online protection.");

          auto& parsed = online->protection;
          parsed = {*title, *version, *digest, *ppu_hash, {}, {}};
          for (const auto& entry : *entries) {
            const auto* item = std::get_if<Map>(&entry);
            const auto* name = item ? StringField(*item, "name") : nullptr;
            const auto writes = item ? item->find(Value("writes")) : Map::const_iterator{};
            const auto count = item && writes != item->end() ? IntegerValue(writes->second) : std::nullopt;
            if (!item || item->size() != 3 || !name || !StringField(*item, "option") ||
                !count || *count < 1 || *count > 4096)
              throw std::runtime_error("Invalid PS3 online protection.");
            parsed.names.push_back(*name);
            parsed.writes.push_back(static_cast<unsigned>(*count));
          }
        }
      }
      std::map<std::string, std::string> graphics;
      const auto values = fields->find(Value("graphics"));
      if (values != fields->end()) {
        if (const auto* saved = std::get_if<Map>(&values->second)) {
          for (const auto& entry : *saved) {
            const auto* key = std::get_if<std::string>(&entry.first);
            const auto* value = std::get_if<std::string>(&entry.second);
            if (key && value) graphics.emplace(*key, *value);
          }
        }
      }
      std::optional<InitialDisc> boot_disc;
      const auto initial_index = fields->find(Value("initialDiscIndex"));
      const auto* initial_path = StringField(*fields, "initialDiscPath");
      if (initial_index != fields->end() || fields->count(Value("initialDiscPath"))) {
        const auto index = initial_index == fields->end() ? std::nullopt : IntegerValue(initial_index->second);
        if (netplay_seat || lan_mac || online || std::filesystem::u8path(*core).filename() != L"swanstation_libretro.dll" ||
            !index || *index < 0 || *index >= 128 || !initial_path || initial_path->find('\0') != std::string::npos ||
            !std::filesystem::u8path(*initial_path).is_absolute()) {
          throw std::runtime_error("Invalid initial PS1 disc.");
        }
        boot_disc = InitialDisc{static_cast<unsigned>(*index), *initial_path};
      }
      RetroCoreSession::AudioSink audio_sink;
      std::vector<ps1_netplay::PadInput> input_trace;
      probe_sink_.reset();
      if (probe_ && netplay_seat) {
        probe_sink_ = std::make_unique<ProbeSink>();
        audio_sink = probe_sink_->Audio();
        if (const auto trace = fields->find(Value("probeInputTrace")); trace != fields->end()) {
          const auto* masks = std::get_if<List>(&trace->second);
          if (!masks) return result->Error("argument", "Invalid probe input trace.");
          for (const auto& mask : *masks) {
            const auto buttons = IntegerValue(mask);
            if (!buttons || *buttons < 0 || *buttons > UINT16_MAX)
              return result->Error("argument", "Invalid probe input trace.");
            input_trace.push_back({static_cast<uint16_t>(*buttons), {}});
          }
        }
      }
      session_ = std::make_shared<RetroCoreSession>(textures_, *core, *disc, *system, *saves,
                                                   resolution ? *resolution : "", std::move(graphics),
                                                   netplay_seat, netplay_bios ? *netplay_bios : "",
                                                   local_staging && *local_staging, lan_mac, std::move(online),
                                                   std::vector<std::string>{}, match_cpu, nullptr,
                                                   rollback || presentation != nullptr, std::move(audio_sink),
                                                   std::move(input_trace), RetroCoreSession::CallbackSlot::kPrimary,
                                                   std::move(boot_disc), netplay_dualshock);
      if (presentation) {
        auto spare = std::make_unique<RetroCoreSession>(
            textures_, *presentation, *disc, *system, *saves, std::string{},
            std::map<std::string, std::string>{}, netplay_seat, *netplay_bios, false, std::nullopt, std::nullopt,
            std::vector<std::string>{}, match_cpu, nullptr, true, RetroCoreSession::AudioSink{},
            std::vector<ps1_netplay::PadInput>{}, RetroCoreSession::CallbackSlot::kPresentation);
        if (!session_->AttachPresentationCore(std::move(spare)))
          throw std::runtime_error("Could not attach the PS1 rollback presentation core.");
      }
      if (!session_->Start()) throw std::runtime_error("Could not register the game surface");
      result->Success(Value(session_->texture_id()));
    } catch (const std::exception& failure) {
      if (session_) {
        session_->Stop();
        Release();
      }
      result->Error("start", failure.what());
    }
    return;
  }
  if (method == "ps1LocalCheckpointAccept") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!ordinary_recovery_ || !session_ || !fields)
      return result->Error("checkpoint", "The private PS1 recovery checkpoint is unavailable.");
    const auto texture = fields->find(Value("texture"));
    const auto source = fields->find(Value("source"));
    if (texture == fields->end() || source == fields->end() ||
        IntegerValue(texture->second) != session_->texture_id() ||
        IntegerValue(source->second) != ordinary_recovery_->source ||
        !session_->AcceptOrdinaryRecovery(ordinary_recovery_->source))
      return result->Error("checkpoint", "The original PS1 game is not ready to resume.");
    ordinary_recovery_.reset();
    return result->Success(Value(true));
  }
  if (!session_) {
    result->Success();
    return;
  }
  if (method == "status") {
    result->Success(Value(session_->Status()));
  } else if (method == "ps1LocalCheckpointContinue") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields || !ordinary_recovery_ || ps1_return_request_ || !ordinary_recovery_->continuation.empty())
      return result->Error("checkpoint", "The original PS1 session is unavailable.");
    const auto texture = fields->find(Value("texture"));
    const auto source = fields->find(Value("source"));
    if (texture == fields->end() || IntegerValue(texture->second) != session_->texture_id() ||
        source == fields->end() || IntegerValue(source->second) != ordinary_recovery_->source)
      return result->Error("checkpoint", "The PS1 recovery session changed.");
    const auto request = session_->RequestConfirmedReturn(*ordinary_recovery_);
    if (!request) return result->Error("checkpoint", "The PS1 match cannot be continued.");
    ps1_return_request_ = request;
    result->Success(Value(*request));
  } else if (method == "ps1LocalCheckpointDetach") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) return result->Error("checkpoint", "Expected the parked PS1 session.");
    const auto texture = fields->find(Value("texture"));
    const auto source = fields->find(Value("source"));
    if (ordinary_recovery_ || texture == fields->end() ||
        IntegerValue(texture->second) != session_->texture_id() || source == fields->end())
      return result->Error("checkpoint", "The parked PS1 session changed or recovery is already pending.");
    const auto token = IntegerValue(source->second);
    if (!token) return result->Error("checkpoint", "Expected a PS1 session token.");
    ordinary_recovery_ = session_->DetachOrdinaryRecovery(*token);
    if (!ordinary_recovery_) return result->Error("checkpoint", "The original PS1 game is not parked.");
    Release();
    result->Success(Value(*token));
  } else if (method == "ps2LanReset") {
    const auto texture = arguments ? IntegerValue(*arguments) : std::nullopt;
    result->Success(Value(texture == session_->texture_id() && session_->ClearLanFrames()));
  } else if (method == "ps2LanExchange") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) return result->Error("argument", "Expected a PS2 LAN frame batch.");
    const auto token = fields->find(Value("texture"));
    const auto texture = token == fields->end() ? std::nullopt : IntegerValue(token->second);
    if (texture != session_->texture_id()) return result->Success();
    const auto input = fields->find(Value("frames"));
    const auto* frames = input == fields->end() ? nullptr : std::get_if<List>(&input->second);
    if (!frames) return result->Error("argument", "Expected a PS2 LAN frame list.");
    auto output = session_->ExchangeLan(*frames);
    result->Success(output ? Value(std::move(*output)) : Value());
  } else if (method == "netplayStart") {
    result->Success(Value(session_->StartNetplay()));
  } else if (method == "ps1CheckpointHold") {
    result->Success(FrameValue(session_->HoldNetplay()));
  } else if (method == "ps1ResumeLocal") {
    result->Success(Value(session_->ResumeLocalNetplay()));
  } else if (method == "ps1CheckpointExport" || method == "ps1CheckpointImport" ||
             method == "ps1LocalCheckpointExport" || method == "ps1LocalCheckpointPark" ||
             method == "ps1LocalCheckpointResume") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) return result->Error("checkpoint", "Expected checkpoint arguments");
    std::optional<int64_t> source;
    if (method != "ps1CheckpointExport" && method != "ps1CheckpointImport") {
      const auto texture_field = fields->find(Value("texture"));
      if (texture_field == fields->end() || IntegerValue(texture_field->second) != session_->texture_id())
        return result->Error("checkpoint", "The local PS1 session changed");
      const auto source_field = fields->find(Value("source"));
      if (source_field != fields->end()) source = IntegerValue(source_field->second);
      if (!source) return result->Error("checkpoint", "Expected the local PS1 checkpoint source");
      if (method == "ps1LocalCheckpointResume") {
        const auto id = session_->RequestOrdinaryCheckpointResume(*source);
        if (!id) return result->Error("checkpoint", "The local PS1 checkpoint is unavailable");
        return result->Success(Value(*id));
      }
    }
    const auto digest_field = fields->find(Value("discHash"));
    const auto* digest = digest_field == fields->end() ? nullptr
                                                       : std::get_if<CheckpointBytes>(&digest_field->second);
    if (!digest || digest->size() != Ps1DiscHash{}.size())
      return result->Error("checkpoint", "Expected a 32-byte disc hash");
    Ps1DiscHash hash{};
    std::copy(digest->begin(), digest->end(), hash.begin());
    CheckpointBytes restore;
    std::optional<uint32_t> frame;
    if (method == "ps1CheckpointImport") {
      const auto state_field = fields->find(Value("state"));
      const auto* state = state_field == fields->end() ? nullptr
                                                       : std::get_if<CheckpointBytes>(&state_field->second);
      if (!state || state->empty() || state->size() > kMaxPs1Checkpoint)
        return result->Error("checkpoint", "Invalid portable checkpoint size");
      const auto frame_field = fields->find(Value("frame"));
      const auto number = frame_field == fields->end() ? std::nullopt : IntegerValue(frame_field->second);
      if (!number || *number < 0 || *number > ps1_netplay::Timeline::kMaxFrame)
        return result->Error("checkpoint", "Invalid portable checkpoint frame");
      frame = static_cast<uint32_t>(*number);
      restore = *state;
    }
    const auto id = source
                        ? session_->RequestBlankCardCheckpoint(*source, hash, method == "ps1LocalCheckpointPark")
                        : session_->RequestCheckpoint(hash, std::move(restore), frame);
    if (!id) return result->Error("checkpoint", "This core cannot run that checkpoint operation now");
    result->Success(Value(*id));
  } else if (method == "ps1CheckpointHistory") {
    const auto first = arguments ? IntegerValue(*arguments) : std::nullopt;
    if (!first || *first < 0 || *first > ps1_netplay::Timeline::kMaxFrame)
      return result->Error("checkpoint", "Invalid first history frame");
    auto history = session_->CheckpointHistory(static_cast<uint32_t>(*first));
    if (!history) return result->Error("checkpoint", "The requested input history is unavailable");
    result->Success(Value(std::move(*history)));
  } else if (method == "ps1CheckpointReplay") {
    const auto reject = [&] { result->Error("checkpoint", "Invalid checkpoint replay inputs"); };
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) return reject();
    const auto frame_field = fields->find(Value("frame"));
    const auto first = frame_field == fields->end() ? std::nullopt : IntegerValue(frame_field->second);
    const auto frames_field = fields->find(Value("frames"));
    const auto* frames = frames_field == fields->end() ? nullptr : std::get_if<List>(&frames_field->second);
    if (!first || *first < 0 || *first > ps1_netplay::Timeline::kMaxFrame ||
        !frames || frames->empty() || frames->size() > kPs1CatchupBatch)
      return reject();
    std::vector<Ps1Ports> batch;
    batch.reserve(frames->size());
    for (const auto& frame : *frames) {
      const auto* ports = std::get_if<List>(&frame);
      if (!ports || ports->size() != 2) return reject();
      Ps1Ports input{};
      for (size_t seat = 0; seat < input.size(); ++seat) {
        const auto* pad = std::get_if<List>(&(*ports)[seat]);
        if (!pad || pad->size() != 5) return reject();
        const auto buttons = IntegerValue(pad->front());
        if (!buttons || *buttons < 0 || *buttons > UINT16_MAX) return reject();
        input[seat].buttons = static_cast<uint16_t>(*buttons);
        for (size_t axis = 0; axis < input[seat].axes.size(); ++axis) {
          const auto value = IntegerValue((*pad)[axis + 1]);
          if (!value || *value < INT16_MIN || *value > INT16_MAX) return reject();
          input[seat].axes[axis] = static_cast<int16_t>(*value);
        }
      }
      batch.push_back(input);
    }
    const auto id = session_->RequestCatchup(static_cast<uint32_t>(*first), std::move(batch));
    if (!id) return result->Error("checkpoint", "The guest cannot replay from that frame");
    result->Success(Value(*id));
  } else if (method == "ps1CheckpointResult" || method == "ps1CheckpointCancel") {
    const auto id = arguments ? IntegerValue(*arguments) : std::nullopt;
    if (!id) return result->Error("checkpoint", "Expected a checkpoint request ID");
    if (method == "ps1CheckpointResult") {
      auto checkpoint = session_->TakeCheckpoint(*id);
      if (id == ps1_return_request_ && !std::holds_alternative<std::monostate>(checkpoint)) {
        ps1_return_request_.reset();
        auto* state = std::get_if<CheckpointBytes>(&checkpoint);
        const bool saved = ordinary_recovery_ && state && !state->empty();
        if (saved) ordinary_recovery_->continuation = std::move(*state);
        checkpoint = Value(saved);
      }
      result->Success(checkpoint);
    } else {
      session_->CancelCheckpoint(*id);
      if (id == ps1_return_request_) ps1_return_request_.reset();
      result->Success();
    }
  } else if (method == "netplaySync") {
    const auto fail = [&] { result->Error("netplayInput", "The PS1 netplay input exchange was rejected"); };
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) return fail();
    const auto remote = fields->find(Value("remote"));
    const auto* samples = remote == fields->end() ? nullptr : std::get_if<List>(&remote->second);
    if (!samples || samples->size() > 32) return fail();
    std::optional<uint32_t> acknowledged;
    const auto ack = fields->find(Value("acknowledgedFrame"));
    if (ack != fields->end() && !std::holds_alternative<std::monostate>(ack->second)) {
      const auto number = IntegerValue(ack->second);
      if (!number || *number < 0 || *number > ps1_netplay::Timeline::kMaxFrame) return fail();
      acknowledged = static_cast<uint32_t>(*number);
    }
    std::vector<ps1_netplay::FrameInput> inputs;
    inputs.reserve(samples->size());
    for (const auto& sample : *samples) {
      const auto* values = std::get_if<List>(&sample);
      if (!values || values->size() != 6) return fail();
      const auto frame = IntegerValue((*values)[0]);
      const auto buttons = IntegerValue((*values)[1]);
      if (!frame || *frame < 0 || *frame > ps1_netplay::Timeline::kMaxFrame ||
          !buttons || *buttons < 0 || *buttons > UINT16_MAX) return fail();
      ps1_netplay::PadInput input{static_cast<uint16_t>(*buttons), {}};
      for (size_t index = 0; index < input.axes.size(); ++index) {
        const auto axis = IntegerValue((*values)[index + 2]);
        if (!axis || *axis < INT16_MIN || *axis > INT16_MAX) return fail();
        input.axes[index] = static_cast<int16_t>(*axis);
      }
      inputs.push_back({static_cast<uint32_t>(*frame), input});
    }
    auto update = session_->SyncNetplay(inputs, acknowledged);
    if (!update) return fail();
    result->Success(Value(std::move(*update)));
  } else if (method == "stop") {
    session_->Stop();
    result->Success();
  } else if (method == "release") {
    if (session_->ended()) Release();
    result->Success();
  } else if (method == "pause") {
    const auto* pause = std::get_if<bool>(arguments);
    if (pause) session_->Pause(*pause);
    result->Success();
  } else if (method == "input") {
    const auto* input = std::get_if<List>(arguments);
    if (input && (input->size() == 5 || input->size() == 6)) {
      const size_t first = input->size() - 5;
      const auto* port = first ? std::get_if<int32_t>(&input->front()) : nullptr;
      std::array<int16_t, 4> axes{};
      for (size_t index = 0; index < axes.size(); ++index) {
        if (const auto* axis = std::get_if<int32_t>(&(*input)[first + index + 1])) {
          axes[index] = static_cast<int16_t>(std::clamp(*axis, -32768, 32767));
        }
      }
      const auto* mask = std::get_if<int32_t>(&(*input)[first]);
      if (mask && (!first || (port && *port >= 0 && *port < 4))) {
        session_->SetInput(port ? static_cast<unsigned>(*port) : 0u, static_cast<uint16_t>(*mask), axes);
      }
    }
    result->Success();
  } else if (method == "connectedPorts") {
    const auto count = arguments ? IntegerValue(*arguments) : std::nullopt;
    result->Success(Value(count && *count >= 1 && *count <= 4 && session_->ConnectPorts(static_cast<unsigned>(*count))));
  } else if (method == "changeDisc") {
    const auto index = arguments ? IntegerValue(*arguments) : std::nullopt;
    result->Success(Value(index && *index >= 0 && *index < 128 &&
                          session_->ChangeDisc(static_cast<unsigned>(*index))));
  } else if (method == "tuningOptions") {
    const auto* fields = std::get_if<Map>(arguments);
    if (!fields) {
      result->Success(Value(false));
      return;
    }
    const auto expected = fields->find(Value("expected"));
    const auto values = fields->find(Value("values"));
    const auto* expected_options = expected == fields->end() ? nullptr : std::get_if<Map>(&expected->second);
    const auto* candidate_options = values == fields->end() ? nullptr : std::get_if<Map>(&values->second);
    result->Success(Value(expected_options && candidate_options &&
                          session_->SetTuningOptions(*expected_options, *candidate_options)));
  } else if (method == "option") {
    const auto* fields = std::get_if<Map>(arguments);
    const auto* key = fields ? StringField(*fields, "key") : nullptr;
    const auto* value = fields ? StringField(*fields, "value") : nullptr;
    result->Success(Value(key && value && session_->SetOption(*key, *value)));
  } else {
    result->NotImplemented();
  }
}

bool RetroCoreHost::HandleProbe(const std::string& method, const Value* arguments, MethodResult& result) {
  if (method == "probeReadRam") {
    const auto* fields = std::get_if<Map>(arguments);
    const auto* core = fields ? StringField(*fields, "core") : nullptr;
    const auto offset = fields ? IntegerValue(fields->count(Value("offset")) ? fields->at(Value("offset")) : Value())
                               : std::nullopt;
    const auto count = fields ? IntegerValue(fields->count(Value("count")) ? fields->at(Value("count")) : Value())
                              : std::nullopt;
    constexpr int64_t kRam = 2 * 1024 * 1024;
    if (!core || !offset || !count || *count < 0 || *count > 256 || *offset < 0 || *offset > kRam ||
        *count > kRam - *offset) {
      result.Error("probe", "Invalid probe RAM read.");
      return true;
    }
    const auto dll = GetModuleHandleW(std::filesystem::u8path(*core).c_str());
    const auto* ram = dll ? static_cast<const uint8_t*>(Symbol<void* (*)(unsigned)>(dll, "retro_get_memory_data")(2))
                          : nullptr;
    if (!ram) {
      result.Error("probe", "PS1 main RAM is unavailable.");
      return true;
    }
    result.Success(Value(std::vector<uint8_t>(ram + *offset, ram + *offset + *count)));
    return true;
  }
  if (method == "probeStats") {
    if (!session_) {
      result.Success();
      return true;
    }
    const auto [frames, hash] = probe_sink_ ? probe_sink_->Read() : std::pair<uint64_t, uint64_t>{};
    const auto pacing = session_->trace_pacing();
    List timing;
    for (const auto value : pacing) timing.emplace_back(static_cast<int64_t>(value));
    result.Success(Value(Map{{Value("audioFrames"), Value(static_cast<int64_t>(frames))},
                             {Value("audioHash"), Value(std::to_string(hash))},
                             {Value("audioDrained"), Value(session_->rollback_audio_drained())},
                             {Value("pacing"), Value(std::move(timing))},
                             {Value("maxWorkUs"), Value(session_->trace_max_work_us())},
                             {Value("workOverruns"), Value(session_->trace_work_overruns())}}));
    return true;
  }
  return false;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend(Surface& surface, const HostOptions& options) {
  return std::make_unique<RetroCoreHost>(surface, options);
}

}  // namespace anyps5::retro_host
