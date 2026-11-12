// Drives a real anyps5-retro-host process through the launcher's proxy with
// the line commands of the original in-process PS1 probes, so the mixed
// Windows/Linux rollback harness runs unchanged against host processes.
#include "retro_host_proxy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace anyps5::retro_ipc;
using Clock = std::chrono::steady_clock;

namespace {
class Probe {
 public:
  Probe(std::filesystem::path host, std::string core) : core_(std::move(core)) {
    ProxyOptions options;
    options.host = std::move(host);
    options.arguments = {"--probe"};
#if defined(_WIN32)
    options.texture_per_session = true;
#else
    options.texture_per_session = false;
#endif
    options.request_timeout = std::chrono::seconds(120);
    ProxyPlatform platform{
        [this] { return ++texture_; },
        [](int64_t) {},
        [this](int64_t texture) { Frame(texture); },
        [this](std::function<void()> task) {
          std::lock_guard<std::mutex> lock(mutex_);
          tasks_.push_back(std::move(task));
          wake_.notify_all();
        },
    };
    proxy_ = std::make_unique<RetroHostProxy>(std::move(options), std::move(platform));
  }

  ProxyResult Call(const std::string& method, Value arguments = Value(), bool owned = true) {
    if (owned && owner_) arguments = Value(Map{{Value("owner"), Value(*owner_)}, {Value("value"), std::move(arguments)}});
    std::optional<ProxyResult> reply;
    proxy_->Handle(method, std::move(arguments), [&](ProxyResult result) { reply = std::move(result); });
    while (!reply) {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return !tasks_.empty(); });
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
    }
    return std::move(*reply);
  }

  Value Success(const std::string& method, Value arguments = Value(), bool owned = true) {
    auto result = Call(method, std::move(arguments), owned);
    if (result.kind != ProxyResult::Kind::kSuccess) throw std::runtime_error(method + ": " + result.message);
    return std::move(result.value);
  }

  void Start(Map arguments) {
    const auto started = Success("start", Value(std::move(arguments)), false);
    if (const auto* handle = std::get_if<Map>(&started)) owner_ = handle->at(Value("session")).LongValue();
  }

  Map Status() {
    auto value = Success("status");
    if (!std::holds_alternative<Map>(value)) throw std::runtime_error("The PS1 host lost its game.");
    return std::get<Map>(value);
  }

  Map Stats() { return std::get<Map>(Success("probeStats")); }

  std::pair<uint64_t, std::pair<int64_t, uint64_t>> Presentation() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return {published_, {max_gap_us_, long_gaps_}};
  }

  const std::string& core() const { return core_; }
  void Shutdown() { proxy_->Shutdown(); }

 private:
  void Frame(int64_t texture) {
    if (!proxy_->Acquire(texture)) return;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    const auto now = Clock::now();
    if (last_frame_ != Clock::time_point{}) {
      const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame_).count();
      max_gap_us_ = std::max(max_gap_us_, static_cast<int64_t>(gap));
      if (gap > 33334) ++long_gaps_;
    }
    last_frame_ = now;
    ++published_;
  }

  std::string core_;
  std::unique_ptr<RetroHostProxy> proxy_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  std::atomic<int64_t> texture_{0};
  std::optional<int64_t> owner_;
  std::mutex frame_mutex_;
  Clock::time_point last_frame_{};
  uint64_t published_ = 0, long_gaps_ = 0;
  int64_t max_gap_us_ = 0;
};

int64_t Integer(const Map& map, const char* key, int64_t fallback = -1) {
  const auto found = map.find(Value(key));
  if (found == map.end() || found->second.IsNull()) return fallback;
  return found->second.LongValue();
}

std::filesystem::path HostPath(const char* probe) {
  if (const char* configured = std::getenv("ANYPS5_RETRO_HOST"); configured && *configured) return configured;
#if defined(_WIN32)
  const auto name = "anyps5-retro-host.exe";
#else
  const auto name = "anyps5-retro-host";
#endif
  const auto directory = std::filesystem::absolute(std::filesystem::u8path(probe)).parent_path();
  for (const auto& candidate : {directory / name, directory.parent_path() / name, directory.parent_path().parent_path() / name})
    if (std::filesystem::exists(candidate)) return candidate;
  throw std::runtime_error("anyps5-retro-host was not found beside the probe.");
}
}  // namespace

