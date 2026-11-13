// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "game.h"

#include <libretro.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <tuple>
#include <utility>

#include "resolution.h"

namespace retro_host {

struct Game::DiscControl {
  retro_disk_control_ext_callback callbacks{};
  bool extended = false;
};

namespace {

Game* g_game = nullptr;

bool OnEnvironment(unsigned command, void* data) { return g_game && g_game->Environment(command, data); }
void OnVideo(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (g_game) g_game->Video(data, width, height, pitch);
}
void OnAudioSample(int16_t left, int16_t right) {
  const int16_t frame[2] = {left, right};
  if (g_game) g_game->AudioBatch(frame, 1);
}
size_t OnAudioBatch(const int16_t* samples, size_t frames) { return g_game ? g_game->AudioBatch(samples, frames) : frames; }
void OnInputPoll() {}
int16_t OnInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  return g_game ? g_game->InputState(port, device, index, id) : 0;
}
void OnLog(enum retro_log_level level, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  va_list measure;
  va_copy(measure, arguments);
  const int length = std::vsnprintf(nullptr, 0, format, measure);
  va_end(measure);
  std::string text(length > 0 ? static_cast<size_t>(length) : 0, '\0');
  if (length > 0) std::vsnprintf(text.data(), text.size() + 1, format, arguments);
  va_end(arguments);
  if (g_game) g_game->Log(static_cast<int>(level), text.c_str());
}
uintptr_t OnCurrentFramebuffer() { return g_game ? g_game->CurrentFramebuffer() : 0; }
retro_proc_address_t OnProcAddress(const char* name) {
  return g_game ? reinterpret_cast<retro_proc_address_t>(g_game->ProcAddress(name)) : nullptr;
}
bool OnRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
  return g_game && g_game->Rumble(port, static_cast<int>(effect), strength);
}

constexpr unsigned kDualShock = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
constexpr int64_t kCardSaveMs = 5000;
constexpr size_t kPathBytes = 3 * 32767 + 1;
// The BIOS image sizes SwanStation loads: PS1, PS2 and PS3 (src/core/bios.h, BIOS_SIZE*).
constexpr size_t kBiosSizes[] = {0x80000, 0x400000, 0x3E66F0};
constexpr size_t kTextBytes = 256;
constexpr size_t kErrorBytes = 1024;
// Lockstep (protocol.md "lockstep"): steps queued ahead of the game at most, how long the game's
// thread waits for one before it looks at other work again, and the state file's header.
constexpr size_t kMaxSteps = 600;
constexpr int64_t kStepWaitMs = 10;
// How long entering or leaving lockstep waits for the frame being run to end.
constexpr int64_t kFrameFenceMs = 4000;
constexpr char kStateMagic[8] = {'A', 'P', '5', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kStateVersion = 1;
constexpr size_t kStateHeaderBytes = 48;
constexpr uint64_t kMaxLockstepFrame = 4294967295ULL;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t Fnv(const void* data, size_t size, uint64_t hash = kFnvOffset) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * kFnvPrime;
  return hash;
}

void PutLe(std::string& out, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

uint64_t GetLe(const std::string& in, size_t at, int bytes) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value |= static_cast<uint64_t>(static_cast<uint8_t>(in[at + i])) << (8 * i);
  return value;
}

// text cut to at most bytes, at a UTF-8 character boundary.
std::string Clip(const std::string& text, size_t bytes) {
  if (text.size() <= bytes) return text;
  size_t end = bytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  return text.substr(0, end);
}
constexpr char kResolutionOption[] = "swanstation_GPU_ResolutionScale";

// SwanStation options it applies only when the console boots. They take effect at the next start,
// like the options SwanStation marks "(Restart)".
constexpr const char* kStartOnlyOptions[] = {"swanstation_Console_Region", "swanstation_Console_Enable8MBRAM",
                                             "swanstation_BIOS_PatchFastBoot"};
// Options version 1 keeps at SwanStation's default and does not list:
// - the memory card options: the host keeps memory card 1 itself (<disc>.srm, protocol.md
//   "start"), and SwanStation's own card files are formatted and written over when it cannot read
//   them, out of the host's reach;
// - multitap, which moves controller port 2 away from the two ports protocol.md defines;
// - the log level, since the host finds a missing BIOS in SwanStation's information messages.
constexpr const char* kFixedOptions[] = {"swanstation_MemoryCards_Card1Type", "swanstation_MemoryCards_Card2Type",
                                         "swanstation_MemoryCards_UsePlaylistTitle",
                                         "swanstation_ControllerPorts_MultitapMode", "swanstation_Logging_LogLevel",
                                         "swanstation_GPU_Renderer"};

bool Listed(const char* const* list, size_t count, const std::string& key) {
  return std::find(list, list + count, key) != list + count;
}

#ifdef _WIN32
constexpr char kSeparators[] = "/\\";
constexpr char kSeparator = '\\';
#else
constexpr char kSeparators[] = "/";
constexpr char kSeparator = '/';
#endif

bool IsSeparator(char c) { return c != '\0' && std::strchr(kSeparators, c) != nullptr; }

