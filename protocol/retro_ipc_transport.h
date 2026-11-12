#ifndef ANYPS5_RETRO_IPC_TRANSPORT_H_
#define ANYPS5_RETRO_IPC_TRANSPORT_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace anyps5::retro_ipc {

constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kMaxMessageBytes = 64u * 1024u * 1024u;

#if defined(_WIN32)
using NativeHandle = void*;
#else
using NativeHandle = int;
#endif

struct Message {
  std::string json;
  std::vector<int> descriptors;
};

// A framed, ordered, bidirectional message stream: a pair of anonymous
// pipes on Windows and a Unix stream socket elsewhere. Each message is a
// four-byte little-endian length followed by that many bytes of JSON. On
// POSIX a message may carry file descriptors as SCM_RIGHTS ancillary data.
// Send may be called from any thread; Receive from one reader thread.
class Channel {
 public:
  Channel(NativeHandle read, NativeHandle write);
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  bool Send(const std::string& json, int descriptor = -1);
  std::optional<Message> Receive();
  void CloseWrite();

 private:
  bool ReadExact(void* data, size_t size, std::vector<int>& descriptors);
  bool WriteAll(const void* data, size_t size, int descriptor);

  NativeHandle read_, write_;
  std::mutex write_mutex_;
};

// The launcher's handle on a running host process.
class ChildProcess {
 public:
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  static std::unique_ptr<ChildProcess> Spawn(const std::filesystem::path& executable,
                                             const std::vector<std::string>& arguments,
                                             std::unique_ptr<Channel>& channel);

  int64_t pid() const { return pid_; }
  bool Wait(std::chrono::milliseconds timeout);
  void Kill();
  std::optional<int> exit_code() const;
  // The host has taken its ends of the channel (it said hello).
  void Connected();
  // The host exited: release any channel end it never took, so the reader
  // sees the end of the stream.
  void Abandon();
#if defined(_WIN32)
  NativeHandle Duplicate(uint64_t remote_handle) const;
#endif

 private:
  ChildProcess() = default;
  bool Poll();
  int64_t pid_ = 0;
  mutable std::mutex mutex_;
  std::optional<int> exit_code_;
#if defined(_WIN32)
  NativeHandle process_ = nullptr;
  // The host's channel ends as handle values in this process, and private
  // duplicates that tell whether a value still names the same pipe.
  NativeHandle child_ends_[2] = {nullptr, nullptr};
  NativeHandle kept_ends_[2] = {nullptr, nullptr};
#endif
};

// The host's end of the channel, from the command line the launcher wrote.
std::unique_ptr<Channel> ConnectFromArguments(const std::vector<std::string>& arguments);
int64_t CurrentProcessId();

}  // namespace anyps5::retro_ipc

#endif  // ANYPS5_RETRO_IPC_TRANSPORT_H_
