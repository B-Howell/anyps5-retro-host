// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <sys/event.h>
#endif

#include "channel.h"
#include "platform.h"

namespace retro_host {
namespace {

std::optional<uint64_t> Decimal(const char* text, uint64_t high) {
  if (!text || !*text) return std::nullopt;
  uint64_t value = 0;
  for (const char* c = text; *c; ++c) {
    if (*c < '0' || *c > '9') return std::nullopt;
    value = value * 10 + static_cast<uint64_t>(*c - '0');
    if (value > high) return std::nullopt;
  }
  return value;
}

#ifdef __linux__
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

class PosixChannel final : public Channel {
 public:
  explicit PosixChannel(int fd) : fd_(fd) {
#ifdef __APPLE__
    int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
  }

  ReadResult Read(std::string& body) override {
    char header[4];
    const ReadResult got_length = ReadExactly(header, 4);
    if (got_length != ReadResult::kMessage) return got_length;
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<unsigned char>(header[i])) << (8 * i);
    if (size < kMinMessage || size > kMaxMessage) return ReadResult::kProtocolError;
    body.assign(size, '\0');
    const ReadResult got_body = ReadExactly(body.data(), size);
    return got_body == ReadResult::kEnd ? ReadResult::kEnd : got_body;
  }

 protected:
  bool WriteFrame(const std::string& frame, NativeHandle memory) override {
    size_t sent = 0;
    if (memory != kNoHandle) {
      iovec data{const_cast<char*>(frame.data()), frame.size()};
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
      msghdr message{};
      message.msg_iov = &data;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof control;
      cmsghdr* rights = CMSG_FIRSTHDR(&message);
      rights->cmsg_level = SOL_SOCKET;
      rights->cmsg_type = SCM_RIGHTS;
      rights->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(rights), &memory, sizeof(int));
      ssize_t result;
      do {
        result = sendmsg(fd_, &message, kSendFlags);
      } while (result < 0 && errno == EINTR);
      if (result <= 0) return false;
      sent = static_cast<size_t>(result);
    }
    while (sent < frame.size()) {
      const ssize_t result = send(fd_, frame.data() + sent, frame.size() - sent, kSendFlags);
      if (result < 0 && errno == EINTR) continue;
      if (result <= 0) return false;
      sent += static_cast<size_t>(result);
    }
    return true;
  }

 private:
  // Reads exactly size bytes, never asking for more than are left of the current message. Any
  // descriptor or other control data is closed and is a protocol error.
  ReadResult ReadExactly(char* out, size_t size) {
    size_t got = 0;
    while (got < size) {
      iovec data{out + got, size - got};
      alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int))];
      msghdr message{};
      message.msg_iov = &data;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof control;
#ifdef MSG_CMSG_CLOEXEC
      const int flags = MSG_CMSG_CLOEXEC;
#else
      const int flags = 0;
#endif
      const ssize_t result = recvmsg(fd_, &message, flags);
      if (result < 0 && errno == EINTR) continue;
      bool stray = (message.msg_flags & MSG_CTRUNC) != 0;
      if (result >= 0 && message.msg_controllen > 0) {
        for (cmsghdr* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
          stray = true;
          if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS) {
            const size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
              int descriptor;
              std::memcpy(&descriptor, CMSG_DATA(item) + i * sizeof(int), sizeof(int));
              close(descriptor);
            }
          }
        }
      }
      if (stray) return ReadResult::kProtocolError;
      if (result <= 0) return ReadResult::kEnd;
      got += static_cast<size_t>(result);
    }
    return ReadResult::kMessage;
  }

  int fd_;
};

}  // namespace

std::optional<Arguments> ParseArguments(int argc, char** argv) {
  Arguments arguments;
  bool have_fd = false;
  bool have_parent = false;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) return std::nullopt;
    const std::string name = argv[i];
    if (name == "--ipc-fd" && !have_fd) {
      const auto fd = Decimal(argv[i + 1], 1 << 20);
      if (!fd) return std::nullopt;
      arguments.ipc_fd = static_cast<int>(*fd);
      have_fd = true;
    } else if (name == "--parent-pid" && !have_parent) {
      const auto pid = Decimal(argv[i + 1], UINT32_MAX);
      if (!pid || *pid == 0) return std::nullopt;
      arguments.parent_pid = static_cast<uint32_t>(*pid);
      have_parent = true;
    } else {
      return std::nullopt;
    }
  }
  if (!have_fd || !have_parent || arguments.ipc_fd != 3) return std::nullopt;
  return arguments;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t ProcessId() { return static_cast<uint32_t>(getpid()); }