std::string Stem(const std::string& path) {
  const size_t slash = path.find_last_of(kSeparators);
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string Directory(const std::string& path) {
  const size_t slash = path.find_last_of(kSeparators);
  return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool IsAbsolute(const std::string& path) {
  if (!path.empty() && IsSeparator(path[0])) return true;
#ifdef _WIN32
  return path.size() > 2 && path[1] == ':' && IsSeparator(path[2]);
#else
  return false;
#endif
}

}  // namespace

Game::Game(GameConfig config, PixelFormat format, GameEvents& events, AudioOutput& audio)
    : config_(std::move(config)), format_(format), events_(events), audio_(audio),
      quiet_since_(NowMs()), disc_control_(std::make_unique<DiscControl>()), lockstep_(config_.lockstep) {}

Game::~Game() {
  stop_ = true;
  std::lock_guard<std::mutex> join(join_mutex_);
  if (thread_.joinable()) thread_.join();
}

bool Game::LoadCore(std::string& error) {
  if (!library_.Open(config_.core, error)) return false;
  const char* const required[] = {"retro_api_version", "retro_set_environment", "retro_set_video_refresh",
                                  "retro_set_audio_sample", "retro_set_audio_sample_batch", "retro_set_input_poll",
                                  "retro_set_input_state", "retro_init", "retro_deinit", "retro_load_game",
                                  "retro_unload_game", "retro_run", "retro_get_system_av_info",
                                  "retro_set_controller_port_device", "retro_get_memory_data",
                                  "retro_get_memory_size"};
  for (const char* name : required) {
    if (!library_.Symbol(name)) {
      error = std::string("The core has no ") + name + ".";
      library_.Close();
      return false;
    }
  }
  if (Get<unsigned (*)()>("retro_api_version")() != RETRO_API_VERSION) {
    error = "The core is not a libretro version 1 core.";
    library_.Close();
    return false;
  }
  return true;
}

void Game::Begin() {
  audio_.SetPaused(false);
  quiet_since_ = NowMs();
  thread_ = std::thread([this] { Run(); });
}

void Game::Run() {
  g_game = this;
  RunGame();
  g_game = nullptr;
  library_.Close();
  std::lock_guard<std::mutex> lock(mutex_);
  finished_ = true;
  finished_signal_.notify_all();
}

void Game::RunGame() {
  CreateDirectories(config_.saves);
  std::string error;
  if (!LoadCore(error)) {
    Finish(error, "core");
    return;
  }
  if (!CanRead(config_.disc)) {
    Finish("The disc file cannot be opened.", "disc");
    return;
  }
  Get<void (*)(retro_environment_t)>("retro_set_environment")(OnEnvironment);
  Get<void (*)(retro_video_refresh_t)>("retro_set_video_refresh")(OnVideo);
  Get<void (*)(retro_audio_sample_t)>("retro_set_audio_sample")(OnAudioSample);
  Get<void (*)(retro_audio_sample_batch_t)>("retro_set_audio_sample_batch")(OnAudioBatch);
  Get<void (*)(retro_input_poll_t)>("retro_set_input_poll")(OnInputPoll);
  Get<void (*)(retro_input_state_t)>("retro_set_input_state")(OnInputState);
  Get<void (*)()>("retro_init")();

  if (config_.initial_disc) {
    const auto entry = PlaylistEntry(*config_.initial_disc);
    if (!entry || !disc_control_->extended || !disc_control_->callbacks.set_initial_image ||
        !disc_control_->callbacks.set_initial_image(*config_.initial_disc, entry->c_str())) {
      Get<void (*)()>("retro_deinit")();
      Finish("The playlist has no disc at that index.", "disc");
      return;
    }
  }

  retro_game_info game{config_.disc.c_str(), nullptr, 0, nullptr};
  if (!Get<bool (*)(const retro_game_info*)>("retro_load_game")(&game)) {
    DropGpu();
    Get<void (*)()>("retro_deinit")();
    if (!UsesBios()) Finish("No PS1 BIOS was found.", "bios");
    else Finish("The disc could not be read.", "disc");
    return;
  }
  if (disc_missing_ || !UsesBios()) {
    DropGpu();
    Get<void (*)()>("retro_unload_game")();
    Get<void (*)()>("retro_deinit")();
    if (disc_missing_) Finish("The disc could not be read.", "disc");
    else Finish("No PS1 BIOS was found.", "bios");
    return;
  }

  const auto set_port = Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device");
  set_port(0, kDualShock);
  set_port(1, RETRO_DEVICE_NONE);

  void* card = Get<void* (*)(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SAVE_RAM);
  const size_t card_size = Get<size_t (*)(unsigned)>("retro_get_memory_size")(RETRO_MEMORY_SAVE_RAM);
  if (card && card_size) {
    const std::string path = CardPath();
    if (const auto saved = ReadWholeFile(path); saved && saved->size() == card_size) {
      std::memcpy(card, saved->data(), card_size);
    } else if (Exists(path)) {
      card_kept_ = false;
      Log(RETRO_LOG_WARN, ("The memory card " + path + " cannot be read as one; it is left as it is.").c_str());
    }
    card_.assign(static_cast<const char*>(card), card_size);
  }

  retro_system_av_info info{};
  Get<void (*)(retro_system_av_info*)>("retro_get_system_av_info")(&info);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    SetAspect(info.geometry);
  }
  std::string end_error;
  std::string end_code;
  uint32_t reserve_width = info.geometry.max_width;
  uint32_t reserve_height = info.geometry.max_height;
  if (gpu_) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto geometry = LimitResolution(info.geometry.max_width, info.geometry.max_height)) {
      std::tie(reserve_width, reserve_height) = *geometry;
    }
  } else {
    std::lock_guard<std::mutex> lock(mutex_);
    software_ = true;
  }
  if (gpu_ && gpu_->Reserve(reserve_width, reserve_height, hw_depth_, hw_stencil_)) {
    hardware_ = true;
    if (hw_context_reset_) hw_context_reset_();
  } else if (gpu_) {
    end_error = "The host could not make a framebuffer on the GPU for the game.";
    end_code = "other";
  }
  RefreshDiscs();
  fps_ = info.timing.fps;
  sample_rate_ = info.timing.sample_rate;
  timing_changed_ = true;
  auto frame_time = std::chrono::duration<double>(1.0 / 60.0);

  const auto run = Get<void (*)()>("retro_run");
  auto next = std::chrono::steady_clock::now();
  int64_t last_card_check = NowMs();
  while (end_code.empty() && !stop_) {
    if (gpu_) gpu_->Pump();
    if (NowMs() - last_card_check >= kCardSaveMs) {
      SaveCard();
      last_card_check = NowMs();
    }
    ServeStateRequest();
    if (paused_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      next = std::chrono::steady_clock::now();
      continue;
    }
    if (timing_changed_) {
      timing_changed_ = false;
      audio_.Open(sample_rate_);
      const double fps = fps_ > 1 && fps_ < 1000 ? fps_ : 60.0;
      frame_time = std::chrono::duration<double>(1.0 / fps);
    }
    // in_frame_ marks the frame, so entering or leaving lockstep happens between two frames. While
    // a call waits for that, no frame starts.
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (fence_waiters_ > 0) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      in_frame_ = true;
    }
    const NextStep step = TakeStep(kStepWaitMs);
    if (step.lockstep && !step.input) {
      EndFrame();
      quiet_since_ = NowMs();
      next = std::chrono::steady_clock::now();
      continue;
    }
    step_input_ = step.input;
    if (step.input) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (connected_ports_ != step.input->connected) {
        connected_ports_ = step.input->connected;
        ports_changed_ = true;
      }
    }
    ApplyPendingWork();
    run();
    step_input_.reset();
    if (step.input) FinishStep(step.frame);
    EndFrame();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (disc_.inserted && *disc_.inserted < disc_.paths.size()) disc_.played = disc_.paths[*disc_.inserted];
    }
    if (frame_failure_ == FrameResult::kTooLarge) {
      end_error = "The game's frame is larger than 8192 x 8192.";
      end_code = "frameTooLarge";
      break;
    }
    if (frame_failure_ == FrameResult::kNoMemory) {
      end_error = "The host could not make memory for the game's frames.";
      end_code = "other";
      break;
    }
    if (frame_failure_ == FrameResult::kGpuFailed) {
      end_error = "The GPU stopped handing over the game's picture.";
      end_code = "other";
      break;
    }
    if (step.input && !step.paced) {
      next = std::chrono::steady_clock::now();
      continue;
    }
    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
    const auto now = std::chrono::steady_clock::now();
    if (next < now - std::chrono::milliseconds(250)) next = now;
    std::this_thread::sleep_until(next);
  }
  if (!SaveCard(true) && end_code.empty()) {
    end_error = "The memory card could not be saved.";
    end_code = "other";
  }
  DropGpu();
  Get<void (*)()>("retro_unload_game")();
  Get<void (*)()>("retro_deinit")();
  audio_.Close();
  Finish(end_error, end_code);
}

