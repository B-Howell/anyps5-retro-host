// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "channel.h"
#include "platform.h"

namespace retro_host {
namespace {

HANDLE g_launcher = nullptr;

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

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

class PipeChannel final : public Channel {
 public:
  PipeChannel(HANDLE read, HANDLE write) : read_(read), write_(write) {}

  ReadResult Read(std::string& body) override {
    char header[4];
    const ReadResult got_length = ReadExactly(header, 4);
    if (got_length != ReadResult::kMessage) return got_length;
    uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(static_cast<unsigned char>(header[i])) << (8 * i);
    if (size < kMinMessage || size > kMaxMessage) return ReadResult::kProtocolError;
    body.assign(size, '\0');
    return ReadExactly(body.data(), size);
  }

 protected:
  bool WriteFrame(const std::string& frame, NativeHandle) override {
    size_t sent = 0;
    while (sent < frame.size()) {
      DWORD wrote = 0;
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(frame.size() - sent, 1u << 20));
      if (!WriteFile(write_, frame.data() + sent, chunk, &wrote, nullptr) || wrote == 0) return false;
      sent += wrote;
    }
    return true;
  }

 private:
  ReadResult ReadExactly(char* out, size_t size) {
    size_t got = 0;
    while (got < size) {
      DWORD read = 0;
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - got, 1u << 20));
      if (!ReadFile(read_, out + got, chunk, &read, nullptr) || read == 0) return ReadResult::kEnd;
      got += read;
    }
    return ReadResult::kMessage;
  }

  HANDLE read_;
  HANDLE write_;
};

}  // namespace

std::optional<Arguments> ParseArguments(int argc, char** argv) {
  Arguments arguments;
  bool have_read = false;
  bool have_write = false;
  bool have_parent = false;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) return std::nullopt;
    const std::string name = argv[i];
    if (name == "--ipc-read" && !have_read) {
      const auto handle = Decimal(argv[i + 1], UINT64_MAX / 10);
      if (!handle || *handle == 0) return std::nullopt;
      arguments.ipc_read = *handle;
      have_read = true;
    } else if (name == "--ipc-write" && !have_write) {
      const auto handle = Decimal(argv[i + 1], UINT64_MAX / 10);
      if (!handle || *handle == 0) return std::nullopt;
      arguments.ipc_write = *handle;
      have_write = true;
    } else if (name == "--parent-pid" && !have_parent) {
      const auto pid = Decimal(argv[i + 1], UINT32_MAX);
      if (!pid || *pid == 0) return std::nullopt;
      arguments.parent_pid = static_cast<uint32_t>(*pid);
      have_parent = true;
    } else {
      return std::nullopt;
    }
  }
  if (!have_read || !have_write || !have_parent) return std::nullopt;
  return arguments;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t ProcessId() { return GetCurrentProcessId(); }

bool PrepareProcess(const Arguments& arguments) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  g_launcher = OpenProcess(SYNCHRONIZE, FALSE, arguments.parent_pid);
  return g_launcher != nullptr && WaitForSingleObject(g_launcher, 0) == WAIT_TIMEOUT;
}

void WatchLauncher(const Arguments&, std::function<void()> on_gone) {
  HANDLE launcher = g_launcher;
  std::thread([launcher, on_gone] {
    WaitForSingleObject(launcher, INFINITE);
    on_gone();
  }).detach();
}

void ExitNow(int code) { TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code)); }

std::optional<SharedMemory> CreateSharedMemory(uint64_t bytes) {
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                      static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes & 0xFFFFFFFFu),
                                      nullptr);
  if (!section) return std::nullopt;
  void* data = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes));
  if (!data) {
    CloseHandle(section);
    return std::nullopt;
  }
  return SharedMemory{section, static_cast<uint8_t*>(data), bytes};
}

Library::~Library() { Close(); }

bool Library::Open(const std::string& path, std::string& error) {
  Close();
  const std::wstring wide = Wide(path);
  handle_ = wide.empty() ? nullptr : LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle_) {
    error = "The core could not be loaded (error " + std::to_string(GetLastError()) + ").";
    return false;
  }
  return true;
}

void* Library::Symbol(const char* name) const {
  return handle_ ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name)) : nullptr;
}

void Library::Close() {
  if (handle_) FreeLibrary(static_cast<HMODULE>(handle_));
  handle_ = nullptr;
}

bool CanRead(const std::string& path) {
  const std::wstring wide = Wide(path);
  if (wide.empty()) return false;
  HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const bool regular = GetFileType(file) == FILE_TYPE_DISK;
  BY_HANDLE_FILE_INFORMATION information{};
  const bool directory = GetFileInformationByHandle(file, &information) &&
                         (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  CloseHandle(file);
  return regular && !directory;
}

bool IsDirectory(const std::string& path) {
  const std::wstring wide = Wide(path);
  const DWORD attributes = wide.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool CreateDirectories(const std::string& path) {
  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(Wide(path)), error);
  return IsDirectory(path);
}

std::optional<std::string> ReadFile(const std::string& path) {
  std::ifstream in(std::filesystem::path(Wide(path)), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream data;
  data << in.rdbuf();
  return data.str();
}

bool WriteFileAtomically(const std::string& path, const std::string& data) {
  const std::wstring target = Wide(path);
  const std::wstring temporary = target + L".writing";
  {
    std::ofstream out(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out.flush()) return false;
  }
  return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::unique_ptr<Channel> OpenChannel(const Arguments& arguments) {
  return std::make_unique<PipeChannel>(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(arguments.ipc_read)),
                                       reinterpret_cast<HANDLE>(static_cast<uintptr_t>(arguments.ipc_write)));
}

}  // namespace retro_host
