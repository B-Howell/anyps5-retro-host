#ifdef NDEBUG
#error "Native online tests require active assertions"
#endif

#include "../ps1_netplay_timeline.h"

#include <cassert>

namespace {

ps1_netplay::PadInput RecordedInput(uint32_t frame, uint32_t seat) {
  if (frame < ps1_netplay::Timeline::kDelay) return {};
  return {static_cast<uint16_t>((frame / 5 + seat) % 7),
          {static_cast<int16_t>(frame % 41), static_cast<int16_t>(seat), 0, 0}};
}

void VerifyDelayedReplay() {
  using ps1_netplay::FrameInput;
  using ps1_netplay::PadInput;
  using ps1_netplay::Timeline;

  Timeline guest(1, Timeline::kMaxPrediction);
  guest.Start();
  std::vector<std::array<PadInput, 2>> played;
  uint32_t verified = 0;
  uint32_t rewinds = 0;
  uint32_t published = 0;
  constexpr uint32_t kFrames = 2000;
  for (uint32_t tick = 0; tick < kFrames + 32 && verified < kFrames; ++tick) {
    std::vector<FrameInput> arrived;
    // Some newer inputs arrive first. Every gap has a delayed retry.
    if (tick % 3 == 0) arrived.push_back({tick, RecordedInput(tick, 0)});
    if (tick >= 9) arrived.push_back({tick - 9, RecordedInput(tick - 9, 0)});
    assert(guest.Receive(arrived, guest.LocalPacket().back().frame));
    if (const auto correction = guest.correction()) {
      assert(*correction >= verified && *correction < played.size());
      assert(guest.Rewind(*correction));
      played.resize(*correction);
      ++rewinds;
    }
    assert(guest.frame() >= published);
    for (int step = 0; step < 3 && guest.cursor() < kFrames; ++step) {
      std::array<PadInput, 2> ports{};
      if (!guest.Prepare(RecordedInput(guest.cursor() + Timeline::kDelay, 1), ports)) break;
      played.push_back(ports);
      guest.Commit();
    }
    published = guest.frame();
    if (const auto confirmed = guest.confirmed()) {
      for (; verified <= *confirmed; ++verified) {
        assert(played[verified][0] == RecordedInput(verified, 0));
        assert(played[verified][1] == RecordedInput(verified, 1));
      }
    }
  }
  assert(verified == kFrames && rewinds > 100);
}

}  // namespace