void Game::Finish(const std::string& error, const std::string& code) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ended_) {
      error_ = error;
      error_code_ = code;
      ended_ = true;
    }
  }
  state_signal_.notify_all();
}

void Game::ApplyPendingWork() {
  std::optional<unsigned> disc;
  bool ports = false;
  unsigned count = 1;
  bool refresh = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(refresh, display_refresh_);
    std::swap(disc, disc_request_);
    ports = ports_changed_;
    ports_changed_ = false;
    count = connected_ports_;
  }
  if (ports) {
    Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device")(1, count == 2 ? kDualShock : RETRO_DEVICE_NONE);
  }
  if (refresh && update_display_) update_display_();
  if (disc) {
    const auto& control = disc_control_->callbacks;
    bool changed = false;
    if (control.set_eject_state && control.get_eject_state && control.set_image_index && control.get_image_index &&
        control.get_num_images && *disc < control.get_num_images()) {
      const unsigned previous = control.get_image_index();
      const bool was_open = control.get_eject_state();
      changed = (was_open || control.set_eject_state(true)) && control.set_image_index(*disc) &&
                control.set_eject_state(false) && !control.get_eject_state() && control.get_image_index() == *disc;
      if (!changed && (control.get_eject_state() || control.set_eject_state(true))) {
        control.set_image_index(previous);
        control.set_eject_state(was_open);
      }
    }
    RefreshDiscs();
    std::lock_guard<std::mutex> lock(mutex_);
    disc_.busy = false;
    disc_.failed = !changed;
  }
}

void Game::RefreshDiscs() {
  const auto& control = disc_control_->callbacks;
  if (!control.get_num_images || !control.get_image_index || !control.get_eject_state) return;
  const unsigned count = control.get_num_images();
  Disc next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next = disc_;
  }
  next.labels.clear();
  next.paths.clear();
  for (unsigned i = 0; i < count; ++i) {
    std::vector<char> buffer(kPathBytes, '\0');
    char* text = buffer.data();
    std::string label;
    if (disc_control_->extended && disc_control_->callbacks.get_image_label &&
        disc_control_->callbacks.get_image_label(i, text, buffer.size() - 1) && text[0]) {
      label = Clip(text, kTextBytes);
    } else {
      label = "Disc " + std::to_string(i + 1);
    }
    next.labels.push_back(label);
    text[0] = '\0';
    if (disc_control_->extended && disc_control_->callbacks.get_image_path &&
        disc_control_->callbacks.get_image_path(i, text, buffer.size() - 1)) {
      next.paths.emplace_back(text);
    } else {
      next.paths.emplace_back();
    }
  }
  if (control.get_eject_state()) {
    next.inserted.reset();
  } else {
    next.inserted = control.get_image_index();
  }
  if (!next.inserted || *next.inserted >= next.paths.size() || next.played != next.paths[*next.inserted]) {
    next.played.clear();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  disc_ = next;
}

