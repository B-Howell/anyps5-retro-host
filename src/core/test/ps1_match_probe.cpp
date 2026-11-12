#include "retro_test_support.h"

#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <mutex>

namespace retro = anyps5::retro;
namespace timeline = ps1_netplay;

namespace {
class ProbeAudio final : public retro::Audio {
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
  void Print() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << frames_ << ' ' << hash_ << std::endl;
  }
  void Flush() override {}
  int64_t dropped_frames() const override { return 0; }
  void Close() override {}

 private:
  std::mutex mutex_;
  uint64_t frames_ = 0, hash_ = 14695981039346656037ull;
};

constexpr uint32_t kRamSize = 2 * 1024 * 1024;
void PrintRam(const char* core, size_t offset, size_t count) {
  void* library = dlopen(core, RTLD_NOW | RTLD_NOLOAD);
  if (!library) throw std::runtime_error("The Linux probe core is unavailable.");
  const auto get = reinterpret_cast<void* (*)(unsigned)>(dlsym(library, "retro_get_memory_data"));
  const auto* ram = get ? static_cast<const uint8_t*>(get(2)) : nullptr;
  std::string result;
  if (ram) {
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = offset; i < offset + count; ++i) {
      result += digits[ram[i] >> 4];
      result += digits[ram[i] & 15];
    }
  }
  dlclose(library);
  if (!ram) throw std::runtime_error("PS1 main RAM is unavailable.");
  std::cout << result << std::endl;
}

int64_t Frame(std::optional<uint32_t> frame) { return frame ? static_cast<int64_t>(*frame) : -1; }
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 7) throw std::runtime_error("Expected core, disc, BIOS, seat, mode and input trace.");
  const auto seat = std::stoul(argv[4]);
  const std::string mode = argv[5];
  if (seat > 1 || (mode != "rollback" && mode != "confirmed"))
    throw std::runtime_error("Invalid seat or mode.");
  retro::Ps1MatchConfig match{static_cast<unsigned>(seat), argv[3]};
  match.rollback = mode == "rollback";
  std::ifstream trace(argv[6]);
  for (unsigned buttons; trace >> buttons;) {
    if (buttons > UINT16_MAX || match.input_trace.size() >= timeline::Timeline::kMaxFrame)
      throw std::runtime_error("Invalid trace input.");
    match.input_trace.push_back({static_cast<uint16_t>(buttons), {}});
  }
  if (!trace.eof() || match.input_trace.size() < timeline::Timeline::kDelay)
    throw std::runtime_error("Missing or malformed trace.");
  const auto target = match.input_trace.size();
  auto audio = std::make_unique<ProbeAudio>();
  auto* sink = audio.get();
  retro::SessionConfig config{argv[1], argv[2], {}, {}, {}, {}};
  config.ps1_match = std::move(match);
  retro::Session session(std::move(config), std::make_unique<retro::test::TestVideo>(),
                         std::move(audio), [] {});
  session.Start();
  retro::test::WaitFor([&] { return session.Status().netplay_ready || session.ended(); },
                      "Linux PS1 probe did not initialize.");
  const auto boot = session.Status();
  if (!boot.netplay_ready) throw std::runtime_error(boot.error);
  std::cout << "ready" << std::endl;

  for (std::string command; std::cin >> command;) {
    const auto status = session.Status();
    if (status.ended) throw std::runtime_error(status.error);
    if (command == "quit") break;
    if (command == "start") {
      if (!session.StartNetplay()) throw std::runtime_error("Could not start the Linux match.");
      std::cout << "started" << std::endl;
    } else if (command == "audio") {
      sink->Print();
    } else if (command == "audio-drained") {
      std::cout << status.confirmed_audio_frame << std::endl;
    } else if (command == "corrections") {
      std::cout << status.rollback_corrections << std::endl;
    } else if (command == "sync" || command == "confirmed" || command == "read") {
      int64_t ack = -1;
      unsigned count = 0;
      if (command == "sync" && (!(std::cin >> ack >> count) || ack < -1 ||
          ack > timeline::Timeline::kMaxFrame || count > 32))
        throw std::runtime_error("Invalid input packet.");
      std::vector<timeline::FrameInput> remote;
      for (unsigned i = 0; i < count; ++i) {
        uint32_t frame;
        unsigned buttons;
        if (!(std::cin >> frame >> buttons) || buttons > UINT16_MAX)
          throw std::runtime_error("Invalid remote input.");
        remote.push_back({frame, {static_cast<uint16_t>(buttons), {}}});
      }
      const auto update = session.SyncNetplay(remote, ack < 0 ? std::nullopt
                                                           : std::optional<uint32_t>(ack));
      if (!update) throw std::runtime_error("Linux PS1 worker rejected the input packet.");
      if (command == "confirmed") {
        std::cout << Frame(update->confirmed) << std::endl;
      } else if (command == "read") {
        size_t offset, size;
        if (update->frame != target || Frame(update->confirmed) + 1 != static_cast<int64_t>(target) ||
            !(std::cin >> offset >> size) || size > 256 || offset > kRamSize || size > kRamSize - offset)
          throw std::runtime_error("RAM reads require a finished, confirmed match and valid bounds.");
        PrintRam(argv[1], offset, size);
      } else {
        std::cout << update->frame << ' ' << Frame(update->received) << ' ' << update->local.size();
        for (const auto& input : update->local) std::cout << ' ' << input.frame << ' ' << input.input.buttons;
        std::cout << ' ' << update->fingerprints.size();
        for (const auto& [frame, hash] : update->fingerprints) std::cout << ' ' << frame << ' ' << hash;
        std::cout << std::endl;
      }
    } else {
      throw std::runtime_error("Unknown Linux probe command.");
    }
  }
  session.Stop();
  session.Join();
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