int main() {
  using ps1_netplay::FrameInput;
  using ps1_netplay::PadInput;
  using ps1_netplay::Timeline;

  Timeline host(0);
  Timeline guest(1);
  const PadInput host_pad{1, {101, -102, 103, -104}};
  const PadInput guest_pad{2, {-201, 202, -203, 204}};
  std::array<PadInput, 2> host_inputs{};
  std::array<PadInput, 2> guest_inputs{};
  assert(!host.Prepare(host_pad, host_inputs));
  assert(host.frame() == 0 && !host.confirmed() && !host.received());
  assert(host.LocalPacket().size() == 2);
  host.Start();
  guest.Start();
  assert(!host.Prepare(host_pad, host_inputs));
  assert(host.LocalPacket().back() == (FrameInput{2, host_pad}));
  assert(host.Receive(guest.LocalPacket(), std::nullopt));
  assert(guest.Receive(host.LocalPacket(), std::nullopt));
  assert(host.Prepare({}, host_inputs) && guest.Prepare(guest_pad, guest_inputs));
  assert(host_inputs == guest_inputs);
  assert(host_inputs == (std::array<PadInput, 2>{}));
  host.Commit();
  guest.Commit();
  assert(host.confirmed() == 0u && host.frame() == 1);

  assert(host.Receive(guest.LocalPacket(), guest.received()));
  assert(guest.Receive(host.LocalPacket(), host.received()));
  assert(host.Prepare({}, host_inputs) && guest.Prepare({}, guest_inputs));
  assert(host_inputs == guest_inputs);
  host.Commit();
  guest.Commit();
  assert(host.Prepare({}, host_inputs) && guest.Prepare({}, guest_inputs));
  assert(host_inputs == guest_inputs);
  assert(host_inputs[0] == host_pad && host_inputs[1] == guest_pad);
  host.Commit();
  guest.Commit();

  // Invalid ACKs, speculative future input, and conflicting redundant inputs
  // leave the receiver's acknowledged frame untouched.
  const auto before = host.received();
  assert(!host.Receive({{4, {}}}, 1000));
  assert(!host.Receive({{4, {}}, {1000, {}}}, std::nullopt));
  assert(!host.Receive({{2, host_pad}}, std::nullopt));
  assert(host.received() == before);
  assert(host.Receive({{4, {}}}, std::nullopt));
  assert(host.received() == before);
  assert(host.Receive({{3, {}}}, std::nullopt));
  assert(host.received() == 4u);
  assert(host.Receive({{3, {}}}, std::nullopt));
  assert(host.received() == 4u);

  // A full, long run keeps memory and retransmission bounded.
  for (int tick = 0; tick < 2000; ++tick) {
    assert(host.Receive(guest.LocalPacket(), guest.received()));
    assert(guest.Receive(host.LocalPacket(), host.received()));
    assert(host.Prepare({}, host_inputs) && guest.Prepare({}, guest_inputs));
    assert(host_inputs == guest_inputs);
    host.Commit();
    guest.Commit();
    assert(host.LocalPacket().size() <= 8);
    assert(guest.LocalPacket().size() <= 8);
  }
  assert(host.frame() == 2003 && host.frame() == guest.frame());

  assert(host.Receive(guest.LocalPacket(), guest.received()));
  assert(host.Prepare(host_pad, host_inputs));
  assert(!host.Hold());
  host.Commit();
  assert(host.Hold());
  assert(!host.Prepare(host_pad, host_inputs));
  assert(host.frame() == 2004);
  host.Reset();
  assert(host.frame() == 0 && !host.received() && !host.confirmed());
  assert((host.LocalPacket() == std::vector<FrameInput>{{0, {}}, {1, {}}}));
  assert(!host.Prepare(host_pad, host_inputs));

  // With prediction enabled, a late matching sample confirms only completed
  // frames. A mismatch arriving mid-step waits for the next frame boundary.
  Timeline predicted(0, 2);
  predicted.Start();
  assert(predicted.Prepare(host_pad, host_inputs));
  assert(predicted.needs_checkpoint());
  assert(predicted.Receive({{0, {}}, {1, guest_pad}}, std::nullopt));
  assert(!predicted.confirmed());
  predicted.Commit();
  assert(predicted.confirmed() == 0u);
  assert(predicted.Prepare(host_pad, host_inputs));
  assert(!predicted.needs_checkpoint());
  assert(host_inputs[1] == guest_pad);
  predicted.Commit();
  assert(predicted.Prepare(host_pad, host_inputs));
  assert(predicted.needs_checkpoint());
  assert(host_inputs[1] == guest_pad);
  assert(predicted.Receive({{3, {}}, {2, {}}}, std::nullopt));
  assert(!predicted.correction());
  predicted.Commit();
  assert(predicted.correction() == 2u);
  assert(predicted.frame() == 3u && predicted.confirmed() == 1u);
  assert(!predicted.Prepare({}, host_inputs));
  assert(!predicted.Rewind(1));
  assert(predicted.Rewind(2));
  assert(predicted.cursor() == 2u && predicted.frame() == 3u);
  assert(predicted.Prepare({}, host_inputs));
  assert(host_inputs[0] == host_pad && host_inputs[1] == PadInput{});
  predicted.Commit();
  assert(predicted.confirmed() == 2u && !predicted.correction());
  assert(predicted.Prepare({}, host_inputs));
  predicted.Commit();
  assert(predicted.Prepare({}, host_inputs));
  predicted.Commit();
  assert(predicted.Prepare({}, host_inputs));
  predicted.Commit();
  assert(predicted.waiting());
  assert(!predicted.Prepare({}, host_inputs));
  assert(predicted.confirmed() == 3u);
  assert(predicted.Receive({{5, guest_pad}}, std::nullopt));
  assert(predicted.correction() == 5u);
  assert(predicted.Receive({{4, guest_pad}}, std::nullopt));
  assert(predicted.correction() == 4u);
  assert(!predicted.Rewind(5));
  assert(predicted.Rewind(4));
  assert(predicted.frame() == 6u && predicted.cursor() == 4u);
  assert(predicted.confirmed() == 3u);
  assert(predicted.Prepare({}, host_inputs));
  assert(host_inputs[1] == guest_pad);
  predicted.Commit();
  assert(predicted.Prepare({}, host_inputs));
  assert(host_inputs[1] == guest_pad);
  predicted.Commit();
  assert(predicted.confirmed() == 5u);
  predicted.Reset();
  predicted.Start();
  assert(predicted.Prepare(host_pad, host_inputs));
  predicted.Commit();
  assert(predicted.frame() == 1u && !predicted.confirmed());
  Timeline preview(0, 4);
  preview.Start();
  assert(preview.Prepare(host_pad, host_inputs));
  preview.Commit();
  assert(preview.Preview(1, host_pad, host_inputs));
  assert(preview.frame() == 1 && preview.cursor() == 1 && !preview.confirmed());
  assert(preview.LocalPacket().back() == (FrameInput{3, host_pad}));
  assert(!preview.Preview(4, guest_pad, host_inputs));
  assert(preview.Prepare(guest_pad, host_inputs));
  preview.Commit();
  assert(preview.Prepare({}, host_inputs));
  preview.Commit();
  assert(preview.Prepare({}, host_inputs));
  assert(host_inputs[0] == host_pad);
  Timeline burst(0);
  try {
    burst = Timeline(0, 16);
  } catch (const std::invalid_argument&) {
    return 1;
  }
  burst.Start();
  for (uint32_t frame = 0; frame < 16; ++frame) {
    assert(burst.Prepare(host_pad, host_inputs));
    burst.Commit();
  }
  assert(burst.waiting() && !burst.Prepare(host_pad, host_inputs));
  const auto fresh = burst.LocalPacket();
  const auto repair = burst.LocalPacket();
  if (fresh.size() != 8 || fresh.front().frame != 11 || fresh.back().frame != 18 ||
      repair.size() != 8 || repair.front().frame != 0 || repair.back().frame != 7)
    return 1;
  assert(burst.Receive({{0, guest_pad}}, std::nullopt));
  assert(burst.correction() == 0u && burst.Rewind(0));
  assert(burst.Prepare({}, host_inputs) && host_inputs[1] == guest_pad);
  burst.Commit();
  assert(burst.confirmed() == 0u && burst.frame() == 16);
  VerifyDelayedReplay();
}