std::optional<std::string> Game::PlaylistEntry(unsigned index) const {
  const auto text = ReadWholeFile(config_.disc);
  if (!text) return std::nullopt;
  unsigned at = 0;
  size_t start = 0;
  while (start <= text->size()) {
    size_t end = text->find('\n', start);
    if (end == std::string::npos) end = text->size();
    std::string line = text->substr(start, end - start);
    start = end + 1;
    if (at == 0 && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
    line.erase(0, first);
    if (line.empty() || line[0] == '#') continue;
    if (at == index) return IsAbsolute(line) ? line : Directory(config_.disc) + line;
    ++at;
  }
  return std::nullopt;
}

std::string Game::CardPath() const {
  std::string saves = config_.saves;
  if (!saves.empty() && !IsSeparator(saves.back())) saves += kSeparator;
  return saves + Stem(config_.disc) + ".srm";
}

bool Game::SaveCard(bool always) {
  void* card = Get<void* (*)(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SAVE_RAM);
  const size_t card_size = Get<size_t (*)(unsigned)>("retro_get_memory_size")(RETRO_MEMORY_SAVE_RAM);
  if (!card || !card_size || !card_kept_) return true;
  std::string now(static_cast<const char*>(card), card_size);
  if (now == card_ && !always) return true;
  if (!CreateDirectories(config_.saves) || !WriteFileAtomically(CardPath(), now)) return false;
  card_ = std::move(now);
  return true;
}

bool Game::Environment(unsigned command, void* data) {
  switch (command) {
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
      if (!data) return false;
      *static_cast<retro_savestate_context*>(data) = static_cast<retro_savestate_context>(savestate_context_);
      return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *static_cast<bool*>(data) = true;
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *static_cast<const char**>(data) = config_.system.c_str();
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *static_cast<const char**>(data) = config_.saves.c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
      const auto format = *static_cast<const retro_pixel_format*>(data);
      if (format != RETRO_PIXEL_FORMAT_0RGB1555 && format != RETRO_PIXEL_FORMAT_XRGB8888 &&
          format != RETRO_PIXEL_FORMAT_RGB565) {
        return false;
      }
      core_pixel_format_ = format;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      static_cast<retro_log_callback*>(data)->log = OnLog;
      return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
      static_cast<retro_rumble_interface*>(data)->set_rumble_state = OnRumble;
      return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *static_cast<unsigned*>(data) = 2;
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
      const retro_core_options_v2* options =
          command == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 ? static_cast<const retro_core_options_v2*>(data)
                                                           : static_cast<const retro_core_options_v2_intl*>(data)->us;
      std::lock_guard<std::mutex> lock(mutex_);
      options_.clear();
      for (const retro_core_option_v2_definition* definition = options ? options->definitions : nullptr;
           definition && definition->key; ++definition) {
        Option option;
        option.key = definition->key;
        const std::string text = definition->desc ? definition->desc : definition->key;
        option.title = Clip(text, kTextBytes);
        option.restart = (text.size() >= 9 && text.compare(text.size() - 9, 9, "(Restart)") == 0) ||
                         Listed(kStartOnlyOptions, std::size(kStartOnlyOptions), option.key);
        option.fixed = Listed(kFixedOptions, std::size(kFixedOptions), option.key);
        for (const retro_core_option_value* value = definition->values; value->value; ++value) {
          option.values.emplace_back(value->value);
          option.labels.emplace_back(Clip(value->label ? value->label : value->value, kTextBytes));
        }
        if (option.values.empty()) continue;
        const auto chosen = [&](const std::string& wanted) {
          const auto found = std::find(option.values.begin(), option.values.end(), wanted);
          return found == option.values.end() ? std::optional<size_t>() : std::optional<size_t>(found - option.values.begin());
        };
        option.selected = chosen(definition->default_value ? definition->default_value : "").value_or(0);
        for (const auto& [key, value] : config_.options) {
          if (key == option.key && !option.fixed) option.selected = chosen(value).value_or(option.selected);
        }
        option.active = option.values[option.selected];
        options_.push_back(std::move(option));
      }
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
      const auto* callback = static_cast<const retro_core_options_update_display_callback*>(data);
      update_display_ = callback ? callback->callback : nullptr;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
      const auto* display = static_cast<const retro_core_option_display*>(data);
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& option : options_) {
        if (display->key && option.key == display->key) option.visible = display->visible;
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* variable = static_cast<retro_variable*>(data);
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& option : options_) {
        if (variable->key && option.key == variable->key) {
          if (!option.restart) option.active = option.values[option.selected];
          option.read = true;
          variable->value = option.active.c_str();
          return true;
        }
      }
      variable->value = nullptr;
      return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
      std::lock_guard<std::mutex> lock(mutex_);
      *static_cast<bool*>(data) = options_changed_;
      options_changed_ = false;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
      *static_cast<unsigned*>(data) = 1;
      return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
      static_assert(offsetof(retro_disk_control_ext_callback, set_initial_image) == sizeof(retro_disk_control_callback));
      disc_control_->callbacks = {};
      std::memcpy(&disc_control_->callbacks, data, sizeof(retro_disk_control_callback));
      disc_control_->extended = false;
      return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
      disc_control_->callbacks = *static_cast<const retro_disk_control_ext_callback*>(data);
      disc_control_->extended = true;
      return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const auto* geometry = static_cast<const retro_game_geometry*>(data);
      std::lock_guard<std::mutex> lock(mutex_);
      SetAspect(*geometry);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
      const auto* info = static_cast<const retro_system_av_info*>(data);
      fps_ = info->timing.fps;
      sample_rate_ = info->timing.sample_rate;
      timing_changed_ = true;
      if (hardware_ && !gpu_->Reserve(info->geometry.max_width, info->geometry.max_height, hw_depth_, hw_stencil_)) {
        frame_failure_ = FrameResult::kGpuFailed;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      SetAspect(info->geometry);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
      stop_ = true;
      return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
      if (!data) return false;
      *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      if (!data) return false;
      auto* request = static_cast<retro_hw_render_callback*>(data);
      const bool core = request->context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
      if (!core && request->context_type != RETRO_HW_CONTEXT_OPENGL) return false;
      gpu_.reset();
      gpu_ = Gpu::Create(core ? request->version_major : 0, core ? request->version_minor : 0, core);
      if (!gpu_) return false;
      request->get_current_framebuffer = OnCurrentFramebuffer;
      request->get_proc_address = OnProcAddress;
      hw_context_reset_ = request->context_reset;
      hw_context_destroy_ = request->context_destroy;
      hw_depth_ = request->depth;
      hw_stencil_ = request->stencil;
      hw_bottom_left_ = request->bottom_left_origin;
      return true;
    }
    default:
      return false;
  }
}

