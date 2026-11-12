#include "libretro_abi.h"
#include "retro_frame.h"
#include "retro_library.h"
#include "retro_session.h"
#include "ps3_network_policy.h"
#include "retro_test_support.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace retro = anyps5::retro;
using namespace retro::test;

namespace {
void CheckLibrary(const std::filesystem::path& path) {
  const retro::DynamicLibrary library(path);
  Check(library.Get<unsigned (*)()>("retro_api_version")() == 1, "Wrong Libretro API version");
  const retro::GameInfo game{"test.cue", nullptr, 0, "test metadata"};
  Check(library.Get<bool (*)(const retro::GameInfo*)>("retro_load_game")(&game), "Wrong game info ABI");

  bool missing_symbol = false;
  try {
    library.Get<void (*)()>("retro_unavailable");
  } catch (const std::runtime_error& error) {
    missing_symbol = std::string_view(error.what()).find("retro_unavailable") != std::string_view::npos;
  }
  Check(missing_symbol, "A missing export must fail with its name");
  Check(library.Get<void (*)()>("retro_unavailable", false) == nullptr,
        "An absent optional hook did not return null");

  bool missing_library = false;
  try {
    const retro::DynamicLibrary unavailable(std::filesystem::path(path).concat(".missing"));
  } catch (const std::runtime_error&) {
    missing_library = true;
  }
  Check(missing_library, "A missing library must fail");
}

void CheckFrames() {
  // Two one-pixel rows with padding. The first is red and the second is green.
  const uint8_t xrgb[] = {0, 0, 255, 0, 12, 34, 56, 78, 0, 255, 0, 0};
  const std::vector<uint8_t> red_green{255, 0, 0, 255, 0, 255, 0, 255};
  const auto frame = retro::DecodeSoftwareFrame(xrgb, 1, 2, 8, retro::kXrgb8888);
  Check(frame && frame->width == 1 && frame->height == 2 && frame->pixels == red_green, "Wrong XRGB8888 pitch or color");

  const uint8_t rgb565[] = {0x00, 0xf8, 0xe0, 0x07};
  const auto rgb = retro::DecodeSoftwareFrame(rgb565, 2, 1, 4, retro::kRgb565);
  Check(rgb && rgb->pixels == red_green, "Wrong RGB565 color");

  const uint8_t rgb1555[] = {0x00, 0x7c, 0xe0, 0x03};
  const auto rgb15 = retro::DecodeSoftwareFrame(rgb1555, 2, 1, 4, retro::kRgb1555);
  Check(rgb15 && rgb15->pixels == red_green, "Wrong 0RGB1555 color");

  Check(!retro::DecodeSoftwareFrame(xrgb, 1, 2, 2, retro::kXrgb8888), "Short pitch accepted");
  Check(!retro::DecodeSoftwareFrame(xrgb, 0, 2, 8, retro::kXrgb8888), "Empty frame accepted");
  Check(!retro::DecodeSoftwareFrame(nullptr, 1, 2, 8, retro::kXrgb8888), "Null frame accepted");
  Check(!retro::DecodeSoftwareFrame(xrgb, 4097, 2, 8, retro::kXrgb8888), "Oversized frame accepted");
}
void CheckPs3Policy() {
  retro::ApplyPs3OfflinePolicy(nullptr, false);
  retro::ApplyPs3OfflinePolicy([](unsigned mode, const char* xfloat, const char* dns, bool upnp) {
    return mode == 0 && !xfloat && !dns && !upnp;
  }, true);
  retro::Ps3NetworkPolicy online{"strict", "192.0.2.1", false, "fixture.invalid", "certificate",
                                 {"TEST00001", "01.00", "patch", "ppu", {"required"}, {2}}};
  const auto accepts = [](const char* xfloat, const char* dns, bool upnp, const char* host,
                          const char* certificate, const char* title, const char* version, const char* digest,
                          const char* ppu, unsigned count, const char* const* names, const unsigned* writes) {
    return std::string_view(xfloat) == "strict" && std::string_view(dns) == "192.0.2.1" && !upnp &&
           std::string_view(host) == "fixture.invalid" && std::string_view(certificate) == "certificate" &&
           std::string_view(title) == "TEST00001" && std::string_view(version) == "01.00" &&
           std::string_view(digest) == "patch" && std::string_view(ppu) == "ppu" &&
           count == 1 && std::string_view(names[0]) == "required" && writes[0] == 2;
  };
  retro::ApplyPs3OnlinePolicy(accepts, online);
  const auto reject = [](const auto& call, const char* message) {
    bool rejected = false;
    try {
      call();
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Check(rejected, message);
  };
  reject([] { retro::ApplyPs3OfflinePolicy(nullptr, true); }, "Isolated PS3 boot accepted a missing offline export");
  reject([&] { retro::ApplyPs3OnlinePolicy(nullptr, online); }, "PS3 online boot accepted a missing attestation export");
  online.upnp = true;
  reject([&] { retro::ApplyPs3OnlinePolicy(accepts, online); }, "PS3 online boot ignored core rejection");
  online.protection.writes.clear();
  reject([&] { retro::ApplyPs3OnlinePolicy(accepts, online); }, "PS3 online boot accepted mismatched patch entries");
}

void CheckSession(const std::filesystem::path& path) {
  const retro::DynamicLibrary library(path);
  const auto lifecycle = library.Get<unsigned (*)()>("test_lifecycle_count");
  const retro::SessionConfig config{path.u8string(), "test.cue", "", "", "", {}};
  retro::Session session(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  session.Start();
  WaitFor([&] { return session.Status().ready; }, "Session did not produce a frame");
  Check(lifecycle() == 2, "Expected init and load");
  Check(!session.Status().discs, "A single-disc session should not offer disc changes");

  retro::Session busy(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  busy.Start();
  busy.Join();
  Check(!busy.Status().error.empty() && lifecycle() == 2, "Concurrent session did not reject C callback reuse");

  session.SetInput(0x8021, {});
  WaitFor([&] {
    const auto frame = session.AcquireFrame();
    return frame && frame->pixels == std::vector<uint8_t>{255, 0, 0, 255};
  }, "Session did not deliver the packed player input mask");
  session.SetInput(1, 0x0010, {});
  WaitFor([&] {
    const auto frame = session.AcquireFrame();
    return frame && frame->pixels == std::vector<uint8_t>{0, 0, 0, 255};
  }, "Session did not deliver the second player's input to port 2");
  session.SetInput(1, 0, {});
  WaitFor([&] {
    const auto frame = session.AcquireFrame();
    return frame && frame->pixels == std::vector<uint8_t>{255, 0, 0, 255};
  }, "Session did not release the second player's input");
  session.SetInput(4, 0x0010, {});
  const auto active = session.Status();
  Check(active.presented && active.telemetry.audio_frames > 0 && active.telemetry.audio_rate == 48000.0,
        "Session did not report video and audio");
  session.Pause(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto paused_steps = session.Status().telemetry.steps;
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  Check(session.Status().telemetry.steps == paused_steps, "Pause still advanced the core");
  session.Pause(false);
  WaitFor([&] { return session.Status().telemetry.steps > paused_steps; }, "Resume did not advance the core");
  session.Stop();
  session.Join();
  Check(session.ended() && lifecycle() == 4 && session.Status().error.empty(), "Expected unload and deinit");
}
void CheckInitialDisc(const std::filesystem::path& path) {
  const retro::SessionConfig config{
      path.u8string(), "test.m3u", "", "", "", {}, retro::InitialDisc{1, path.u8string()}};
  retro::Session session(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  session.Start();
  WaitFor([&] {
    const auto status = session.Status();
    if (status.ended) throw std::runtime_error(status.error);
    return status.ready;
  }, "Disc 2 did not boot");
  const auto discs = session.Status().discs;
  Check(discs && discs->inserted == 1, "The requested disc was not inserted at boot");
  session.Stop();
  session.Join();
  Check(session.Status().error.empty(), "Selected-disc session did not stop cleanly");
}

void CheckDiscSwap(const std::filesystem::path& path, bool legacy = false) {
  const retro::DynamicLibrary library(path);
  const auto lifecycle = library.Get<unsigned (*)()>("test_lifecycle_count");
  const auto wrong_thread = library.Get<bool (*)()>("test_wrong_disk_thread");
  const auto run_count = library.Get<unsigned (*)()>("test_run_count");
  const auto fail_next_recovery = library.Get<void (*)()>("test_fail_next_recovery");
  const auto initial = lifecycle();
  const retro::SessionConfig config{path.u8string(), "test.m3u", "", "", "", {}};
  retro::Session session(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  session.Start();
  WaitFor([&] { return session.Status().ready; }, "Multi-disc session did not start");
  const auto first = session.Status().discs;
  const auto expected_labels = legacy ? std::vector<std::string>(3)
                                      : std::vector<std::string>{"Disc A", "Disc B", "Disc C"};
  Check(first && first->labels == expected_labels && first->inserted == 0,
        "Expected three ordered discs and the first inserted");
  Check(!session.ChangeDisc(3), "Invalid disc index was queued");

  session.Pause(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto steps = run_count();
  Check(session.ChangeDisc(1), "Valid disc change was not queued");
  WaitFor([&] {
    const auto discs = session.Status().discs;
    return discs && !discs->busy && discs->inserted == 1;
  }, "Disc switch did not complete while paused");
  Check(!session.Status().discs->failed && lifecycle() == initial + 2 &&
        run_count() == steps, "Disc switch restarted or advanced the game");

  Check(session.ChangeDisc(2), "Unavailable disc must be attempted on the worker");
  WaitFor([&] {
    const auto discs = session.Status().discs;
    return discs && !discs->busy && discs->failed;
  }, "Failed disc switch was not reported");
  Check(session.Status().discs->inserted == 1, "Failed insertion did not restore the previous disc");

  fail_next_recovery();
  Check(session.ChangeDisc(2), "Second insertion failure was not queued");
  WaitFor([&] {
    const auto discs = session.Status().discs;
    return discs && !discs->busy && discs->failed;
  }, "Failed recovery was not reported");
  Check(!session.Status().discs->inserted, "Failed recovery incorrectly reported a disc inserted");

  Check(session.ChangeDisc(0), "Retry disc switch was not queued");
  WaitFor([&] {
    const auto discs = session.Status().discs;
    return discs && !discs->busy && !discs->failed && discs->inserted == 0;
  }, "Disc switch did not recover after retry");
  Check(run_count() == steps, "Recovery advanced the paused game");
  Check(!wrong_thread(), "Core disk callbacks ran outside the native worker");
  session.Stop();
  session.Join();
  Check(lifecycle() == initial + 4, "Multi-disc game did not close its original session");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "Pass the path to the test core");
    CheckLibrary(std::filesystem::u8path(argv[1]));
    CheckFrames();
    CheckPs3Policy();
    CheckSession(std::filesystem::u8path(argv[1]));
    const auto path = std::filesystem::u8path(argv[1]);
    const retro::DynamicLibrary library(path);
    CheckInitialDisc(path);
    CheckDiscSwap(path);
    library.Get<void (*)()>("test_use_legacy_disk_control")();
    CheckDiscSwap(path, true);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
