// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <cstdio>
#include <string>

#include "hash.h"

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using retro_host::Fnv;
  using retro_host::FnvZeros;
  using retro_host::UsedLength;

  Expect(Fnv("", 0) == retro_host::kFnvOffset, "no bytes leave the offset basis");
  Expect(Fnv("a", 1) == 0xAF63DC4C8601EC8CULL, "FNV-1a 64 of \"a\" is the published value");
  Expect(Fnv("foobar", 6) == 0x85944171F73967E8ULL, "FNV-1a 64 of \"foobar\" is the published value");

  for (const uint64_t count : {0ULL, 1ULL, 2ULL, 7ULL, 8ULL, 9ULL, 63ULL, 64ULL, 1000ULL, 4097ULL}) {
    const std::string zeros(static_cast<size_t>(count), '\0');
    Expect(FnvZeros(count) == Fnv(zeros.data(), zeros.size()), "a run of zeros hashes as byte by byte");
    Expect(FnvZeros(count, 12345) == Fnv(zeros.data(), zeros.size(), 12345), "and continues any hash");
  }

  const auto zero_tail = [](const std::string& buffer) {
    const size_t used = UsedLength(buffer.data(), buffer.size());
    return FnvZeros(buffer.size() - used, Fnv(buffer.data(), used));
  };
  for (size_t size = 0; size < 40; ++size) {
    for (size_t last = 0; last <= size; ++last) {
      std::string buffer(size, '\0');
      for (size_t at = 0; at < last; at += 3) buffer[at] = static_cast<char>(at + 1);
      if (last) buffer[last - 1] = 'x';
      if (UsedLength(buffer.data(), buffer.size()) != last) {
        std::fprintf(stderr, "FAIL: used length of %zu bytes ending at %zu\n", size, last);
        ++failures;
      }
      if (zero_tail(buffer) != Fnv(buffer.data(), buffer.size())) {
        std::fprintf(stderr, "FAIL: zero tail hash of %zu bytes ending at %zu\n", size, last);
        ++failures;
      }
    }
  }

  std::string state(11 * 1024 * 1024, '\0');
  for (size_t at = 0; at < 3 * 1024 * 1024; at += 4093) state[at] = static_cast<char>(at);
  state[3 * 1024 * 1024 + 5] = 1;
  Expect(zero_tail(state) == Fnv(state.data(), state.size()),
         "a state-sized buffer hashes the same with its zero tail skipped");

  if (failures) return 1;
  std::puts("All hash checks passed.");
  return 0;
}