void Game::Video(const void* data, unsigned width, unsigned height, size_t pitch) {
  if (published_) quiet_since_ = NowMs();
  if (!data) return;
  if (width == 0 || height == 0) return;
  if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    if (!hardware_) return;
    if (width > kMaxFrameSide || height > kMaxFrameSide) {
      frame_failure_ = FrameResult::kTooLarge;
      return;
    }
    try {
      converted_.resize(static_cast<size_t>(width) * height * 4);
    } catch (const std::bad_alloc&) {
      frame_failure_ = FrameResult::kNoMemory;
      return;
    }
    if (!gpu_->Read(width, height, format_ == PixelFormat::kBgra8, hw_bottom_left_, converted_.data())) {
      frame_failure_ = FrameResult::kGpuFailed;
      return;
    }
    Publish(width, height);
    return;
  }
  if (width > kMaxFrameSide || height > kMaxFrameSide) {
    frame_failure_ = FrameResult::kTooLarge;
    return;
  }
  try {
    converted_.resize(static_cast<size_t>(width) * height * 4);
  } catch (const std::bad_alloc&) {
    frame_failure_ = FrameResult::kNoMemory;
    return;
  }
  const bool bgra = format_ == PixelFormat::kBgra8;
  for (unsigned y = 0; y < height; ++y) {
    const auto* row = static_cast<const uint8_t*>(data) + y * pitch;
    uint8_t* out = converted_.data() + static_cast<size_t>(y) * width * 4;
    for (unsigned x = 0; x < width; ++x, out += 4) {
      uint8_t r, g, b;
      if (core_pixel_format_ == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t pixel;
        std::memcpy(&pixel, row + x * 4, 4);
        r = (pixel >> 16) & 0xFF;
        g = (pixel >> 8) & 0xFF;
        b = pixel & 0xFF;
      } else {
        uint16_t pixel;
        std::memcpy(&pixel, row + x * 2, 2);
        if (core_pixel_format_ == RETRO_PIXEL_FORMAT_RGB565) {
          r = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);
          g = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);
        } else {
          r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
          g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
        }
        b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
      }
      out[0] = bgra ? b : r;
      out[1] = g;
      out[2] = bgra ? r : b;
      out[3] = 0xFF;
    }
  }
  Publish(width, height);
}

void Game::Publish(uint32_t width, uint32_t height) {
  if (step_input_) {
    // The picture in RGB order, whichever pixel format the ring has, so peers can compare it.
    const uint32_t size[2] = {width, height};
    uint64_t hash = Fnv(size, sizeof size);
    const bool bgra = format_ == PixelFormat::kBgra8;
    for (size_t at = 0; at < converted_.size(); at += 4) {
      const uint8_t rgb[3] = {converted_[at + (bgra ? 2 : 0)], converted_[at + 1], converted_[at + (bgra ? 0 : 2)]};
      hash = Fnv(rgb, sizeof rgb, hash);
    }
    video_hash_ = hash;
  }
  const FrameResult shown = events_.OnFrame(converted_.data(), width, height, config_.epoch);
  if (shown != FrameResult::kShown) {
    frame_failure_ = shown;
    return;
  }
  published_ = true;
  quiet_since_ = NowMs();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
      ready_ = true;
      if (disc_.inserted && *disc_.inserted < disc_.paths.size()) disc_.played = disc_.paths[*disc_.inserted];
    }
  }
}

size_t Game::AudioBatch(const int16_t* samples, size_t frames) {
  audio_.Write(samples, frames);
  return frames;
}

int16_t Game::InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
  if (port > 1) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (port == 1 && connected_ports_ < 2) return 0;
  const PortInput& input = step_input_ ? step_input_->ports[port] : input_[port];
  const unsigned base = device & RETRO_DEVICE_MASK;
  if (base == RETRO_DEVICE_JOYPAD) {
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(input.buttons);
    return id < 16 ? static_cast<int16_t>((input.buttons >> id) & 1) : 0;
  }
  if (base == RETRO_DEVICE_ANALOG) {
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) return id < 16 && ((input.buttons >> id) & 1) ? 0x7FFF : 0;
    if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT || id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
    return input.sticks[index * 2 + id];
  }
  return 0;
}

