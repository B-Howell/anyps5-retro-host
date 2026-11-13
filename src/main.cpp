// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <cstdio>

#include "channel.h"
#include "host.h"
#include "platform.h"

int main(int argc, char** argv) {
  const auto arguments = retro_host::ParseArguments(argc, argv);
  if (!arguments) {
    std::fprintf(stderr,
                 "anyps5-retro-host is started by the AnyPS5 launcher (docs/protocol.md, \"Starting the host\").\n");
    return 64;
  }
  if (!retro_host::PrepareProcess(*arguments)) return retro_host::kExitClean;
  auto channel = retro_host::OpenChannel(*arguments);
  if (!channel) return 64;
  retro_host::Host host(*channel);
  retro_host::WatchLauncher(*arguments, [&host] { host.LauncherGone(); });
  const int code = host.Run();
  std::fflush(nullptr);
  retro_host::ExitNow(code);
}
