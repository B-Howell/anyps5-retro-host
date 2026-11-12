#include "retro_ipc_transport.h"

#include <cstring>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace anyps5::retro_ipc {
namespace {
std::optional<uint64_t> ParseNumber(const std::string& text) {
  if (text.empty() || text.size() > 20) return std::nullopt;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

std::optional<uint64_t> Argument(const std::vector<std::string>& arguments, const char* name) {
  for (size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] == name) return ParseNumber(arguments[index + 1]);
  }
  return std::nullopt;
}
}  // namespace

#if defined(_WIN32)

Channel::Channel(NativeHandle read, NativeHandle write) : read_(read), write_(write) {}

Channel::~Channel() {
  if (read_) CloseHandle(read_);
  if (write_) CloseHandle(write_);
}

void Channel::CloseWrite() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (write_) CloseHandle(write_);
  write_ = nullptr;
}

bool Channel::ReadExact(void* data, size_t size, std::vector<int>&) {
  auto* cursor = static_cast<uint8_t*>(data);
  while (size) {
    DWORD read = 0;
    const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
    if (!ReadFile(read_, cursor, chunk, &read, nullptr) || read == 0) return false;
    cursor += read;
    size -= read;
  }
  return true;
}

bool Channel::WriteAll(const void* data, size_t size, int) {
  const auto* cursor = static_cast<const uint8_t*>(data);
  while (size) {
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
    if (!write_ || !WriteFile(write_, cursor, chunk, &written, nullptr) || written == 0) return false;
    cursor += written;
    size -= written;
  }
  return true;
}

#else

Channel::Channel(NativeHandle read, NativeHandle write) : read_(read), write_(write) {}

Channel::~Channel() {
  if (read_ >= 0) close(read_);
  if (write_ >= 0 && write_ != read_) close(write_);
}

void Channel::CloseWrite() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (write_ >= 0) shutdown(write_, SHUT_WR);
}

bool Channel::ReadExact(void* data, size_t size, std::vector<int>& descriptors) {
  auto* cursor = static_cast<uint8_t*>(data);
  while (size) {
    iovec vector{cursor, size};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)];
    msghdr header{};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
#if defined(MSG_CMSG_CLOEXEC)
    const auto received = recvmsg(read_, &header, MSG_CMSG_CLOEXEC);
#else
    const auto received = recvmsg(read_, &header, 0);
#endif
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return false;
    for (auto* item = CMSG_FIRSTHDR(&header); item; item = CMSG_NXTHDR(&header, item)) {
      if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS) continue;
      const auto count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (size_t index = 0; index < count; ++index) {
        int descriptor;
        std::memcpy(&descriptor, CMSG_DATA(item) + index * sizeof(int), sizeof(int));
        descriptors.push_back(descriptor);
      }
    }
    cursor += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

bool Channel::WriteAll(const void* data, size_t size, int descriptor) {
  const auto* cursor = static_cast<const uint8_t*>(data);
  while (size) {
    iovec vector{const_cast<uint8_t*>(cursor), size};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
    msghdr header{};
    header.msg_iov = &vector;
    header.msg_iovlen = 1;
    if (descriptor >= 0) {
      header.msg_control = control;
      header.msg_controllen = sizeof(control);
      auto* item = CMSG_FIRSTHDR(&header);
      item->cmsg_level = SOL_SOCKET;
      item->cmsg_type = SCM_RIGHTS;
      item->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(item), &descriptor, sizeof(int));
    }
#if defined(MSG_NOSIGNAL)
    const auto sent = sendmsg(write_, &header, MSG_NOSIGNAL);
#else
    const auto sent = sendmsg(write_, &header, 0);
#endif
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    descriptor = -1;
    cursor += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

#endif

bool Channel::Send(const std::string& json, int descriptor) {
  if (json.size() > kMaxMessageBytes) return false;
  const auto size = static_cast<uint32_t>(json.size());
  std::string frame(4 + json.size(), '\0');
  for (int index = 0; index < 4; ++index) frame[index] = static_cast<char>((size >> (8 * index)) & 0xFF);
  std::memcpy(frame.data() + 4, json.data(), json.size());
  std::lock_guard<std::mutex> lock(write_mutex_);
  return WriteAll(frame.data(), frame.size(), descriptor);
}

std::optional<Message> Channel::Receive() {
  Message message;
  uint8_t prefix[4];
  if (!ReadExact(prefix, sizeof(prefix), message.descriptors)) return std::nullopt;
  const uint32_t size = prefix[0] | (prefix[1] << 8) | (prefix[2] << 16) | (static_cast<uint32_t>(prefix[3]) << 24);
  if (size > kMaxMessageBytes) return std::nullopt;
  message.json.resize(size);
  if (size && !ReadExact(message.json.data(), size, message.descriptors)) return std::nullopt;
  return message;
}

#if defined(_WIN32)

namespace {
std::wstring Quote(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const auto c : argument) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') quoted.append(backslashes * 2 + 1, L'\\');
    else quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted += c;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted += L'"';
  return quoted;
}

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (process_) CloseHandle(process_);
}