void Game::Log(int level, const char* text) {
  if (std::strstr(text, "Using BIOS '") || std::strstr(text, "Falling back to possibly-incompatible image")) {
    bios_found_ = true;
  }
  if (const char* region = std::strstr(text, "Console Region: ")) console_region_ = region + 16;
  if (std::strstr(text, "Booting BIOS with no disc")) disc_missing_ = true;
  std::fprintf(stderr, "[core %d] %s", level, text);
  const size_t length = std::strlen(text);
  if (length == 0 || text[length - 1] != '\n') std::fputc('\n', stderr);
}

bool Game::Rumble(unsigned port, int effect, uint16_t strength) {
  if (port > 1 || (effect != RETRO_RUMBLE_STRONG && effect != RETRO_RUMBLE_WEAK)) return false;
  auto& motors = rumble_[port];
  const size_t motor = effect == RETRO_RUMBLE_STRONG ? 0 : 1;
  if (motors[motor] == strength) return true;
  motors[motor] = strength;
  events_.OnRumble(port, motors[0], motors[1]);
  return true;
}

Json Game::Status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonObject options;
  for (const auto& option : options_) {
    if (!option.visible || option.fixed) continue;
    JsonArray values(option.values.begin(), option.values.end());
    JsonArray labels(option.labels.begin(), option.labels.end());
    options.emplace_back(option.key, JsonObject{{"title", option.title},
                                                {"values", std::move(values)},
                                                {"labels", std::move(labels)},
                                                {"selected", static_cast<uint64_t>(option.selected)},
                                                {"activeValue", option.active},
                                                {"restart", option.restart}});
  }
  const double aspect = aspect_ > 0 && aspect_ <= 10 ? aspect_ : 4.0 / 3.0;
  bool pending = false;
  for (const auto& option : options_) {
    pending = pending || (option.read && !option.restart && option.values[option.selected] != option.active);
  }
  JsonObject status{{"ready", ready_},
                    {"ended", ended_},
                    {"error", Clip(error_, kErrorBytes)},
                    {"errorCode", error_code_},
                    {"aspect", aspect},
                    {"hardware", hardware_.load()},
                    {"resolutionError", ResolutionError()},
                    {"graphicsPending", pending},
                    {"progressLabel", nullptr},
                    {"progressPercent", nullptr},
                    {"options", std::move(options)}};
  if (disc_.labels.size() > 1) {
    JsonObject discs;
    discs.emplace_back("labels", JsonArray(disc_.labels.begin(), disc_.labels.end()));
    if (disc_.inserted) {
      discs.emplace_back("inserted", static_cast<uint64_t>(*disc_.inserted));
    } else {
      discs.emplace_back("inserted", nullptr);
    }
    if (disc_.inserted && *disc_.inserted < disc_.paths.size()) {
      discs.emplace_back("insertedDiscPath", disc_.paths[*disc_.inserted]);
    } else {
      discs.emplace_back("insertedDiscPath", nullptr);
    }
    discs.emplace_back("playedDiscPath", ready_ ? disc_.played : std::string());
    discs.emplace_back("busy", disc_.busy);
    discs.emplace_back("failed", disc_.failed);
    status.emplace_back("discs", std::move(discs));
  }
  return Json(std::move(status));
}

void Game::Pause(bool paused) {
  std::lock_guard<std::mutex> lock(pause_mutex_);
  if (paused == paused_) return;
  audio_.SetPaused(paused);
  if (paused) {
    paused_at_ = NowMs();
    paused_ = true;
  } else {
    const int64_t quiet_before = std::max<int64_t>(0, paused_at_ - quiet_since_);
    quiet_since_ = NowMs() - quiet_before;
    paused_at_ = -1;
    paused_ = false;
  }
}

bool Game::SetConnectedPorts(unsigned count) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ended_ || lockstep_) return false;
  if (connected_ports_ != count) {
    connected_ports_ = count;
    ports_changed_ = true;
  }
  return true;
}

bool Game::ChangeDisc(unsigned index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ended_ || lockstep_ || disc_.labels.size() < 2 || index >= disc_.labels.size() || disc_.busy) return false;
  disc_.busy = true;
  disc_request_ = index;
  return true;
}

bool Game::SetOption(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ended_ || lockstep_) return false;
  for (auto& option : options_) {
    if (option.key != key || !option.visible || option.fixed) continue;
    const auto found = std::find(option.values.begin(), option.values.end(), value);
    if (found == option.values.end()) return false;
    const auto selected = static_cast<size_t>(found - option.values.begin());
    if (selected != option.selected) {
      option.selected = selected;
      options_changed_ = true;
      display_refresh_ = true;
    }
    if (option.key == kResolutionOption) resolution_refused_.clear();
    return true;
  }
  return false;
}

void Game::SetInput(unsigned port, const PortInput& input) {
  if (port > 1) return;
  std::lock_guard<std::mutex> lock(mutex_);
  input_[port] = input;
}

void Game::Stop() { stop_ = true; }

bool Game::Ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ended_;
}

void Game::Fail(const std::string& error, const std::string& code) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ended_) {
      error_ = error;
      error_code_ = code;
      ended_ = true;
    }
  }
  state_signal_.notify_all();
  stop_ = true;
}

