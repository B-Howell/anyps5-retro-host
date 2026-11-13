// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_PLATFORM_H_
#define ANYPS5_RETRO_HOST_PLATFORM_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace retro_host {

#ifdef _WIN32
using NativeHandle = void*;
inline NativeHandle const kNoHandle = nullptr;
#else
using NativeHandle = int;
constexpr NativeHandle kNoHandle = -1;
#endif

struct Arguments {
  int ipc_fd = -1;
  uint64_t ipc_read = 0;
  uint64_t ipc_write = 0;
  uint32_t parent_pid = 0;
};

// The command line of protocol.md, "Starting the host"; nullopt when it is not that.
std::optional<Arguments> ParseArguments(int argc, char** argv);

// Steady milliseconds.
int64_t NowMs();

uint32_t ProcessId();

// The process setup protocol.md asks for before anything else: on Linux, descriptors above 3
// closed, every signal unblocked, PR_SET_PDEATHSIG and the parent check; on macOS SO_NOSIGPIPE
// is set by the channel; on Windows the error mode. False means the launcher is already gone.
bool PrepareProcess(const Arguments& arguments);

// Calls on_gone once, from another thread, when the launcher exits or (on Linux and macOS) the
// host gets SIGTERM.
void WatchLauncher(const Arguments& arguments, std::function<void()> on_gone);

// Ends the process at once, without running destructors (a missed limit, protocol.md "Timeouts").
[[noreturn]] void ExitNow(int code);

// Shared memory for one frame ring, created as protocol.md "Memory objects" says, mapped read
// and write. The host keeps every ring until it exits.
struct SharedMemory {
  NativeHandle handle = kNoHandle;
  uint8_t* data = nullptr;
  uint64_t bytes = 0;
};
std::optional<SharedMemory> CreateSharedMemory(uint64_t bytes);

// A dynamically loaded library (the core).
class Library {
 public:
  Library() = default;
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  ~Library();
  // Loads the library at the UTF-8 path; on failure returns false and sets error.
  bool Open(const std::string& path, std::string& error);
  void* Symbol(const char* name) const;
  void Close();
  bool IsOpen() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

// Opens a file named by a UTF-8 path for reading, or returns false.
bool CanRead(const std::string& path);
bool Exists(const std::string& path);
bool IsDirectory(const std::string& path);
bool CreateDirectories(const std::string& path);
std::optional<std::string> ReadWholeFile(const std::string& path);
// Replaces the file at path with data: written beside it, then renamed over it.
bool WriteFileAtomically(const std::string& path, const std::string& data);

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_PLATFORM_H_
