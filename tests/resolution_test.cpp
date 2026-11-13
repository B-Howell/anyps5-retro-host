// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <cstdio>
#include <string>
#include <vector>

#include "resolution.h"

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::vector<std::string> Scales(int last) {
  std::vector<std::string> values;
  for (int i = 1; i <= last; ++i) values.push_back(std::to_string(i));
  return values;
}

}  // namespace

int main() {
  using retro_host::LimitResolution;
  using retro_host::ParseScale;
  const auto scales = Scales(16);

  const auto at_16 = LimitResolution(scales, 15, 15, 16384, 8192, 8192, 8192);
  Expect(at_16 && at_16->offered.size() == 8 && at_16->offered.back() == 7, "VRAM times 16 offers 1x to 8x within 8192");
  Expect(at_16 && at_16->selected == 7, "16x moves to 8x");
  Expect(at_16 && at_16->width == 8192 && at_16->height == 4096, "8x of 1024 x 512 is 8192 x 4096");
  Expect(at_16 && at_16->refused_width == 16384 && at_16->refused_height == 8192, "16x would have been 16384 x 8192");

  const auto at_2 = LimitResolution(scales, 1, 1, 2048, 1024, 8192, 8192);
  Expect(at_2 && at_2->offered.size() == 8 && at_2->selected == 1, "2x stays and the same eight scales are offered");
  Expect(at_2 && at_2->width == 2048 && at_2->refused_width == 0, "2x is not refused");

  const auto small_gpu = LimitResolution(scales, 15, 15, 16384, 8192, 4096, 8192);
  Expect(small_gpu && small_gpu->offered.size() == 4 && small_gpu->selected == 3, "a 4096-wide GPU limit offers 1x to 4x");

  const auto tall = LimitResolution(scales, 0, 0, 1024, 3000, 8192, 8192);
  Expect(tall && tall->offered.size() == 2, "the height limits too");

  const auto pending = LimitResolution(scales, 11, 1, 2048, 1024, 8192, 8192);
  Expect(pending && pending->selected == 7 && pending->refused_width == 12288, "a pending 12x is judged from the 2x geometry");

  Expect(at_16 && !at_16->active_offered, "the core's 16x is not offered");
  Expect(at_2 && at_2->active_offered, "the core's 2x is offered");
  Expect(pending && pending->active_offered, "a pending 12x leaves the core's 2x offered");

  const auto chosen_2 = LimitResolution(scales, 1, 15, 16384, 8192, 8192, 8192);
  Expect(chosen_2 && chosen_2->selected == 1 && !chosen_2->active_offered && chosen_2->refused_width == 0,
         "2x chosen while the core draws at 16x is kept, and the core's 16x is not offered");
  Expect(chosen_2 && chosen_2->width == 2048 && chosen_2->height == 1024, "the geometry is the chosen 2x one");

  Expect(!LimitResolution(scales, 0, 0, 16384, 16384, 8192, 8192), "no scale fits: nothing to offer");
  Expect(!LimitResolution({"auto", "1", "2"}, 0, 0, 1024, 512, 8192, 8192), "an active value that is not a scale cannot be judged");
  const auto mixed = LimitResolution({"1", "auto", "16"}, 2, 2, 16384, 8192, 8192, 8192);
  Expect(mixed && mixed->offered == std::vector<size_t>({0, 1}) && mixed->selected == 0, "a value that is not a scale stays offered");
  Expect(!LimitResolution(scales, 16, 0, 1024, 512, 8192, 8192), "an index past the values is refused");

  Expect(ParseScale("16") == 16u && ParseScale("1") == 1u && ParseScale("1024") == 1024u, "whole scales parse");
  Expect(!ParseScale("0") && !ParseScale("01") && !ParseScale("1025") && !ParseScale("2x") && !ParseScale("") &&
             !ParseScale("-1") && !ParseScale("99999"),
         "other text is not a scale");

  if (failures) return 1;
  std::puts("All resolution checks passed.");
  return 0;
}