std::unique_ptr<ChildProcess> ChildProcess::Spawn(const std::filesystem::path& executable,
                                                  const std::vector<std::string>& arguments,
                                                  std::unique_ptr<Channel>& channel) {
  HANDLE to_host_read = nullptr, to_host_write = nullptr, from_host_read = nullptr, from_host_write = nullptr;
  if (!CreatePipe(&to_host_read, &to_host_write, nullptr, 1 << 20)) return nullptr;
  if (!CreatePipe(&from_host_read, &from_host_write, nullptr, 1 << 20)) {
    CloseHandle(to_host_read);
    CloseHandle(to_host_write);
    return nullptr;
  }
  std::wstring command = Quote(executable.wstring());
  for (const auto& argument : arguments) command += L" " + Quote(Wide(argument));
  command += L" --ipc-read " + std::to_wstring(reinterpret_cast<uintptr_t>(to_host_read));
  command += L" --ipc-write " + std::to_wstring(reinterpret_cast<uintptr_t>(from_host_write));
  command += L" --parent-pid " + std::to_wstring(GetCurrentProcessId());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &information)) {
    for (const auto handle : {to_host_read, to_host_write, from_host_read, from_host_write}) CloseHandle(handle);
    return nullptr;
  }
  CloseHandle(information.hThread);
  auto child = std::unique_ptr<ChildProcess>(new ChildProcess());
  child->process_ = information.hProcess;
  child->pid_ = information.dwProcessId;
  channel = std::make_unique<Channel>(from_host_read, to_host_write);
  return child;
}

bool ChildProcess::Wait(std::chrono::milliseconds timeout) {
  if (exit_code_) return true;
  if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(process_, &code);
  exit_code_ = static_cast<int>(code);
  return true;
}

void ChildProcess::Kill() {
  if (exit_code_) return;
  TerminateProcess(process_, 0xDEAD);
  Wait(std::chrono::milliseconds(5000));
}

NativeHandle ChildProcess::Duplicate(uint64_t remote_handle) const {
  HANDLE local = nullptr;
  if (!DuplicateHandle(process_, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remote_handle)),
                       GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return nullptr;
  return local;
}

std::unique_ptr<Channel> ConnectFromArguments(const std::vector<std::string>& arguments) {
  const auto read = Argument(arguments, "--ipc-read");
  const auto write = Argument(arguments, "--ipc-write");
  const auto parent = Argument(arguments, "--parent-pid");
  if (!read || !write || !parent || *parent > UINT32_MAX) return nullptr;
  const HANDLE launcher = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(*parent));
  if (!launcher) return nullptr;
  const auto take = [launcher](uint64_t value) {
    HANDLE local = nullptr;
    if (!DuplicateHandle(launcher, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)), GetCurrentProcess(),
                         &local, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
      return HANDLE{nullptr};
    return local;
  };
  const auto input = take(*read);
  const auto output = take(*write);
  CloseHandle(launcher);
  if (!input || !output) {
    if (input) CloseHandle(input);
    if (output) CloseHandle(output);
    return nullptr;
  }
  return std::make_unique<Channel>(input, output);
}

int64_t CurrentProcessId() { return GetCurrentProcessId(); }

#else

ChildProcess::~ChildProcess() = default;

std::unique_ptr<ChildProcess> ChildProcess::Spawn(const std::filesystem::path& executable,
                                                  const std::vector<std::string>& arguments,
                                                  std::unique_ptr<Channel>& channel) {
  int sockets[2];
#if defined(SOCK_CLOEXEC)
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return nullptr;
#else
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return nullptr;
#endif
  constexpr int kChildDescriptor = 3;
  std::vector<std::string> storage{executable.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  storage.insert(storage.end(), {"--ipc-fd", std::to_string(kChildDescriptor), "--parent-pid", std::to_string(getpid())});
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, sockets[1], kChildDescriptor);
  pid_t pid = 0;
  const int failed = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (failed) {
    close(sockets[0]);
    return nullptr;
  }
  auto child = std::unique_ptr<ChildProcess>(new ChildProcess());
  child->pid_ = pid;
  channel = std::make_unique<Channel>(sockets[0], sockets[0]);
  return child;
}

bool ChildProcess::Wait(std::chrono::milliseconds timeout) {
  if (exit_code_) return true;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    int status = 0;
    const auto done = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (done == pid_) {
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
      return true;
    }
    if (done < 0 && errno != EINTR) {
      exit_code_ = -1;
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void ChildProcess::Kill() {
  if (exit_code_) return;
  kill(static_cast<pid_t>(pid_), SIGKILL);
  Wait(std::chrono::milliseconds(5000));
}

std::unique_ptr<Channel> ConnectFromArguments(const std::vector<std::string>& arguments) {
  const auto descriptor = Argument(arguments, "--ipc-fd");
  if (!descriptor || *descriptor > 1 << 20) return nullptr;
  const int fd = static_cast<int>(*descriptor);
  return std::make_unique<Channel>(fd, fd);
}

int64_t CurrentProcessId() { return getpid(); }

#endif

}  // namespace anyps5::retro_ipc