bool Game::Join(int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!finished_signal_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return finished_; })) {
    return false;
  }
  lock.unlock();
  std::lock_guard<std::mutex> join(join_mutex_);
  if (thread_.joinable()) thread_.join();
  return true;
}

int64_t Game::QuietMs() const {
  std::lock_guard<std::mutex> lock(pause_mutex_);
  const int64_t until = paused_ ? paused_at_.load() : NowMs();
  return std::max<int64_t>(0, until - quiet_since_);
}

void Game::SetAspect(const retro_game_geometry& geometry) {
  if (geometry.aspect_ratio > 0) {
    aspect_ = geometry.aspect_ratio;
  } else if (geometry.base_width && geometry.base_height) {
    aspect_ = static_cast<double>(geometry.base_width) / geometry.base_height;
  }
}

bool Game::UsesBios() const {
  if (bios_found_) return true;
  const char* key = nullptr;
  if (console_region_.rfind("NTSC-J", 0) == 0) key = "swanstation_BIOS_PathNTSCJ";
  else if (console_region_.rfind("NTSC-U", 0) == 0) key = "swanstation_BIOS_PathNTSCU";
  else if (console_region_.rfind("PAL", 0) == 0) key = "swanstation_BIOS_PathPAL";
  if (!key) return false;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& option : options_) {
      if (option.key == key) name = option.active;
    }
  }
  if (name.empty()) return false;
  std::string path = config_.system;
  if (!path.empty() && !IsSeparator(path.back())) path += kSeparator;
  const auto image = ReadWholeFile(path + name);
  return image && std::find(std::begin(kBiosSizes), std::end(kBiosSizes), image->size()) != std::end(kBiosSizes);
}

uintptr_t Game::CurrentFramebuffer() const { return gpu_ ? gpu_->Framebuffer() : 0; }

void* Game::ProcAddress(const char* name) const { return gpu_ ? gpu_->Proc(name) : nullptr; }

// As RetroArch does in core_unload_game: the core is told its context goes away before
// retro_unload_game, while the game is still loaded (SwanStation switches back to its software
// renderer then).
void Game::DropGpu() {
  if (hardware_ && hw_context_destroy_) hw_context_destroy_();
  hardware_ = false;
  gpu_.reset();
}

std::optional<std::pair<uint32_t, uint32_t>> Game::LimitResolution(uint32_t max_width, uint32_t max_height) {
  const uint32_t width_limit = std::min(kMaxFrameSide, gpu_->MaxWidth());
  const uint32_t height_limit = std::min(kMaxFrameSide, gpu_->MaxHeight());
  for (auto& option : options_) {
    if (option.key != kResolutionOption || option.fixed || option.restart || !option.read) continue;
    const auto active = std::find(option.values.begin(), option.values.end(), option.active);
    if (active == option.values.end()) return std::nullopt;
    const auto limit = retro_host::LimitResolution(option.values, option.selected,
                                                   static_cast<size_t>(active - option.values.begin()), max_width,
                                                   max_height, width_limit, height_limit);
    if (!limit) return std::nullopt;
    const std::string asked = option.labels[option.selected];
    const bool moved = limit->selected != option.selected;
    std::vector<std::string> values;
    std::vector<std::string> labels;
    for (const size_t i : limit->offered) {
      if (i == limit->selected) option.selected = values.size();
      values.push_back(option.values[i]);
      labels.push_back(option.labels[i]);
    }
    option.values = std::move(values);
    option.labels = std::move(labels);
    if (moved) {
      options_changed_ = true;
      resolution_refused_ = asked + " would make the game's picture up to " +
                            std::to_string(limit->refused_width) + " x " + std::to_string(limit->refused_height) +
                            ", more than the " + std::to_string(width_limit) + " x " + std::to_string(height_limit) +
                            " the host can pass on with this GPU, so the game uses " +
                            option.labels[option.selected] + ".";
    }
    if (limit->active_offered) return std::nullopt;
    option.active = option.values[option.selected];
    options_changed_ = true;
    return std::make_pair(limit->width, limit->height);
  }
  return std::nullopt;
}

std::string Game::ResolutionError() const {
  if (!resolution_refused_.empty()) return resolution_refused_;
  if (!software_) return "";
  for (const auto& option : options_) {
    if (option.key == kResolutionOption && option.values[option.selected] != "1") {
      return "No OpenGL context could be made, so the game draws in software, which has no internal resolution above 1x.";
    }
  }
  return "";
}

bool Game::Finished() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_;
}

bool Game::Ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_;
}


void Game::EndFrame() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_frame_ = false;
  }
  fence_signal_.notify_all();
}

bool Game::AwaitFrameEnd(std::unique_lock<std::mutex>& lock) {
  ++fence_waiters_;
  step_signal_.notify_all();
  const bool between =
      fence_signal_.wait_for(lock, std::chrono::milliseconds(kFrameFenceMs), [this] { return !in_frame_; });
  --fence_waiters_;
  return between;
}

std::optional<uint64_t> Game::Lockstep(const LockstepConfig& config) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!AwaitFrameEnd(lock) || ended_) return std::nullopt;
  if (!lockstep_) {
    next_frame_ = 0;
    steps_.clear();
  }
  lockstep_ = config;
  return next_frame_ + steps_.size();
}

bool Game::LeaveLockstep() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!AwaitFrameEnd(lock) || ended_) return false;
    lockstep_.reset();
    steps_.clear();
  }
  step_signal_.notify_all();
  return true;
}

