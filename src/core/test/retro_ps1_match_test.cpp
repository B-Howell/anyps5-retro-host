#include "ps1_match.h"
#include "retro_session.h"
#include "retro_test_support.h"

#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace anyps5::retro;
using namespace anyps5::retro::test;

namespace {
struct StagedMatch {
  ps1_checkpoint::Snapshot snapshot;
  Ps1MatchHistory history;
};

StagedMatch StageHost(const char* core, const std::filesystem::path& root) {
  SessionConfig config{core, "test.cue", root.u8string(), root.u8string(), "", {}};
  config.ps1_match = Ps1MatchConfig{0, (root / "bios.bin").u8string(), true, true};
  Session host(std::move(config), std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  host.SetInput(4, {});
  host.Start();
  WaitFor([&] { return host.Status().telemetry.steps >= 3; }, "Host did not stage local play");
  Check(!host.StartNetplay(), "Host switched to the match before holding");
  ps1_checkpoint::Digest digest{};
  const auto ticket = host.RequestMatchExport(digest);
  Check(ticket.has_value(), "Staged checkpoint was rejected");
  ps1_checkpoint::Mailbox::Reply reply;
  WaitFor([&] { reply = host.TakeCheckpoint(*ticket); return !std::holds_alternative<std::monostate>(reply); },
          "Staged checkpoint did not complete");
  auto snapshot = std::get<ps1_checkpoint::Snapshot>(std::move(reply));
  WaitFor([&] { return host.Status().telemetry.steps >= snapshot.frame + 5; }, "Host did not advance after capture");
  std::optional<uint32_t> held;
  WaitFor([&] { held = host.HoldNetplay(); return held.has_value(); }, "Host did not hold at a frame boundary");
  Check(host.ResumeLocalNetplay(), "Held game could not resume local play");
  const auto previous = *held;
  WaitFor([&] { return host.Status().telemetry.steps > previous; }, "Resumed host did not advance");
  WaitFor([&] { held = host.HoldNetplay(); return held.has_value(); }, "Resumed host did not hold again");
  const auto history = host.MatchHistory(snapshot.frame);
  Check(history && history->next_frame == *held && history->frames.size() == *held - snapshot.frame &&
        history->frames.front()[0].buttons == 4 && history->frames.front()[1].buttons == 0,
        "Host did not retain both controller ports through cutover");
  Check(host.StartNetplay(), "Held host could not start the synchronized match");
  const auto first = host.SyncNetplay({}, {});
  Check(first && first->frame == 0 && first->waiting, "Host did not start a new input timeline");
  host.Stop(); host.Join();
  return {std::move(snapshot), *history};
}

void CheckSeat(const char* core, const std::filesystem::path& root, unsigned seat,
               const StagedMatch& staged, const std::string& expected) {
  const std::array<ps1_netplay::PadInput, 2> pads{{{1, {-22, 0, 0, 0}}, {2, {33, 0, 0, 0}}}};
  SessionConfig config{core, "test.cue", root.u8string(), root.u8string(), "",
                       {{"swanstation_CDROM_ReadThread", "true"}}};
  config.ps1_match = Ps1MatchConfig{seat, (root / "bios.bin").u8string(), true};
  Session match(std::move(config), std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
  Check(!match.StartNetplay(), "Match started before the core was ready");
  match.Start();
  WaitFor([&] { return match.Status().netplay_ready || match.ended(); }, "Match worker did not become ready");
  Check(match.Status().netplay_ready, match.Status().error.c_str());
  Check(match.Status().options.at("swanstation_CDROM_ReadThread").active_value == "false",
        "An offline option override reached the match");
  Check(!match.AcquireFrame() && match.Status().telemetry.steps == 0, "Core advanced before match start");
  Check(match.Status().portable_checkpoint, "Portable checkpoint hooks were not advertised");
  ps1_checkpoint::Digest digest{};
  const auto rejected = match.RequestMatchImport(digest, 37, ps1_checkpoint::Bytes(7, 0));
  Check(rejected.has_value() && !match.StartNetplay(), "Pending import let the match start");
  ps1_checkpoint::Mailbox::Reply checkpoint;
  WaitFor([&] { checkpoint = match.TakeCheckpoint(*rejected); return !std::holds_alternative<std::monostate>(checkpoint); },
          "Rejected import did not complete");
  Check(!std::get<bool>(checkpoint) && !match.StartNetplay(), "Failed import let the match start");
  const auto saved_position = staged.snapshot.frame;
  const auto& state = staged.snapshot.state;
  const auto cancelled = match.RequestMatchImport(digest, saved_position, state);
  Check(cancelled.has_value(), "Could not queue an import to cancel");
  match.CancelCheckpoint(*cancelled);
  Check(!match.StartNetplay(), "Cancelled import let the match start");
  std::optional<int64_t> imported;
  WaitFor([&] { imported = match.RequestMatchImport(digest, saved_position, state); return imported.has_value(); },
          "Cancelled import blocked a fresh retry");
  Check(imported.has_value() && !match.StartNetplay(), "Retry started before its import result was collected");
  WaitFor([&] { checkpoint = match.TakeCheckpoint(*imported); return !std::holds_alternative<std::monostate>(checkpoint); },
          "Valid import did not complete");
  Check(std::get<bool>(checkpoint), "Valid import failed");
  Check(!match.RequestMatchReplay(saved_position + 1, staged.history.frames), "Guest accepted a gap in catch-up");
  const auto replay = match.RequestMatchReplay(saved_position, staged.history.frames);
  Check(replay && !match.StartNetplay(), "Queued replay let the match start");
  WaitFor([&] { checkpoint = match.TakeCheckpoint(*replay); return !std::holds_alternative<std::monostate>(checkpoint); },
          "Catch-up replay did not complete");
  Check(std::get<bool>(checkpoint) && !match.AcquireFrame(), "Catch-up did not stay off the presentation surface");
  Check(!match.RequestMatchReplay(saved_position, staged.history.frames), "Guest accepted duplicate catch-up");
  match.SetInput(pads[seat].buttons, pads[seat].axes);
  Check(match.StartNetplay() && match.StartNetplay(), "Match start was not idempotent");
  Check(!match.RequestMatchImport(digest, 37, state), "Running match accepted a checkpoint import");
  Check(match.SyncNetplay({{0, {}}}, {}).has_value(), "First remote frame rejected");
  WaitFor([&] {
    const auto tick = match.SyncNetplay({}, {});
    return tick && tick->frame == 1 && tick->waiting;
  }, "Worker did not stop at the missing remote frame");
  Check(!match.SyncNetplay({{0, pads[1 - seat]}}, {}), "Conflicting input rewrote a confirmed frame");

  uint32_t next_remote = 1;
  std::optional<uint32_t> ack;
  Ps1MatchTick result{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (result.frame < 120 && std::chrono::steady_clock::now() < deadline) {
    std::vector<ps1_netplay::FrameInput> batch;
    while (next_remote < 120 && next_remote <= result.frame + 8) {
      batch.push_back({next_remote, next_remote < 2 ? ps1_netplay::PadInput{} : pads[1 - seat]});
      ++next_remote;
    }
    const auto update = match.SyncNetplay(batch, ack);
    Check(update.has_value(), "Match exchange rejected valid frames");
    result = *update;
    if (!result.local.empty()) ack = result.local.back().frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  Check(result.frame == 120 && result.confirmed == 119 && result.received == 119,
        "Worker did not finish the confirmed trace");
  Check(result.fingerprints.size() == 1 && result.fingerprints[0] == std::make_pair(119u, expected),
        "Wrong controller routing or RAM fingerprint");
  Check(match.AcquireFrame() != nullptr, "Match did not present a frame");
  Check(!match.Status().checkpoint_source, "Match exposed ordinary recovery");
  Check(!match.SetOption("swanstation_CPU_ExecutionMode", "Recompiler"), "Match accepted a profile change");
  match.Stop(); match.Join();
  Check(!match.SyncNetplay({}, {}), "Stopped match accepted network input");
  Check(!std::filesystem::exists(root / "test.srm"), "Match wrote to the ordinary save directory");
}
}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path root;
  try {
    Check(argc == 2, "Pass the PS1 fixture");
    root = std::filesystem::temp_directory_path() /
        ("anyps5-ps1-match-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(root), "Could not create match test directory");
    {
      std::ofstream bios(root / "bios.bin", std::ios::binary);
      bios.seekp(512 * 1024 - 1);
      bios.put(0);
      Check(bios.good(), "Could not write the synthetic firmware fixture");
    }
#ifndef _WIN32
    {
      const Ps1MatchStorage storage((root / "bios.bin").u8string());
      const auto permissions = std::filesystem::status(storage.content()).permissions();
      Check((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                std::filesystem::perms::none,
            "Match firmware directory was accessible to other users");
    }
#endif
    std::vector<uint8_t> ram(2 * 1024 * 1024);
    ram[0] = 1;
    ram[1] = static_cast<uint8_t>(-22);
    ram[2] = 2;
    ram[3] = 33;
    const auto staged = StageHost(argv[1], root);
    const auto cursor = staged.history.next_frame + 120;
    for (unsigned i = 0; i < 4; ++i) ram[4 + i] = static_cast<uint8_t>(cursor >> (8 * i));
    ram[8] = 4;
    const auto expected = NetplayFingerprint(ram.data(), ram.size());
    CheckSeat(argv[1], root, 0, staged, expected);
    CheckSeat(argv[1], root, 1, staged, expected);
    std::filesystem::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (!root.empty()) std::filesystem::remove_all(root);
    return 1;
  }
}
