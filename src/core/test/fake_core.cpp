#include <atomic>
#include <array>
#include <algorithm>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace {
#ifdef TEST_CHECKPOINT
uint32_t checkpoint_card = 7;
bool checkpoint_loaded = false;
std::array<uint8_t, 2 * 1024 * 1024> match_ram{};
std::atomic<bool> checkpoint_hold_tray{false}, checkpoint_tray_entered{false};
std::atomic<bool> checkpoint_hold_export{false}, checkpoint_export_entered{false};
#endif
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

void (*video)(const void*, unsigned, unsigned, std::size_t);
int16_t (*input)(unsigned, unsigned, unsigned, unsigned);
std::size_t (*audio)(const int16_t*, std::size_t);
// Independent declarations keep the fixture an ABI check for the frontend.
struct DiskControl {
  bool (*set_eject)(bool);
  bool (*get_eject)();
  unsigned (*get_index)();
  bool (*set_index)(unsigned);
  unsigned (*count)();
  bool (*replace)(unsigned, const GameInfo*);
  bool (*add)();
  bool (*set_initial)(unsigned, const char*);
  bool (*get_path)(unsigned, char*, std::size_t);
  bool (*get_label)(unsigned, char*, std::size_t);
};

bool (*environment)(unsigned, void*);
struct Variable { const char* key; const char* value; };
#ifdef TEST_LAN
struct LanDescriptor {
  void* opaque;
  uint8_t mac[6];
  bool (*send)(void*, const void*, std::size_t);
  std::size_t (*recv)(void*, void*, std::size_t);
};
std::mutex lan_mutex;
LanDescriptor lan{};
bool lan_bound = false;
std::atomic<bool> lan_loaded{false}, lan_hold{false};
std::atomic<unsigned> lan_seen{0}, lan_sent{0};
#endif
std::atomic<unsigned> lifecycle{0};
std::atomic<bool> wrong_disk_thread{false}, fail_next_recovery{false};
std::atomic<unsigned> run_count{0};
std::thread::id worker;
bool multidisc = false;
bool legacy_disk_control = false;
bool ejected = false;
unsigned image = 0;
std::string initial_path;

void CheckDiskThread() {
  if (std::this_thread::get_id() != worker) wrong_disk_thread = true;
}

