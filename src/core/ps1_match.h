#ifndef ANYPS5_RETRO_PS1_MATCH_H_
#define ANYPS5_RETRO_PS1_MATCH_H_

#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace anyps5::retro {

constexpr std::pair<std::string_view, std::string_view> kPs1NetplayOptions[] = {
    {"swanstation_GPU_Renderer", "Software"},
    {"swanstation_GPU_ResolutionScale", "1"},
    {"swanstation_CPU_ExecutionMode", "CachedInterpreter"},
    {"swanstation_CPU_RecompilerBlockLinking", "false"},
    {"swanstation_GPU_UseThread", "false"},
    {"swanstation_BIOS_PatchFastBoot", "false"},
};

// SwanStation's RETRO_DEVICE_PS_DUALSHOCK (first analog subclass).
constexpr unsigned kPs1DualShockDevice = (1u << 8) | 5u;

class Ps1MatchStorage {
 public:
  explicit Ps1MatchStorage(const std::string& bios) {
    const auto input = std::filesystem::u8path(bios);
    if (!input.is_absolute() || !std::filesystem::is_regular_file(input) ||
        std::filesystem::file_size(input) != 512 * 1024)
      throw std::runtime_error("PS1 netplay requires a verified SCPH1001 BIOS.");
    const auto temp = std::filesystem::canonical(std::filesystem::temp_directory_path());
    std::random_device random;
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto candidate = temp / ("anyps5-ps1-netplay-" + std::to_string(random()) + "-" +
                                     std::to_string(random()));
      if (!std::filesystem::create_directory(candidate)) continue;
      root_ = candidate;
      break;
    }
    if (root_.empty()) throw std::runtime_error("Could not create PS1 netplay scratch storage.");
    try {
#ifndef _WIN32
      std::filesystem::permissions(root_, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace);
      constexpr auto shared = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
      if ((std::filesystem::status(root_).permissions() & shared) != std::filesystem::perms::none)
        throw std::runtime_error("PS1 netplay needs a private temporary directory.");
#endif
      std::filesystem::create_directory(root_ / "system");
      std::filesystem::create_directory(root_ / "save");
      std::filesystem::copy_file(input, root_ / "system" / "scph1001.bin");
    } catch (...) {
      Cleanup();
      throw;
    }
  }
  ~Ps1MatchStorage() { Cleanup(); }
  Ps1MatchStorage(const Ps1MatchStorage&) = delete;
  Ps1MatchStorage& operator=(const Ps1MatchStorage&) = delete;

  std::string system() const { return (root_ / "system").u8string(); }
  std::string saves() const { return (root_ / "save").u8string(); }
  std::string content() const { return root_.u8string(); }

 private:
  void Cleanup() noexcept {
    if (root_.empty()) return;
    std::error_code error;
    const auto resolved = std::filesystem::canonical(root_, error);
    if (!error && resolved == root_) std::filesystem::remove_all(root_, error);
  }

  std::filesystem::path root_;
};

inline std::string NetplayFingerprint(const uint8_t* memory, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t index = 0; index < size; ++index)
    hash = (hash ^ memory[index]) * 1099511628211ULL;
  std::string result(16, '0');
  constexpr char digits[] = "0123456789abcdef";
  for (size_t index = result.size(); index > 0; --index) {
    result[index - 1] = digits[hash & 15];
    hash >>= 4;
  }
  return result;
}

}  // namespace anyps5::retro

#endif
