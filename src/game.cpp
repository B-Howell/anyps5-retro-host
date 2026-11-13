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
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(text, sizeof text, format, arguments);
  va_end(arguments);
  if (g_game) g_game->Log(static_cast<int>(level), text);
}
bool OnRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
  return g_game && g_game->Rumble(port, static_cast<int>(effect), strength);
}

constexpr unsigned kDualShock = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
constexpr int64_t kCardSaveMs = 5000;
constexpr size_t kPathBytes = 32768;
constexpr char kResolutionOption[] = "swanstation_GPU_ResolutionScale";

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
      quiet_since_(NowMs()), disc_control_(std::make_unique<DiscControl>()) {}

Game::~Game() {
  stop_ = true;
  std::lock_guard<std::mutex> join(join_mutex_);
  if (thread_.joinable()) thread_.join();
}

bool Game::Start(std::string& error) {
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
}

void Game::RunGame() {
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
    Get<void (*)()>("retro_deinit")();
    if (bios_missing_) Finish("No PS1 BIOS was found.", "bios");
    else Finish("The disc could not be read.", "disc");
    return;
  }
  if (bios_missing_) {
    Get<void (*)()>("retro_unload_game")();
    Get<void (*)()>("retro_deinit")();
    Finish("No PS1 BIOS was found.", "bios");
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
    if (info.geometry.aspect_ratio > 0) aspect_ = info.geometry.aspect_ratio;
    else if (info.geometry.base_height) aspect_ = static_cast<double>(info.geometry.base_width) / info.geometry.base_height;
  }
  RefreshDiscs();
  fps_ = info.timing.fps;
  sample_rate_ = info.timing.sample_rate;
  timing_changed_ = true;
  auto frame_time = std::chrono::duration<double>(1.0 / 60.0);

  const auto run = Get<void (*)()>("retro_run");
  auto next = std::chrono::steady_clock::now();
  int64_t last_card_check = NowMs();
  std::string end_error;
  std::string end_code;
  while (!stop_) {
    if (NowMs() - last_card_check >= kCardSaveMs) {
      SaveCard();
      last_card_check = NowMs();
    }
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
    ApplyPendingWork();
    run();
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
    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame_time);
    const auto now = std::chrono::steady_clock::now();
    if (next < now - std::chrono::milliseconds(250)) next = now;
    std::this_thread::sleep_until(next);
  }
  if (!SaveCard(true) && end_code.empty()) {
    end_error = "The memory card could not be saved.";
    end_code = "other";
  }
  Get<void (*)()>("retro_unload_game")();
  Get<void (*)()>("retro_deinit")();
  audio_.Close();
  Finish(end_error, end_code);
}

void Game::Finish(const std::string& error, const std::string& code) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ended_) {
    error_ = error;
    error_code_ = code;
    ended_ = true;
  }
  finished_ = true;
  finished_signal_.notify_all();
}

void Game::ApplyPendingWork() {
  std::optional<unsigned> disc;
  bool ports = false;
  unsigned count = 1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(disc, disc_request_);
    ports = ports_changed_;
    ports_changed_ = false;
    count = connected_ports_;
  }
  if (ports) {
    Get<void (*)(unsigned, unsigned)>("retro_set_controller_port_device")(1, count == 2 ? kDualShock : RETRO_DEVICE_NONE);
  }
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
      label = text;
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
        option.title = definition->desc ? definition->desc : definition->key;
        for (const retro_core_option_value* value = definition->values; value->value; ++value) {
          option.values.emplace_back(value->value);
          option.labels.emplace_back(value->label ? value->label : value->value);
        }
        if (option.values.empty()) continue;
        const auto chosen = [&](const std::string& wanted) {
          const auto found = std::find(option.values.begin(), option.values.end(), wanted);
          return found == option.values.end() ? std::optional<size_t>() : std::optional<size_t>(found - option.values.begin());
        };
        option.selected = chosen(definition->default_value ? definition->default_value : "").value_or(0);
        for (const auto& [key, value] : config_.options) {
          if (key == option.key) option.selected = chosen(value).value_or(option.selected);
        }
        option.active = option.values[option.selected];
        options_.push_back(std::move(option));
      }
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
          option.active = option.values[option.selected];
          variable->value = option.values[option.selected].c_str();
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
      if (geometry->aspect_ratio > 0) aspect_ = geometry->aspect_ratio;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
      const auto* info = static_cast<const retro_system_av_info*>(data);
      fps_ = info->timing.fps;
      sample_rate_ = info->timing.sample_rate;
      timing_changed_ = true;
      std::lock_guard<std::mutex> lock(mutex_);
      if (info->geometry.aspect_ratio > 0) aspect_ = info->geometry.aspect_ratio;
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
    default:
      return false;
  }
}

void Game::Video(const void* data, unsigned width, unsigned height, size_t pitch) {
  quiet_since_ = NowMs();
  if (!data) return;
  if (width == 0 || height == 0) return;
  if (width > kMaxFrameSide || height > kMaxFrameSide) {
    frame_failure_ = FrameResult::kTooLarge;
    return;
  }
  converted_.resize(static_cast<size_t>(width) * height * 4);
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
  const FrameResult shown = events_.OnFrame(converted_.data(), width, height, config_.epoch);
  if (shown != FrameResult::kShown) {
    frame_failure_ = shown;
    return;
  }
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
  const PortInput& input = input_[port];
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
  if (std::strstr(text, "No BIOS image found")) bios_missing_ = true;
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
    if (!option.visible) continue;
    JsonArray values(option.values.begin(), option.values.end());
    JsonArray labels(option.labels.begin(), option.labels.end());
    options.emplace_back(option.key, JsonObject{{"title", option.title},
                                                {"values", std::move(values)},
                                                {"labels", std::move(labels)},
                                                {"selected", static_cast<uint64_t>(option.selected)},
                                                {"activeValue", option.active},
                                                {"restart", false}});
  }
  const double aspect = aspect_ > 0 && aspect_ <= 10 ? aspect_ : 4.0 / 3.0;
  bool pending = false;
  for (const auto& option : options_) pending = pending || option.values[option.selected] != option.active;
  JsonObject status{{"ready", ready_},
                    {"ended", ended_},
                    {"error", error_},
                    {"errorCode", error_code_},
                    {"aspect", aspect},
                    {"hardware", false},
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
  if (ended_) return false;
  if (connected_ports_ != count) {
    connected_ports_ = count;
    ports_changed_ = true;
  }
  return true;
}

bool Game::ChangeDisc(unsigned index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ended_ || disc_.labels.size() < 2 || index >= disc_.labels.size() || disc_.busy) return false;
  disc_.busy = true;
  disc_request_ = index;
  return true;
}

bool Game::SetOption(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ended_) return false;
  for (auto& option : options_) {
    if (option.key != key || !option.visible) continue;
    const auto found = std::find(option.values.begin(), option.values.end(), value);
    if (found == option.values.end()) return false;
    const auto selected = static_cast<size_t>(found - option.values.begin());
    if (selected != option.selected) {
      option.selected = selected;
      options_changed_ = true;
    }
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

std::string Game::ResolutionError() const {
  for (const auto& option : options_) {
    if (option.key == kResolutionOption && option.values[option.selected] != "1") {
      return "This host draws with the software renderer, which has no internal resolution above 1x.";
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

}  // namespace retro_host
