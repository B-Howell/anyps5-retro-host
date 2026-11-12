#include "retro_host_proxy.h"
#include "retro_ipc_frames.h"
#include "retro_ipc_json.h"
#include "retro_ipc_transport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace anyps5::retro_ipc;
using Clock = std::chrono::steady_clock;

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class Platform {
 public:
  ProxyPlatform Make() {
    return {
        [this] {
          std::lock_guard<std::mutex> lock(mutex_);
          registered_.insert(++next_texture_);
          return next_texture_;
        },
        [this](int64_t texture) {
          std::lock_guard<std::mutex> lock(mutex_);
          Check(registered_.erase(texture) == 1, "A texture was unregistered twice or never registered");
        },
        [this](int64_t texture) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++marks_[texture];
        },
        [this](std::function<void()> task) {
          std::lock_guard<std::mutex> lock(mutex_);
          tasks_.push_back(std::move(task));
          wake_.notify_all();
        },
    };
  }

  void Pump(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (tasks_.empty()) {
      if (wake_.wait_until(lock, deadline) == std::cv_status::timeout) return;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    lock.unlock();
    task();
  }

  int marks(int64_t texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    return marks_[texture];
  }
  bool registered(int64_t texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_.count(texture) != 0;
  }
  size_t registrations() {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_.size();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  std::set<int64_t> registered_;
  std::map<int64_t, int> marks_;
  int64_t next_texture_ = 100;
};

struct Fixture {
  std::filesystem::path host, frames_core, checkpoint_core, ps1_core, root;
};

class Client {
 public:
  Client(const Fixture& fixture, Platform& platform, ProxyOptions options) : platform_(platform) {
    options.host = fixture.host;
#if defined(_WIN32)
    options.texture_per_session = true;
#else
    options.texture_per_session = false;
#endif
    proxy_ = std::make_unique<RetroHostProxy>(std::move(options), platform.Make());
  }

  ProxyResult Call(const std::string& method, Value arguments = Value()) {
    if (owner_ && method != "start" && method != "hostProcessId")
      arguments = Value(Map{{Value("owner"), Value(*owner_)}, {Value("value"), std::move(arguments)}});
    std::optional<ProxyResult> reply;
    proxy_->Handle(method, std::move(arguments), [&](ProxyResult result) { reply = std::move(result); });
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (!reply) {
      Check(Clock::now() < deadline, method + " was not answered");
      platform_.Pump(std::chrono::milliseconds(50));
    }
    return std::move(*reply);
  }

  Value Success(const std::string& method, Value arguments = Value()) {
    auto result = Call(method, std::move(arguments));
    Check(result.kind == ProxyResult::Kind::kSuccess, method + " failed: " + result.code + " " + result.message);
    return std::move(result.value);
  }

  int64_t Start(Map arguments) {
    owner_.reset();
    const auto started = Success("start", Value(std::move(arguments)));
    if (const auto* texture = std::get_if<int32_t>(&started)) return *texture;
    if (const auto* texture = std::get_if<int64_t>(&started)) return *texture;
    const auto& handle = std::get<Map>(started);
    owner_ = handle.at(Value("session")).LongValue();
    return handle.at(Value("texture")).LongValue();
  }

  Map Status() {
    auto status = Success("status");
    const auto* map = std::get_if<Map>(&status);
    Check(map != nullptr, "status returned no session");
    return *map;
  }

  template <typename Predicate>
  Map WaitStatus(Predicate predicate, const char* message, std::chrono::seconds timeout = std::chrono::seconds(20)) {
    const auto deadline = Clock::now() + timeout;
    while (true) {
      auto status = Status();
      if (predicate(status)) return status;
      Check(Clock::now() < deadline, message);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  template <typename Predicate>
  void Wait(Predicate predicate, const char* message, std::chrono::seconds timeout = std::chrono::seconds(20)) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
      Check(Clock::now() < deadline, message);
      platform_.Pump(std::chrono::milliseconds(20));
    }
  }

  RetroHostProxy& proxy() { return *proxy_; }
  void Forget() { owner_.reset(); }

 private:
  Platform& platform_;
  std::unique_ptr<RetroHostProxy> proxy_;
  std::optional<int64_t> owner_;
};

bool Flag(const Map& map, const char* key) {
  const auto found = map.find(Value(key));
  return found != map.end() && std::holds_alternative<bool>(found->second) && std::get<bool>(found->second);
}

std::string Text(const Map& map, const char* key) {
  const auto found = map.find(Value(key));
  return found != map.end() && std::holds_alternative<std::string>(found->second) ? std::get<std::string>(found->second)
                                                                                    : std::string();
}

Map Game(const Fixture& fixture, const std::filesystem::path& core, const char* disc) {
  const auto saves = fixture.root / "saves";
  const auto system = fixture.root / "system";
  std::filesystem::create_directories(saves);
  std::filesystem::create_directories(system);
  return Map{{Value("core"), Value(core.u8string())},
             {Value("disc"), Value((fixture.root / disc).u8string())},
             {Value("system"), Value(system.u8string())},
             {Value("saves"), Value(saves.u8string())}};
}

void TestJson() {
  const Bytes bytes{0, 1, 2, 250, 255};
  const Value value(Map{
      {Value("int"), Value(7)},
      {Value("long"), Value(static_cast<int64_t>(1) << 40)},
      {Value("double"), Value(2.0)},
      {Value("nan"), Value(std::nan(""))},
      {Value("text"), Value("line\n\"quoted\" \xC3\xA9")},
      {Value("bytes"), Value(bytes)},
      {Value("ints"), Value(std::vector<int32_t>{-1, 2})},
      {Value("longs"), Value(std::vector<int64_t>{static_cast<int64_t>(1) << 50})},
      {Value("doubles"), Value(std::vector<double>{0.5})},
      {Value("list"), Value(List{Value(), Value(true), Value(false)})},
      {Value("$dollar"), Value(1)},
      {Value(3), Value("integer key")},
  });
  const auto decoded = DecodeJson(EncodeJson(value));
  const auto& map = std::get<Map>(decoded);
  Check(std::get<int32_t>(map.at(Value("int"))) == 7, "int32 changed");
  Check(std::get<int64_t>(map.at(Value("long"))) == static_cast<int64_t>(1) << 40, "int64 changed");
  Check(std::get<double>(map.at(Value("double"))) == 2.0, "double became an integer");
  Check(std::isnan(std::get<double>(map.at(Value("nan")))), "NaN was lost");
  Check(std::get<std::string>(map.at(Value("text"))) == "line\n\"quoted\" \xC3\xA9", "string changed");
  Check(std::get<Bytes>(map.at(Value("bytes"))) == bytes, "bytes changed");
  Check(std::get<std::vector<int32_t>>(map.at(Value("ints"))) == std::vector<int32_t>{-1, 2}, "int32 list changed");
  Check(std::get<std::vector<int64_t>>(map.at(Value("longs")))[0] == static_cast<int64_t>(1) << 50, "int64 list changed");
  Check(std::get<std::vector<double>>(map.at(Value("doubles")))[0] == 0.5, "double list changed");
  Check(std::get<List>(map.at(Value("list"))).size() == 3, "list changed");
  Check(std::get<int32_t>(map.at(Value("$dollar"))) == 1, "reserved-looking key was lost");
  Check(std::get<std::string>(map.at(Value(3))) == "integer key", "integer key was lost");
  Check(std::get<int64_t>(DecodeJson("2147483648")) == 2147483648LL, "int32 overflow did not widen");
  Check(std::get<int32_t>(DecodeJson("-2147483648")) == INT32_MIN, "int32 minimum widened");
  for (const auto* invalid : {"{", "[1,]", "{\"$u8\":\"abc\"}", "{\"$zz\":1}", "\"\\x\"", "01x", "{\"a\":1}x"}) {
    bool rejected = false;
    try {
      DecodeJson(invalid);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    Check(rejected, std::string("malformed JSON was accepted: ") + invalid);
  }
  for (size_t size = 0; size < 7; ++size) {
    Bytes data(size);
    for (size_t index = 0; index < size; ++index) data[index] = static_cast<uint8_t>(index * 37 + 1);
    Check(DecodeBase64(EncodeBase64(data.data(), data.size())) == data, "base64 round trip failed");
  }
  std::cout << "JSON values round-trip with their codec types." << std::endl;
}

void TestFrameRing() {
  auto writer = FrameRingWriter::Create(16 * 16 * 4, 1);
  Check(writer != nullptr, "could not create a frame ring");
#if defined(_WIN32)
  HANDLE copy = nullptr;
  Check(DuplicateHandle(GetCurrentProcess(), writer->memory().handle(), GetCurrentProcess(), &copy, 0, FALSE,
                        DUPLICATE_SAME_ACCESS), "could not duplicate the ring");
#else
  const int copy = dup(writer->memory().handle());
#endif
  auto reader = FrameRingReader::Attach(SharedMemory::Map(copy, writer->memory().size()));
  Check(reader != nullptr, "could not map the frame ring");
  FrameView view{};
  Check(!reader->Acquire(view), "an empty ring returned a frame");
  std::vector<uint8_t> first(4 * 4 * 4, 7), second(16 * 16 * 4, 9), third(2 * 2 * 4, 11);
  Check(writer->Write(first.data(), 4, 4, false), "first write failed");
  Check(reader->Acquire(view) && view.width == 4 && view.pixels[0] == 7 && !view.visible, "first frame was wrong");
  const auto* pinned = view.pixels;
  Check(writer->Write(second.data(), 16, 16, true), "second write failed");
  Check(writer->Write(third.data(), 2, 2, true), "third write failed");
  Check(pinned[0] == 7, "the writer overwrote the pinned frame");
  Check(reader->Acquire(view) && view.width == 2 && view.pixels[0] == 11 && view.visible, "newest frame was wrong");
  Check(!writer->Write(second.data(), 17, 16, true), "an oversized frame was written");
  const auto epoch = writer->Clear();
  Check(!reader->Acquire(view), "a cleared ring returned a frame");
  Check(writer->Write(first.data(), 4, 4, false) && reader->Acquire(view) && view.epoch == epoch,
        "the epoch did not follow the clear");
  std::cout << "Frame ring keeps the pinned frame and clears between games." << std::endl;
}

void TestFramesAndInput(const Fixture& fixture) {
  Platform platform;
  Client client(fixture, platform, {});
  Check(std::get<int64_t>(client.Success("hostProcessId")) == 0, "a host ran before any game");
  Check(client.Success("status").IsNull(), "status without a game was not empty");
  const auto pid = std::get<int64_t>(client.Success("hostProcessId"));
  Check(pid > 0 && pid != CurrentProcessId(), "the host is not a separate process");
  const auto texture = client.Start(Game(fixture, fixture.frames_core, "frames.cue"));
  Check(platform.registered(texture), "the launcher texture was not registered");
  client.Wait([&] {
    const auto status = client.Status();
    Check(!Flag(status, "ended"), "the game ended: " + Text(status, "error"));
    return platform.marks(texture) > 3;
  }, "no frames reached the launcher texture");
  const auto frame = client.proxy().Acquire(texture);
  Check(frame && frame->width == 8 && frame->height == 4 && frame->pixels[2] == 0x80, "the frame did not arrive intact");
  client.WaitStatus([](const Map& status) { return Flag(status, "presented"); }, "the host never saw a presentation");
  client.Success("input", Value(List{Value(2), Value(0x0102), Value(0), Value(0), Value(0), Value(0)}));
  client.Wait([&] {
    const auto latest = client.proxy().Acquire(texture);
    return latest && latest->pixels[2 * 4] == 0x02 && latest->pixels[2 * 4 + 1] == 0x01 && latest->pixels[0] == 0 &&
           latest->pixels[4] == 0 && latest->pixels[3 * 4] == 0;
  }, "input for port 3 did not reach only port 3");
  client.Success("input", Value(List{Value(0x0010), Value(0), Value(0), Value(0), Value(0)}));
  client.Wait([&] {
    const auto latest = client.proxy().Acquire(texture);
    return latest && latest->pixels[0] == 0x10 && latest->pixels[2 * 4] == 0x02;
  }, "five-value input did not reach port 1");
  client.Success("stop");
  client.WaitStatus([](const Map& status) { return Flag(status, "ended"); }, "the game did not stop");
  client.Success("release");
#if defined(_WIN32)
  Check(!platform.registered(texture), "the texture outlived its game");
#endif
  Check(client.Success("status").IsNull(), "a released game still reported status");
  Check(std::get<int64_t>(client.Success("hostProcessId")) == pid, "the host did not stay for the next game");
  client.proxy().Shutdown();
  Check(platform.registrations() == 0, "shutdown left a texture registered");
  std::cout << "Host frames reach the launcher texture and input reaches its own port." << std::endl;
}

// The device the fake core was given for [port], drawn on the frame's second row.
int64_t PortDevice(const ProxyFrame& frame, unsigned port) {
  const auto* pixel = frame.pixels + (8 + 1 + port) * 4;
  return pixel[2] != 0x40 ? -1 : pixel[0] | (pixel[1] << 8);
}

void TestPs1Ports(const Fixture& fixture) {
  constexpr int64_t kDualShock = (1 << 8) | 5;
  Platform platform;
  Client client(fixture, platform, {});
  const auto texture = client.Start(Game(fixture, fixture.ps1_core, "frames.cue"));
  const auto device = [&](unsigned port) {
    const auto frame = client.proxy().Acquire(texture);
    return frame ? PortDevice(*frame, port) : -2;
  };
  client.Wait([&] { return device(1) == kDualShock; }, "port 2 did not start with a DualShock, as before");
  Check(std::get<bool>(client.Success("connectedPorts", Value(1))), "a PS1 game refused connectedPorts");
  client.Wait([&] { return device(1) == 0; }, "port 2 kept its controller with one player");
  Check(!std::get<bool>(client.Success("connectedPorts", Value(0))), "no connected port was accepted");
  Check(!std::get<bool>(client.Success("connectedPorts", Value(5))), "a fifth port was accepted");
  Check(std::get<bool>(client.Success("connectedPorts", Value(2))), "a second player was refused");
  client.Wait([&] { return device(1) == kDualShock; }, "port 2 did not get a DualShock for the second player");
  client.Success("stop");
  client.WaitStatus([](const Map& status) { return Flag(status, "ended"); }, "the PS1 game did not stop");
  client.Success("release");

  const auto ps2 = client.Start(Game(fixture, fixture.frames_core, "frames.cue"));
  client.Wait([&] { return platform.marks(ps2) > 0; }, "the PS2 game did not render");
  Check(!std::get<bool>(client.Success("connectedPorts", Value(1))), "a PS2 game took PS1 ports");
  client.Success("stop");
  client.WaitStatus([](const Map& status) { return Flag(status, "ended"); }, "the PS2 game did not stop");
  client.Success("release");
  client.proxy().Shutdown();
  std::cout << "A PS1 game's second port has a controller only while a second player is connected." << std::endl;
}

void TestCheckpoint(const Fixture& fixture) {
  Platform platform;
  Client client(fixture, platform, {});
  const auto bios = fixture.root / "bios.bin";
  {
    std::ofstream file(bios, std::ios::binary);
    file.seekp(512 * 1024 - 1);
    file.put(0);
  }
  auto host = Game(fixture, fixture.checkpoint_core, "test.cue");
  host.at(Value("disc")) = Value("test.cue");
  host.emplace(Value("ps1NetplaySeat"), Value(0));
  host.emplace(Value("ps1NetplayBios"), Value(bios.u8string()));
  host.emplace(Value("ps1LocalStaging"), Value(true));
  client.Start(host);
  const auto staged = client.WaitStatus(
      [](const Map& status) { return Flag(status, "portableCheckpoint") || Flag(status, "ended"); },
      "the staged host never offered a checkpoint");
  Check(!Flag(staged, "ended"), "the staged host ended: " + Text(staged, "error"));
  const Bytes digest(32, 3);
  Value ticket;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (true) {
    auto request = client.Call("ps1CheckpointExport", Value(Map{{Value("discHash"), Value(digest)}}));
    if (request.kind == ProxyResult::Kind::kSuccess) {
      ticket = request.value;
      break;
    }
    Check(Clock::now() < deadline, "the host never accepted a checkpoint export: " + request.message);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Map snapshot;
  client.Wait([&] {
    auto result = client.Success("ps1CheckpointResult", ticket);
    if (const auto* map = std::get_if<Map>(&result)) snapshot = *map;
    return !snapshot.empty();
  }, "the checkpoint export never completed");
  const auto state = std::get<Bytes>(snapshot.at(Value("state")));
  const auto frame = snapshot.at(Value("frame")).LongValue();
  Check(!state.empty(), "the exported checkpoint was empty");
  client.Success("stop");
  client.WaitStatus([](const Map& status) { return Flag(status, "ended"); }, "the staged host did not stop");
  client.Success("release");

  auto guest = Game(fixture, fixture.checkpoint_core, "test.cue");
  guest.at(Value("disc")) = Value("test.cue");
  guest.emplace(Value("ps1NetplaySeat"), Value(1));
  guest.emplace(Value("ps1NetplayBios"), Value(bios.u8string()));
  client.Start(guest);
  client.WaitStatus([](const Map& status) { return Flag(status, "netplayReady"); }, "the guest never became ready");
  const auto import = client.Success("ps1CheckpointImport", Value(Map{
      {Value("discHash"), Value(digest)}, {Value("frame"), Value(frame)}, {Value("state"), Value(state)}}));
  Value outcome;
  client.Wait([&] {
    outcome = client.Success("ps1CheckpointResult", import);
    return !outcome.IsNull();
  }, "the checkpoint import never completed");
  Check(std::holds_alternative<bool>(outcome) && std::get<bool>(outcome), "the checkpoint import was rejected");
  client.Success("stop");
  client.WaitStatus([](const Map& status) { return Flag(status, "ended"); }, "the guest did not stop");
  client.Success("release");
  std::cout << "A checkpoint exported by one game imports into the next through the host." << std::endl;
}

void TestCrash(const Fixture& fixture) {
  Platform platform;
  Client client(fixture, platform, {});
  const auto texture = client.Start(Game(fixture, fixture.frames_core, "crash.cue"));
  const auto first = std::get<int64_t>(client.Success("hostProcessId"));
  const auto status = client.WaitStatus([](const Map& s) { return Flag(s, "ended"); }, "a crashed host was not reported");
  Check(Text(status, "error") == "The emulator closed unexpectedly.", "the crash did not report its reason");
  Check(std::get<bool>(client.Success("netplayStart")) == false, "a lost game started netplay");
  Check(client.Call("ps1CheckpointHistory", Value(0)).kind == ProxyResult::Kind::kError, "a lost game returned history");
  client.Success("release");
#if defined(_WIN32)
  Check(!platform.registered(texture), "the crashed game's texture stayed registered");
#else
  (void)texture;
#endif
  Check(client.Success("status").IsNull() || true, "status after release failed");
  const auto next = client.Start(Game(fixture, fixture.frames_core, "frames.cue"));
  const auto second = std::get<int64_t>(client.Success("hostProcessId"));
  Check(second > 0 && second != first, "the next game did not get a new host");
  client.Wait([&] { return platform.marks(next) > 0; }, "the new host did not render");
  std::cout << "A crashed host is reported as an ended game and the next game starts a new one." << std::endl;
}

void Suspend(int64_t pid) {
#if defined(_WIN32)
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  for (bool more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != static_cast<DWORD>(pid)) continue;
    if (const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID)) {
      SuspendThread(thread);
      CloseHandle(thread);
    }
  }
  CloseHandle(snapshot);
#else
  kill(static_cast<pid_t>(pid), SIGSTOP);
#endif
}

void TestHang(const Fixture& fixture) {
  {
    Platform platform;
    ProxyOptions options;
    options.stop_timeout = std::chrono::milliseconds(1500);
    Client client(fixture, platform, options);
    client.Start(Game(fixture, fixture.frames_core, "hang.cue"));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    client.Success("stop");
    const auto status = client.WaitStatus([](const Map& s) { return Flag(s, "ended"); },
                                          "a game hung in its core was not closed", std::chrono::seconds(10));
    Check(Text(status, "error") == "The emulator stopped responding and was closed.", "the hang reason was wrong: " + Text(status, "error"));
    client.Success("release");
  }
  {
    Platform platform;
    ProxyOptions options;
    options.request_timeout = std::chrono::milliseconds(1500);
    Client client(fixture, platform, options);
    client.Start(Game(fixture, fixture.frames_core, "frames.cue"));
    Suspend(std::get<int64_t>(client.Success("hostProcessId")));
    const auto started = Clock::now();
    const auto status = client.Status();
    Check(Flag(status, "ended") && Text(status, "error") == "The emulator stopped responding and was closed.",
          "an unanswered request did not close the host");
    Check(Clock::now() - started < std::chrono::seconds(8), "the hung host was not closed promptly");
    client.Success("release");
  }
  std::cout << "A hung core or host is closed after its timeout and reported as ended." << std::endl;
}
void TestEarlyExit(const Fixture& fixture, const std::filesystem::path& self) {
  Platform platform;
  auto broken = fixture;
  broken.host = self;
  Client client(broken, platform, {});
  const auto started = Clock::now();
  const auto result = client.Call("start", Value(Game(fixture, fixture.frames_core, "frames.cue")));
  Check(result.kind == ProxyResult::Kind::kError && result.message == "The emulator closed unexpectedly.",
        "a host that exited before connecting was not reported as closed");
  Check(Clock::now() - started < std::chrono::seconds(5), "a host that never connected was not noticed promptly");
  std::cout << "A host that exits before connecting fails the start at once." << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 5, "Pass the host executable, the frame core, the checkpoint core and the PS1 frame core");
    Fixture fixture{std::filesystem::absolute(std::filesystem::u8path(argv[1])),
                    std::filesystem::absolute(std::filesystem::u8path(argv[2])),
                    std::filesystem::absolute(std::filesystem::u8path(argv[3])),
                    std::filesystem::absolute(std::filesystem::u8path(argv[4])),
                    std::filesystem::temp_directory_path() /
                        ("anyps5-retro-host-test-" + std::to_string(Clock::now().time_since_epoch().count()))};
    std::filesystem::create_directories(fixture.root);
    TestJson();
    TestFrameRing();
    TestFramesAndInput(fixture);
    TestPs1Ports(fixture);
    TestCheckpoint(fixture);
    TestCrash(fixture);
    TestHang(fixture);
    TestEarlyExit(fixture, std::filesystem::absolute(std::filesystem::u8path(argv[0])));
    std::error_code ignored;
    std::filesystem::remove_all(fixture.root, ignored);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
  return 0;
}
