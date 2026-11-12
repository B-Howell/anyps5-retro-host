#include "ps1_checkpoint.h"
#include "retro_test_support.h"

#include <cstring>
#include <iostream>
#include <future>
#include <functional>

using namespace anyps5::retro;
using anyps5::retro::test::Check;

namespace {
uint32_t position = 123;
bool fail_private = false, fail_load = false;
std::function<void()> on_blank;
size_t Blank(void* out, size_t capacity, const uint8_t*) {
  if (on_blank) on_blank();
  if (capacity < 4) return 0;
  std::memset(out, 0, 4);
  return 4;
}
size_t Private(void* out, size_t capacity, const uint8_t*) {
  if (fail_private || capacity < 4) return 0;
  std::memcpy(out, &position, 4);
  return 4;
}
bool Load(const void* bytes, size_t size, const uint8_t*) {
  if (fail_load || size != 4) return false;
  std::memcpy(&position, bytes, 4);
  return true;
}
}

int main() {
  try {
    ps1_checkpoint::Mailbox queue;
    ps1_checkpoint::Digest digest{};
    Check(!queue.RequestCapture(digest, false), "Unconfigured core accepted a checkpoint");
    queue.Configure({Blank, Private, Load, 64});
    Check(queue.ready(), "Portable core did not become available");
    const auto ticket = queue.RequestCapture(digest, true);
    Check(ticket.has_value() && !queue.RequestCapture(digest, false), "Concurrent checkpoint accepted");
    Check(std::holds_alternative<std::monostate>(queue.Take(*ticket)), "Queued export was not pending");
    const auto captured = queue.Service(42);
    Check(captured.flush_audio && !captured.refresh_memory && queue.parked(), "Capture did not park at its boundary");
    const auto result = queue.Take(*ticket);
    const auto* snapshot = std::get_if<ps1_checkpoint::Snapshot>(&result);
    Check(snapshot && snapshot->frame == 42 && snapshot->state == std::vector<uint8_t>(4), "Public checkpoint leaked private card data");
    Check(position == 123 && !queue.RequestCapture(digest, false), "Park changed the original game or accepted a second capture");
    position = 999;
    const auto resume = queue.RequestResume();
    Check(resume.has_value(), "Parked game cannot request resume");
    const auto restored = queue.Service(99);
    Check(restored.refresh_memory && restored.flush_audio && !queue.parked(), "Resume effects were lost");
    Check(std::get<bool>(queue.Take(*resume)) && position == 123, "Original private state was not restored");

    fail_private = true;
    const auto failed = queue.RequestCapture(digest, true);
    queue.Service(45);
    Check(!std::get<bool>(queue.Take(*failed)) && !queue.parked(), "Failed private backup parked the game");
    fail_private = false;
    const auto cancelled = queue.RequestCapture(digest, true);
    queue.Cancel(*cancelled);
    queue.Service(46);
    Check(!std::get<bool>(queue.Take(*cancelled)) && !queue.parked(), "Cancelled export parked the game");

    const auto parked = queue.RequestCapture(digest, true);
    queue.Service(47);
    queue.Cancel(*parked);
    Check(!queue.parked(), "Cancelling a completed capture left the game stuck");
    const auto again = queue.RequestCapture(digest, true);
    queue.Service(48);
    queue.Take(*again);
    fail_load = true;
    const auto retry = queue.RequestResume();
    Check(queue.Service(49).refresh_memory, "Attempted restore did not refresh the card pointer");
    Check(!std::get<bool>(queue.Take(*retry)) && queue.parked(), "Failed restore lost its recovery checkpoint");
    fail_load = false;
    const auto good = queue.RequestResume();
    queue.Service(50);
    Check(std::get<bool>(queue.Take(*good)) && !queue.parked(), "Failed restore could not be retried");

    const auto invalid_frame = queue.RequestCapture(digest, false);
    queue.Service(ps1_netplay::Timeline::kMaxFrame + uint64_t{1});
    Check(!std::get<bool>(queue.Take(*invalid_frame)), "Out-of-range frame was exported");
    queue.Stop();
    Check(!queue.ready() && !queue.RequestCapture(digest, false) && !queue.RequestResume(), "Stopped queue accepted work");
    Check(!std::get<bool>(queue.Take(*ticket)), "Stopped queue exposed a prior result");

    // Cancel and stop while the worker is inside a core exporter. Completion
    // must not resurrect the request, retain private state or park the game.
    for (const bool stop : {false, true}) {
      ps1_checkpoint::Mailbox concurrent;
      concurrent.Configure({Blank, Private, Load, 64});
      std::promise<void> entered, release;
      auto entered_future = entered.get_future();
      auto released = release.get_future().share();
      on_blank = [&] {
        entered.set_value();
        if (released.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
          throw std::runtime_error("Core-export test gate timed out");
      };
      const auto pending = concurrent.RequestCapture(digest, true);
      auto worker = std::async(std::launch::async, [&] { return concurrent.Service(100); });
      Check(entered_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "Worker did not enter the core exporter");
      if (stop) concurrent.Stop(); else concurrent.Cancel(*pending);
      release.set_value();
      worker.get();
      on_blank = {};
      Check(!concurrent.parked() && !std::get<bool>(concurrent.Take(*pending)),
            "An in-flight cancelled exporter parked the game or exposed its result");
      Check(concurrent.ready() != stop, "In-flight completion changed the stopped state");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