bool PrepareProcess(const Arguments& arguments) {
#ifdef __linux__
  if (syscall(SYS_close_range, 4u, ~0u, 0u) != 0) {
    std::vector<int> open;
    if (DIR* listing = opendir("/proc/self/fd")) {
      while (dirent* entry = readdir(listing)) {
        const auto fd = Decimal(entry->d_name, 1 << 30);
        if (fd && *fd > 3 && static_cast<int>(*fd) != dirfd(listing)) open.push_back(static_cast<int>(*fd));
      }
      closedir(listing);
    }
    for (const int fd : open) close(fd);
  }
#endif
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  signal(SIGPIPE, SIG_IGN);
#ifdef __linux__
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (static_cast<uint32_t>(getppid()) != arguments.parent_pid) return false;
#else
  if (kill(static_cast<pid_t>(arguments.parent_pid), 0) != 0 && errno == ESRCH) return false;
#endif
  return true;
}

void WatchLauncher(const Arguments& arguments, std::function<void()> on_gone) {
  sigset_t term;
  sigemptyset(&term);
  sigaddset(&term, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &term, nullptr);
  auto shared = std::make_shared<std::function<void()>>(std::move(on_gone));
  auto once = std::make_shared<std::once_flag>();
  auto fire = [shared, once] { std::call_once(*once, [&] { (*shared)(); }); };
  std::thread([term, fire] {
    int received = 0;
    while (sigwait(&term, &received) != 0) {
    }
    fire();
  }).detach();
#ifdef __APPLE__
  const pid_t parent = static_cast<pid_t>(arguments.parent_pid);
  std::thread([parent, fire] {
    const int queue = kqueue();
    struct kevent change;
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    struct kevent event;
    const int result = kevent(queue, &change, 1, &event, 1, nullptr);
    if (result < 0 || (event.flags & EV_ERROR)) {
      if (errno == ESRCH || (result > 0 && event.data == ESRCH)) fire();
      return;
    }
    fire();
  }).detach();
#else
  (void)arguments;
#endif
}

void ExitNow(int code) { _exit(code); }

std::optional<SharedMemory> CreateSharedMemory(uint64_t bytes) {
#ifdef __linux__
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "anyps5-retro-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (fd < 0) return std::nullopt;
#else
  static const char kLetters[] = "abcdefghijklmnopqrstuvwxyz";
  std::random_device random;
  int fd = -1;
  for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
    std::string name = "/a5r-";
    for (int i = 0; i < 12; ++i) name += kLetters[random() % 26];
    fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name.c_str());
  }
  if (fd < 0) return std::nullopt;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    close(fd);
    return std::nullopt;
  }
#ifdef __linux__
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
    close(fd);
    return std::nullopt;
  }
#endif
  void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return std::nullopt;
  }
  return SharedMemory{fd, static_cast<uint8_t*>(data), bytes};
}

Library::~Library() { Close(); }

bool Library::Open(const std::string& path, std::string& error) {
  Close();
  handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    const char* reason = dlerror();
    error = reason ? reason : "the library could not be loaded";
    return false;
  }
  return true;
}

void* Library::Symbol(const char* name) const { return handle_ ? dlsym(handle_, name) : nullptr; }

void Library::Close() {
  if (handle_) dlclose(handle_);
  handle_ = nullptr;
}

bool CanRead(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info;
  const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  close(fd);
  return regular;
}

bool IsDirectory(const std::string& path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool CreateDirectories(const std::string& path) {
  if (IsDirectory(path)) return true;
  for (size_t at = 1; at <= path.size(); ++at) {
    if (at == path.size() || path[at] == '/') {
      const std::string part = path.substr(0, at);
      if (!IsDirectory(part) && mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return IsDirectory(path);
}

std::optional<std::string> ReadWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream data;
  data << in.rdbuf();
  return data.str();
}

bool WriteFileAtomically(const std::string& path, const std::string& data) {
  const std::string temporary = path + ".writing";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out.flush()) return false;
  }
  return std::rename(temporary.c_str(), path.c_str()) == 0;
}

std::unique_ptr<Channel> OpenChannel(const Arguments& arguments) {
  return std::make_unique<PosixChannel>(arguments.ipc_fd);
}

}  // namespace retro_host
