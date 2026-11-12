#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace {
struct GameInfo {
  const char* path;
  const void* data;
  std::size_t size;
  const char* meta;
};
struct AvInfo {
  unsigned width, height, max_width, max_height;
  float aspect;
  double fps, sample_rate;
};

constexpr unsigned kWidth = 8, kHeight = 4, kPorts = 4;
constexpr unsigned kJoypad = 1, kMask = 256, kBitmasks = 51 | 0x10000, kPixelFormat = 10;

bool (*environment)(unsigned, void*);
void (*video)(const void*, unsigned, unsigned, std::size_t);
int16_t (*input)(unsigned, unsigned, unsigned, unsigned);
void (*poll)();
std::size_t (*audio)(const int16_t*, std::size_t);

enum class Mode { kFrames, kCrash, kHang };
Mode mode = Mode::kFrames;
std::atomic<uint32_t> frames{0};
constexpr unsigned kNeverSet = 0xFFFF;
std::atomic<unsigned> devices[kPorts] = {kNeverSet, kNeverSet, kNeverSet, kNeverSet};

std::string Name(const char* path) {
  std::string text(path ? path : "");
  const auto slash = text.find_last_of("/\\");
  return slash == std::string::npos ? text : text.substr(slash + 1);
}

uint16_t Buttons(unsigned port) {
  if (environment(kBitmasks, nullptr)) return static_cast<uint16_t>(input(port, kJoypad, 0, kMask));
  uint16_t mask = 0;
  for (unsigned id = 0; id < 16; ++id)
    if (input(port, kJoypad, 0, id)) mask |= static_cast<uint16_t>(1u << id);
  return mask;
}
}  // namespace

extern "C" TEST_EXPORT unsigned retro_api_version() { return 1; }
extern "C" TEST_EXPORT void retro_init() { frames = 0; }
extern "C" TEST_EXPORT void retro_deinit() {}
extern "C" TEST_EXPORT void retro_set_environment(decltype(environment) callback) { environment = callback; }
extern "C" TEST_EXPORT void retro_set_video_refresh(decltype(video) callback) { video = callback; }
extern "C" TEST_EXPORT void retro_set_input_state(decltype(input) callback) { input = callback; }
extern "C" TEST_EXPORT void retro_set_input_poll(void (*callback)()) { poll = callback; }
extern "C" TEST_EXPORT void retro_set_audio_sample(void (*)(int16_t, int16_t)) {}
extern "C" TEST_EXPORT void retro_set_audio_sample_batch(decltype(audio) callback) { audio = callback; }
extern "C" TEST_EXPORT void retro_set_controller_port_device(unsigned port, unsigned device) {
  if (port < kPorts) devices[port] = device;
}
extern "C" TEST_EXPORT void retro_reset() {}
extern "C" TEST_EXPORT void retro_get_system_info(void* info) { std::memset(info, 0, 5 * sizeof(void*)); }
extern "C" TEST_EXPORT void retro_get_system_av_info(AvInfo* info) {
  *info = {kWidth, kHeight, kWidth, kHeight, 2.0f, 60.0, 48000.0};
}
extern "C" TEST_EXPORT unsigned retro_get_region() { return 0; }
extern "C" TEST_EXPORT std::size_t retro_serialize_size() { return 4; }
extern "C" TEST_EXPORT bool retro_serialize(void* data, std::size_t size) {
  if (size < 4) return false;
  const uint32_t count = frames;
  std::memcpy(data, &count, 4);
  return true;
}
extern "C" TEST_EXPORT bool retro_unserialize(const void* data, std::size_t size) {
  if (size < 4) return false;
  uint32_t count;
  std::memcpy(&count, data, 4);
  frames = count;
  return true;
}
extern "C" TEST_EXPORT void* retro_get_memory_data(unsigned) { return nullptr; }
extern "C" TEST_EXPORT std::size_t retro_get_memory_size(unsigned) { return 0; }
extern "C" TEST_EXPORT bool retro_load_game_special(unsigned, const void*, std::size_t) { return false; }
extern "C" TEST_EXPORT void retro_unload_game() {}
extern "C" TEST_EXPORT void retro_cheat_reset() {}
extern "C" TEST_EXPORT void retro_cheat_set(unsigned, bool, const char*) {}

extern "C" TEST_EXPORT bool retro_load_game(const GameInfo* info) {
  if (!info) return false;
  const auto name = Name(info->path);
  mode = name == "crash.cue" ? Mode::kCrash : name == "hang.cue" ? Mode::kHang : Mode::kFrames;
  unsigned format = 1;
  environment(kPixelFormat, &format);
  return true;
}

extern "C" TEST_EXPORT void retro_run() {
  const auto frame = ++frames;
  if (frame > 30 && mode == Mode::kCrash) *static_cast<volatile int*>(nullptr) = 1;
  if (frame > 30 && mode == Mode::kHang) {
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (poll) poll();
  uint32_t pixels[kWidth * kHeight];
  for (auto& pixel : pixels) pixel = 0x00202020u;
  for (unsigned port = 0; port < kPorts; ++port) {
    const auto buttons = Buttons(port);
    pixels[port] = 0x00000080u | (static_cast<uint32_t>(buttons & 0xFF) << 16) |
                   (static_cast<uint32_t>(buttons >> 8) << 8);
  }
  pixels[kWidth] = 0x00800000u | (frame & 0xFFFF);
  // Each port's device: red is its low byte, green its high byte.
  for (unsigned port = 0; port < kPorts; ++port) {
    const auto device = devices[port].load();
    pixels[kWidth + 1 + port] = 0x00000040u | ((device & 0xFF) << 16) | (((device >> 8) & 0xFF) << 8);
  }
  video(pixels, kWidth, kHeight, kWidth * 4);
  int16_t silence[800 * 2] = {};
  audio(silence, 800);
}