int main(int argc, char** argv) try {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  if (argc != 7 || std::string(argv[5]) != "rollback")
    throw std::runtime_error("Expected core, disc, BIOS, seat, rollback and an input trace.");
  const auto seat = std::stoi(argv[4]);
  if (seat < 0 || seat > 1) throw std::runtime_error("Invalid seat.");
  List trace;
  std::ifstream input(std::filesystem::u8path(argv[6]));
  for (unsigned buttons; input >> buttons;) {
    if (buttons > UINT16_MAX) throw std::runtime_error("Invalid trace input.");
    trace.emplace_back(static_cast<int32_t>(buttons));
  }
  if (!input.eof() || trace.size() < 2) throw std::runtime_error("Missing or malformed trace.");
  Probe probe(HostPath(argv[0]), argv[1]);
  probe.Start(Map{{Value("core"), Value(std::string(argv[1]))},
                  {Value("disc"), Value(std::string(argv[2]))},
                  {Value("system"), Value("")},
                  {Value("saves"), Value("")},
                  {Value("ps1NetplaySeat"), Value(seat)},
                  {Value("ps1NetplayBios"), Value(std::string(argv[3]))},
                  {Value("ps1NetplayRollback"), Value(true)},
                  {Value("probeInputTrace"), Value(std::move(trace))}});
  const auto deadline = Clock::now() + std::chrono::seconds(60);
  while (true) {
    const auto status = probe.Status();
    if (std::get<bool>(status.at(Value("ended")))) throw std::runtime_error(std::get<std::string>(status.at(Value("error"))));
    const auto ready = status.find(Value("netplayReady"));
    if (ready != status.end() && std::get<bool>(ready->second)) break;
    if (Clock::now() > deadline) throw std::runtime_error("The PS1 host did not become ready.");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::cout << "ready" << std::endl;
  for (std::string command; std::cin >> command && command != "quit";) {
    if (command == "start") {
      std::cout << (std::get<bool>(probe.Success("netplayStart")) ? "started" : "error") << std::endl;
    } else if (command == "sync" || command == "confirmed") {
      int64_t ack = -1;
      unsigned count = 0;
      List remote;
      if (command == "sync") {
        if (!(std::cin >> ack >> count) || count > 32) throw std::runtime_error("Invalid input packet.");
        for (unsigned index = 0; index < count; ++index) {
          int64_t frame;
          unsigned buttons;
          if (!(std::cin >> frame >> buttons)) throw std::runtime_error("Invalid remote input.");
          remote.emplace_back(List{Value(frame), Value(static_cast<int32_t>(buttons)), Value(0), Value(0), Value(0), Value(0)});
        }
      }
      const auto update = std::get<Map>(probe.Success("netplaySync", Value(Map{
          {Value("remote"), Value(std::move(remote))},
          {Value("acknowledgedFrame"), ack < 0 ? Value() : Value(ack)}})));
      if (command == "confirmed") {
        std::cout << Integer(update, "confirmedFrame") << std::endl;
        continue;
      }
      const auto& local = std::get<List>(update.at(Value("local")));
      std::cout << Integer(update, "frame") << ' ' << Integer(update, "receivedFrame") << ' ' << local.size();
      for (const auto& row : local) {
        const auto& values = std::get<List>(row);
        std::cout << ' ' << values[0].LongValue() << ' ' << values[1].LongValue();
      }
      const auto& fingerprints = std::get<List>(update.at(Value("fingerprints")));
      std::cout << ' ' << fingerprints.size();
      for (const auto& row : fingerprints) {
        const auto& values = std::get<List>(row);
        std::cout << ' ' << values[0].LongValue() << ' ' << std::get<std::string>(values[1]);
      }
      std::cout << std::endl;
    } else if (command == "read") {
      int64_t offset, count;
      if (!(std::cin >> offset >> count)) throw std::runtime_error("Invalid RAM read.");
      const auto bytes = std::get<Bytes>(probe.Success("probeReadRam", Value(Map{
          {Value("core"), Value(probe.core())}, {Value("offset"), Value(offset)}, {Value("count"), Value(count)}}), false));
      constexpr char digits[] = "0123456789abcdef";
      for (const auto byte : bytes) std::cout << digits[byte >> 4] << digits[byte & 15];
      std::cout << std::endl;
    } else if (command == "audio") {
      const auto stats = probe.Stats();
      std::cout << Integer(stats, "audioFrames") << ' ' << std::get<std::string>(stats.at(Value("audioHash"))) << std::endl;
    } else if (command == "audio-drained") {
      std::cout << Integer(probe.Stats(), "audioDrained") << std::endl;
    } else if (command == "corrections") {
      std::cout << Integer(probe.Stats(), "corrections", 0) << std::endl;
    } else if (command == "published") {
      std::cout << probe.Presentation().first << std::endl;
    } else if (command == "presentation") {
      const auto stats = probe.Stats();
      const auto [published, gaps] = probe.Presentation();
      std::cout << published << ' ' << gaps.first << ' ' << gaps.second << " 0 " << Integer(stats, "maxWorkUs", 0) << ' '
                << Integer(stats, "workOverruns", 0) << std::endl;
    } else if (command == "pacing") {
      const auto stats = probe.Stats();
      const auto found = stats.find(Value("pacing"));
      if (found == stats.end()) {
        std::cout << "0 0 0 0" << std::endl;
      } else {
        const auto& values = std::get<List>(found->second);
        for (size_t index = 0; index < values.size(); ++index) std::cout << (index ? " " : "") << values[index].LongValue();
        std::cout << std::endl;
      }
    } else if (command == "cpu") {
      const auto status = probe.Status();
      const auto& options = std::get<Map>(status.at(Value("options")));
      const auto& cpu = std::get<Map>(options.at(Value("swanstation_CPU_ExecutionMode")));
      std::cout << std::get<std::string>(cpu.at(Value("activeValue"))) << std::endl;
    } else {
      throw std::runtime_error("Unknown probe command " + command);
    }
  }
  probe.Success("stop");
  std::string error;
  for (const auto until = Clock::now() + std::chrono::seconds(20); Clock::now() < until;) {
    const auto status = probe.Status();
    if (std::get<bool>(status.at(Value("ended")))) {
      error = std::get<std::string>(status.at(Value("error")));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  probe.Success("release");
  probe.Shutdown();
  if (!error.empty()) std::cerr << error << '\n';
  return error.empty() ? 0 : 7;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