bool SetEject(bool value) {
  CheckDiskThread();
#ifdef TEST_CHECKPOINT
  if (value && checkpoint_hold_tray) {
    checkpoint_tray_entered = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (checkpoint_hold_tray && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
#endif
  // Disc three simulates missing media. Closing the tray with the old
  // disc is still allowed so the frontend can recover.
  if (!value && image == 2) return false;
  if (!value && image == 1 && fail_next_recovery.exchange(false)) return false;
  ejected = value;
  return true;
}

bool GetEject() { CheckDiskThread(); return ejected; }
unsigned GetIndex() { CheckDiskThread(); return image; }
bool SetIndex(unsigned index) {
  CheckDiskThread();
  if (!ejected || index >= 3) return false;
  image = index;
  return true;
}
unsigned Count() { CheckDiskThread(); return multidisc ? 3 : 1; }
bool Replace(unsigned, const GameInfo*) { return false; }
bool Add() { return false; }
bool GetLabel(unsigned index, char* label, std::size_t size) {
  CheckDiskThread();
  if (index >= 3 || size < 7) return false;
  const char* labels[] = {"Disc A", "Disc B", "Disc C"};
  std::memcpy(label, labels[index], 7);
  return true;
}
bool SetInitialImage(unsigned index, const char* path) {
  CheckDiskThread();
  if (run_count != 0 || index >= 3) return false;
  image = index;
  initial_path = path;
  return true;
}
bool GetImagePath(unsigned index, char* path, std::size_t size) {
  CheckDiskThread();
  if (!multidisc || index != image || initial_path.size() >= size) return false;
  std::memcpy(path, initial_path.c_str(), initial_path.size() + 1);
  return true;
}
void RegisterDiskControl() {
  unsigned version = 0;
  DiskControl callbacks{
      SetEject, GetEject, GetIndex, SetIndex, Count, Replace, Add, SetInitialImage, GetImagePath, GetLabel};
  if (!legacy_disk_control && environment(57, &version) && version >= 1)
    environment(58, &callbacks);
  else
    environment(13, &callbacks);
}
}  // namespace

extern "C" TEST_EXPORT unsigned retro_api_version() { return 1; }

extern "C" TEST_EXPORT bool retro_load_game(const GameInfo* info) {
  if (!info || info->data || info->size != 0) return false;
  const bool playlist = std::strcmp(info->path, "test.m3u") == 0;
  if (!playlist && std::strcmp(info->path, "test.cue") != 0) return false;
  if (info->meta) return std::strcmp(info->meta, "test metadata") == 0;
#ifdef TEST_LAN
  Variable eth{"pcsx2_dev9_eth", nullptr};
  if (!environment(15, &eth) || !eth.value || std::strcmp(eth.value, "enabled")) return false;
  { std::lock_guard<std::mutex> lock(lan_mutex); if (!lan_bound) return false; }
  lan_loaded = true;
#endif
  multidisc = playlist;
#ifdef TEST_CHECKPOINT
  checkpoint_loaded = true;
#endif
  ++lifecycle;
  return true;
}

extern "C" TEST_EXPORT void retro_init() {
  ++lifecycle;
  worker = std::this_thread::get_id();
  wrong_disk_thread = false;
  image = 0;
  initial_path.clear();
  ejected = false;
  run_count = 0;
#ifdef TEST_CHECKPOINT
  checkpoint_card = 7;
  checkpoint_loaded = false;
  match_ram.fill(0);
  const Variable variables[]{
      {"swanstation_CPU_ExecutionMode", "CPU; Recompiler|CachedInterpreter"},
      {"swanstation_GPU_Renderer", "Renderer; Software"},
      {"swanstation_GPU_ResolutionScale", "Scale; 1"},
      {"swanstation_CPU_RecompilerBlockLinking", "Linking; true|false"},
      {"swanstation_GPU_UseThread", "Thread; true|false"},
      {"swanstation_BIOS_PatchFastBoot", "Fast boot; true|false"},
      {"swanstation_CDROM_ReadThread", "Read thread; false|true"},
      {nullptr, nullptr}};
  environment(16, const_cast<Variable*>(variables));
#endif
  if (!legacy_disk_control) RegisterDiskControl();
#ifdef TEST_LAN
  lan_seen = lan_sent = 0;
  lan_hold = false;
  const Variable variables[]{{"pcsx2_dev9_eth", "Networking; disabled|enabled"}, {nullptr, nullptr}};
  environment(16, const_cast<Variable*>(variables));
#endif
}
extern "C" TEST_EXPORT void retro_unload_game() {
#ifdef TEST_CHECKPOINT
  checkpoint_loaded = false;
#endif
#ifdef TEST_LAN
  lan_loaded = false;
#endif
  ++lifecycle;
}
extern "C" TEST_EXPORT void retro_deinit() { ++lifecycle; }
extern "C" TEST_EXPORT unsigned test_lifecycle_count() { return lifecycle; }

extern "C" TEST_EXPORT bool test_wrong_disk_thread() { return wrong_disk_thread; }
extern "C" TEST_EXPORT void test_use_legacy_disk_control() { legacy_disk_control = true; }

extern "C" TEST_EXPORT unsigned test_run_count() { return run_count; }
extern "C" TEST_EXPORT void test_fail_next_recovery() { fail_next_recovery = true; }

extern "C" TEST_EXPORT void retro_set_environment(decltype(environment) callback) {
  environment = callback;
  if (legacy_disk_control) RegisterDiskControl();
}
extern "C" TEST_EXPORT void retro_set_video_refresh(decltype(video) callback) { video = callback; }
extern "C" TEST_EXPORT void retro_set_input_state(decltype(input) callback) { input = callback; }
extern "C" TEST_EXPORT void retro_set_input_poll(void (*)()) {}
extern "C" TEST_EXPORT void retro_set_audio_sample(void (*)(int16_t, int16_t)) {}
extern "C" TEST_EXPORT void retro_set_audio_sample_batch(decltype(audio) callback) { audio = callback; }

extern "C" TEST_EXPORT void retro_get_system_av_info(AvInfo* info) {
  *info = {1, 1, 1, 1, 1.0f, 60.0, 48000.0};
}

extern "C" TEST_EXPORT void retro_run() {
  ++run_count;
#ifdef TEST_CHECKPOINT
  for (unsigned port = 0; port < 2; ++port) {
    match_ram[port * 2] = static_cast<uint8_t>(input(port, 1, 0, 256));
    match_ram[port * 2 + 1] = static_cast<uint8_t>(input(port, 5, 0, 0));
  }
  const auto cursor = run_count.load();
  for (unsigned i = 0; i < 4; ++i) match_ram[4 + i] = static_cast<uint8_t>(cursor >> (8 * i));
  match_ram[8] |= match_ram[0] & 4;
#endif
  const auto packed = static_cast<uint16_t>(input(0, 1, 0, 256));
  const bool pad_ready = environment(51 | 0x10000, nullptr) &&
                         packed == 0x8021 && input(0, 1, 0, 0) &&
                         input(1, 1, 0, 256) == 0;
  const uint8_t pixel[] = {0, static_cast<uint8_t>(pad_ready ? 0x7c : 0)};
  const int16_t stereo[] = {123, -123};
  video(pixel, 1, 1, sizeof(pixel));
  audio(stereo, 1);
#ifdef TEST_LAN
  LanDescriptor descriptor;
  bool bound;
  { std::lock_guard<std::mutex> lock(lan_mutex); descriptor = lan; bound = lan_bound; }
  if (bound && !lan_hold) {
    std::array<uint8_t, 1514> packet{};
    const auto size = descriptor.recv(descriptor.opaque, packet.data(), packet.size());
    if (size >= 14) {
      ++lan_seen;
      std::copy(packet.begin() + 6, packet.begin() + 12, packet.begin());
      std::copy(std::begin(descriptor.mac), std::end(descriptor.mac), packet.begin() + 6);
      if (descriptor.send(descriptor.opaque, packet.data(), size)) ++lan_sent;
    }
  }
#endif
}

#ifdef TEST_CHECKPOINT
extern "C" TEST_EXPORT void retro_set_controller_port_device(unsigned, unsigned) { CheckDiskThread(); }
extern "C" TEST_EXPORT void test_checkpoint_hold_tray(bool hold) {
  if (hold) checkpoint_tray_entered = false;
  checkpoint_hold_tray = hold;
}
extern "C" TEST_EXPORT bool test_checkpoint_tray_entered() { return checkpoint_tray_entered; }
extern "C" TEST_EXPORT void test_checkpoint_hold_export(bool hold) {
  if (hold) checkpoint_export_entered = false;
  checkpoint_hold_export = hold;
}
extern "C" TEST_EXPORT bool test_checkpoint_export_entered() { return checkpoint_export_entered; }
extern "C" TEST_EXPORT void* retro_get_memory_data(unsigned kind) {
  CheckDiskThread();
  if (kind == 2) return match_ram.data();
  return kind == 0 ? &checkpoint_card : nullptr;
}
extern "C" TEST_EXPORT size_t retro_get_memory_size(unsigned kind) {
  CheckDiskThread();
  if (kind == 2) return match_ram.size();
  return kind == 0 ? sizeof(checkpoint_card) : 0;
}
extern "C" TEST_EXPORT size_t retro_serialize_size() { return 8; }
extern "C" TEST_EXPORT size_t retro_anyps5_export_checkpoint_v1(void* bytes, size_t capacity, const uint8_t*) {
  CheckDiskThread();
  if (!checkpoint_loaded || capacity < 8) return 0;
  const uint32_t values[]{run_count, checkpoint_card};
  std::memcpy(bytes, values, sizeof(values));
  return sizeof(values);
}
extern "C" TEST_EXPORT size_t retro_anyps5_export_blank_card_checkpoint_v1(void* bytes, size_t capacity, const uint8_t*) {
  CheckDiskThread();
  if (checkpoint_hold_export) {
    checkpoint_export_entered = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (checkpoint_hold_export && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!checkpoint_loaded || capacity < 8) return 0;
  const uint32_t values[]{run_count, 0};
  std::memcpy(bytes, values, sizeof(values));
  return sizeof(values);
}
extern "C" TEST_EXPORT bool retro_anyps5_import_checkpoint_v1(const void* bytes, size_t size, const uint8_t*) {
  CheckDiskThread();
  if (!checkpoint_loaded || size != 8) return false;
  uint32_t values[2];
  std::memcpy(values, bytes, sizeof(values));
  run_count = values[0];
  checkpoint_card = values[1];
  return true;
}
#endif

#ifndef TEST_CHECKPOINT
extern "C" TEST_EXPORT void* retro_get_memory_data(unsigned) { CheckDiskThread(); return nullptr; }
extern "C" TEST_EXPORT size_t retro_get_memory_size(unsigned) { CheckDiskThread(); return 0; }
#endif

#ifdef TEST_LAN
extern "C" TEST_EXPORT bool retro_anyps5_dev9_bind_v1(const LanDescriptor* descriptor) {
  CheckDiskThread();
  std::lock_guard<std::mutex> lock(lan_mutex);
  if (lan_loaded) return false;
  if (descriptor && (lan_bound || !descriptor->send || !descriptor->recv)) return false;
  lan = descriptor ? *descriptor : LanDescriptor{};
  lan_bound = descriptor != nullptr;
  return true;
}
extern "C" TEST_EXPORT unsigned test_lan_seen() { return lan_seen; }
extern "C" TEST_EXPORT unsigned test_lan_sent() { return lan_sent; }
extern "C" TEST_EXPORT void test_lan_hold(bool value) { lan_hold = value; }
extern "C" TEST_EXPORT bool test_lan_bound() {
  std::lock_guard<std::mutex> lock(lan_mutex);
  return lan_bound;
}
#endif