StepResult Game::Step(uint64_t frame, const StepInput& input) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ended_) return StepResult::kIgnored;
    if (!lockstep_ || frame != next_frame_ + steps_.size() || steps_.size() >= kMaxSteps) return StepResult::kRefused;
    steps_.push_back(input);
  }
  step_signal_.notify_all();
  return StepResult::kTaken;
}

Game::NextStep Game::TakeStep(int64_t wait_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  NextStep next;
  if (!lockstep_) return next;
  if (steps_.empty()) {
    step_signal_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                          [this] {
                            return !lockstep_ || !steps_.empty() || state_request_ || stop_ || fence_waiters_ > 0;
                          });
  }
  if (!lockstep_) return next;
  next.lockstep = true;
  next.paced = lockstep_->paced;
  if (steps_.empty() || state_request_ || fence_waiters_ > 0) return next;
  next.frame = next_frame_++;
  next.input = steps_.front();
  steps_.pop_front();
  return next;
}

void Game::FinishStep(uint64_t frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lockstep_ || frame % lockstep_->check != 0) return;
  }
  SyncPoint point;
  point.frame = frame;
  point.video = video_hash_;
  const auto ram = Get<void* (*)(unsigned)>("retro_get_memory_data")(RETRO_MEMORY_SYSTEM_RAM);
  const size_t ram_size = Get<size_t (*)(unsigned)>("retro_get_memory_size")(RETRO_MEMORY_SYSTEM_RAM);
  if (ram && ram_size) point.ram = Fnv(ram, ram_size);
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
  if (const std::string* state = Serialize()) point.state = Fnv(state->data(), state->size());
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_NORMAL;
  events_.OnSync(point);
}

const std::string* Game::Serialize() {
  const auto size_of = Get<size_t (*)()>("retro_serialize_size");
  const auto serialize = Get<bool (*)(void*, size_t)>("retro_serialize");
  if (!size_of || !serialize) return nullptr;
  const size_t size = size_of();
  if (size == 0) return nullptr;
  try {
    serialized_.assign(size, '\0');
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
  return serialize(serialized_.data(), size) ? &serialized_ : nullptr;
}

std::optional<StateResult> Game::SaveState(const std::string& path, int64_t timeout_ms) {
  return RequestState(StateWork::kSave, path, timeout_ms);
}

std::optional<StateResult> Game::LoadState(const std::string& path, int64_t timeout_ms) {
  return RequestState(StateWork::kLoad, path, timeout_ms);
}

std::optional<StateResult> Game::RequestState(StateWork work, const std::string& path, int64_t timeout_ms) {
  auto request = std::make_shared<StateRequest>();
  request->work = work;
  request->path = path;
  std::unique_lock<std::mutex> lock(mutex_);
  if (ended_ || !lockstep_ || state_request_) return std::nullopt;
  state_request_ = request;
  step_signal_.notify_all();
  if (state_signal_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->done || ended_; })) {
    if (state_request_ == request) state_request_.reset();
    return request->result;
  }
  const bool taken = state_request_ != request;
  if (!taken) state_request_.reset();
  lock.unlock();
  // A load the game's thread began but did not finish leaves the console in no known state.
  if (taken && work == StateWork::kLoad) Fail("The game's state could not be loaded in time.", "other");
  return std::nullopt;
}

void Game::ServeStateRequest() {
  std::shared_ptr<StateRequest> request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(request, state_request_);
  }
  if (!request) return;
  auto result = request->work == StateWork::kSave ? WriteState(request->path) : ReadState(request->path);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    request->result = result;
    request->done = true;
  }
  state_signal_.notify_all();
}

std::optional<StateResult> Game::WriteState(const std::string& path) {
  uint64_t frame = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lockstep_) return std::nullopt;
    frame = next_frame_;
  }
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
  const std::string* state = Serialize();
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_NORMAL;
  if (!state) return std::nullopt;
  std::string file(kStateMagic, sizeof kStateMagic);
  PutLe(file, kStateVersion, 4);
  PutLe(file, video_hash_ ? 1 : 0, 4);
  PutLe(file, frame, 8);
  PutLe(file, video_hash_.value_or(0), 8);
  PutLe(file, state->size(), 8);
  PutLe(file, 0, 8);
  file += *state;
  if (!WriteFileAtomically(path, file)) return std::nullopt;
  return StateResult{frame, file.size()};
}

std::optional<StateResult> Game::ReadState(const std::string& path) {
  const auto file = ReadWholeFile(path);
  if (!file || file->size() < kStateHeaderBytes ||
      file->compare(0, sizeof kStateMagic, kStateMagic, sizeof kStateMagic) != 0 ||
      GetLe(*file, 8, 4) != kStateVersion || GetLe(*file, 12, 4) > 1 || GetLe(*file, 40, 8) != 0 ||
      GetLe(*file, 32, 8) != file->size() - kStateHeaderBytes) {
    return std::nullopt;
  }
  const auto unserialize = Get<bool (*)(const void*, size_t)>("retro_unserialize");
  if (!unserialize) return std::nullopt;
  const uint64_t frame = GetLe(*file, 16, 8);
  if (frame > kMaxLockstepFrame) return std::nullopt;
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
  const bool loaded = unserialize(file->data() + kStateHeaderBytes, file->size() - kStateHeaderBytes);
  savestate_context_ = RETRO_SAVESTATE_CONTEXT_NORMAL;
  if (!loaded) return std::nullopt;
  video_hash_ = GetLe(*file, 12, 4) ? std::optional<uint64_t>(GetLe(*file, 24, 8)) : std::nullopt;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next_frame_ = frame;
    steps_.clear();
  }
  return StateResult{frame, file->size()};
}

}  // namespace retro_host
