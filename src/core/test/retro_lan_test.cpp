#include "retro_library.h"
#include "retro_session.h"
#include "retro_test_support.h"

#include <iostream>

namespace retro = anyps5::retro;
using namespace retro::test;

int main(int argc, char** argv) {
  try {
    Check(argc == 3, "Pass the LAN and unsupported core paths");
    const retro::DynamicLibrary library(std::filesystem::u8path(argv[1]));
    const auto seen = library.Get<unsigned (*)()>("test_lan_seen");
    const auto sent = library.Get<unsigned (*)()>("test_lan_sent");
    const auto hold = library.Get<void (*)(bool)>("test_lan_hold");
    const auto bound = library.Get<bool (*)()>("test_lan_bound");
    const auto wrong_thread = library.Get<bool (*)()>("test_wrong_disk_thread");
    retro::SessionConfig config{argv[1], "test.cue", "", "", "", {}};
    config.lan_mac = std::array<uint8_t, 6>{2, 41, 42, 43, 44, 1};
    retro::Session session(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
    Check(!session.ExchangeLan({}), "LAN exchanged before the core was ready");
    session.Start();
    WaitFor([&] { return session.Status().ready || session.ended(); }, "LAN core did not become ready");
    if (!session.Status().error.empty() || !bound())
      throw std::runtime_error("Core did not bind before game load: " + session.Status().error);
    const auto steps = session.Status().telemetry.steps;
    session.Pause(true);
    WaitFor([&] { return session.Status().telemetry.steps > steps + 2; }, "LAN pause froze the game");

    std::vector<uint8_t> frame(64, 0x42);
    std::copy(config.lan_mac->begin(), config.lan_mac->end(), frame.begin());
    const std::array<uint8_t, 6> peer{2, 41, 42, 43, 44, 2};
    std::copy(peer.begin(), peer.end(), frame.begin() + 6);
    frame[12] = 8; frame[13] = 0;
    Check(!session.ExchangeLan({frame, {1}}), "Malformed batch was partly accepted");
    Check(seen() == 0, "Malformed batch injected an earlier valid packet");
    Check(session.ExchangeLan({frame}).has_value(), "Valid peer frame was rejected");
    WaitFor([&] { return sent() == 1; }, "Core did not receive and send the private frame");
    const auto output = session.ExchangeLan({});
    Check(output && output->size() == 1 && output->front().size() == frame.size(), "Missing outbound frame");
    Check(std::equal(peer.begin(), peer.end(), output->front().begin()), "Wrong outbound destination");
    Check(std::equal(config.lan_mac->begin(), config.lan_mac->end(), output->front().begin() + 6), "Wrong outbound source");

    hold(true);
    Check(session.ExchangeLan({frame}).has_value(), "Could not queue the pre-reset packet");
    Check(session.ClearLanFrames(), "Native queue reset failed");
    hold(false);
    const auto reset_steps = session.Status().telemetry.steps;
    WaitFor([&] { return session.Status().telemetry.steps > reset_steps + 2; }, "Core stalled after queue reset");
    Check(seen() == 1 && session.ExchangeLan({})->empty(), "Pre-reset packet survived reset");
    session.Stop(); session.Join();
    Check(!bound() && !wrong_thread(), "NIC detach or callback ownership failed");
    Check(!session.ExchangeLan({}) && !session.ClearLanFrames(), "Closed session accepted LAN traffic");

    config.core = argv[2];
    retro::Session unsupported(config, std::make_unique<TestVideo>(), std::make_unique<TestAudio>(), [] {});
    unsupported.Start(); unsupported.Join();
    Check(unsupported.Status().error.find("retro_anyps5_dev9_bind_v1") != std::string::npos,
          "Missing private-NIC export did not fail before boot");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
