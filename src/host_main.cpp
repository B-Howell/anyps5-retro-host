#include "host_server.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace {
void RedirectLog() {
#if defined(_WIN32)
  wchar_t path[MAX_PATH];
  const auto length = GetEnvironmentVariableW(L"ANYPS5_RETRO_HOST_LOG", path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return;
  FILE* stream = nullptr;
  _wfreopen_s(&stream, path, L"a", stdout);
  _wfreopen_s(&stream, path, L"a", stderr);
#else
  const char* path = std::getenv("ANYPS5_RETRO_HOST_LOG");
  if (!path || !*path) return;
  if (!std::freopen(path, "a", stdout) || !std::freopen(path, "a", stderr)) return;
#endif
  std::setvbuf(stderr, nullptr, _IONBF, 0);
}
}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
    std::string argument(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, argument.data(), size, nullptr, nullptr);
    arguments.push_back(std::move(argument));
  }
#else
int main(int argc, char** argv) {
  std::vector<std::string> arguments(argv + 1, argv + argc);
#endif
  RedirectLog();
  auto channel = anyps5::retro_ipc::ConnectFromArguments(arguments);
  if (!channel) {
    std::fprintf(stderr, "anyps5-retro-host is started by AnyPS5 Desktop and cannot run on its own.\n");
    return 2;
  }
  anyps5::retro_host::HostOptions options;
  for (const auto& argument : arguments) options.probe = options.probe || argument == "--probe";
  return anyps5::retro_host::RunHost(std::move(channel), options);
}
