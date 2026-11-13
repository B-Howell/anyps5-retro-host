// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

// Makes and drops an OpenGL context on a thread that then exits, twice, as two games in one host
// do. Each context is dropped on its own thread, as Game::DropGpu does, and the thread then ends:
// a system library left with state for that thread after the host unloads it crashes here.
// Exits 77 (skipped) when the system has no OpenGL context.

#include <cstdio>
#include <thread>

#include "gpu.h"

namespace {

constexpr int kSkipped = 77;

enum class Outcome { kPassed, kNoContext, kFailed };

Outcome OneGame() {
  auto gpu = retro_host::Gpu::Create(3, 3, true);
  if (!gpu) return Outcome::kNoContext;
  const bool reserved = gpu->Reserve(64, 64, true, true) && gpu->Framebuffer() != 0;
  gpu->ReleaseFramebuffer();
  gpu.reset();
  return reserved ? Outcome::kPassed : Outcome::kFailed;
}

}  // namespace

int main() {
  for (int game = 1; game <= 2; ++game) {
    Outcome outcome = Outcome::kFailed;
    std::thread thread([&outcome] { outcome = OneGame(); });
    thread.join();
    if (outcome == Outcome::kNoContext) {
      if (game == 1) {
        std::printf("skipped: no OpenGL 3.3 core context\n");
        return kSkipped;
      }
      std::fprintf(stderr, "FAIL: game %d made no context after game 1 did\n", game);
      return 1;
    }
    if (outcome == Outcome::kFailed) {
      std::fprintf(stderr, "FAIL: game %d could not make a 64 x 64 framebuffer\n", game);
      return 1;
    }
  }
  std::printf("two contexts made and dropped on threads that exited\n");
  return 0;
}
