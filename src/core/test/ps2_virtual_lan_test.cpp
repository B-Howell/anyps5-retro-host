#ifdef NDEBUG
#error "Native online tests require active assertions"
#endif

#include "../ps2_virtual_lan.h"

#include <cassert>

namespace {
const Ps2VirtualLan::Descriptor* active;
Ps2VirtualLan::Descriptor copied{};

bool Bind(const Ps2VirtualLan::Descriptor* descriptor) {
  if (descriptor) {
    copied = *descriptor;
    active = &copied;
  } else {
    active = nullptr;
  }
  return true;
}
}

int main() {
  const Ps2VirtualLan::Mac mac{2, 0, 0, 0, 0, 1};
  Ps2VirtualLan lan(mac);
  assert(lan.Attach(Bind));
  assert(active && active->mac[0] == 2 && active->mac[5] == 1);
  std::array<uint8_t, 64> outbound{};
  std::fill_n(outbound.begin(), 6, uint8_t{255});
  std::copy(mac.begin(), mac.end(), outbound.begin() + 6);
  assert(active->send(active->opaque, outbound.data(), outbound.size()));
  outbound[13] = 42;
  std::array<uint8_t, 1514> frame{};
  assert(lan.Drain(frame.data(), frame.size()) == 64 && frame[13] == 0);
  assert(lan.Drain(frame.data(), frame.size()) == 0);
  outbound[11] = 2;
  assert(!active->send(active->opaque, outbound.data(), outbound.size()));
  assert(!active->send(active->opaque, outbound.data(), 13));

  assert(lan.Inject(outbound.data(), outbound.size()));
  assert(active->recv(active->opaque, frame.data(), frame.size()) == 64 && frame[13] == 42);
  std::copy(mac.begin(), mac.end(), outbound.begin());
  assert(lan.Inject(outbound.data(), outbound.size()));
  assert(active->recv(active->opaque, frame.data(), 20) == 0);
  assert(active->recv(active->opaque, frame.data(), frame.size()) == 64);
  outbound[5] = 3;
  assert(!lan.Inject(outbound.data(), outbound.size()));
  outbound[5] = 1;
  for (size_t index = 0; index < Ps2VirtualLan::kQueueSize; ++index) {
    assert(lan.Inject(outbound.data(), outbound.size()));
  }
  assert(!lan.Inject(outbound.data(), outbound.size()));
  outbound[11] = 1;
  assert(active->send(active->opaque, outbound.data(), outbound.size()));
  lan.ClearFrames();
  assert(lan.Drain(frame.data(), frame.size()) == 0);
  assert(active->recv(active->opaque, frame.data(), frame.size()) == 0);
  assert(lan.Inject(outbound.data(), outbound.size()));
  lan.Close();
  assert(!lan.Inject(outbound.data(), outbound.size()));
  assert(active->recv(active->opaque, frame.data(), frame.size()) == 0);
  assert(lan.Detach());
  assert(active == nullptr);
}
