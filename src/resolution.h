// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_RESOLUTION_H_
#define ANYPS5_RETRO_HOST_RESOLUTION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace retro_host {

// The internal resolution values a GPU game can be offered (docs/host.md, "Resolution limits").
// Each value is a whole scale; the core's largest geometry grows with it, so a value fits when
// max_width x max_height, reported while the core drew at values[active], times value / active
// stays within width_limit x height_limit.
struct ResolutionLimit {
  // Indexes into values of the values offered, in their order. A value that is not a whole
  // scale of 1 or more cannot be judged and stays offered.
  std::vector<size_t> offered;
  // Index into values of the value to use: selected when it is offered, otherwise the largest
  // scale offered.
  size_t selected = 0;
  // The largest geometry at values[selected].
  uint32_t width = 0;
  uint32_t height = 0;
  // The largest geometry the value asked for would have had, when it is not offered.
  uint32_t refused_width = 0;
  uint32_t refused_height = 0;
};

// std::nullopt when values[active] is not a whole scale, or when no scale fits.
std::optional<ResolutionLimit> LimitResolution(const std::vector<std::string>& values, size_t selected,
                                               size_t active, uint32_t max_width, uint32_t max_height,
                                               uint32_t width_limit, uint32_t height_limit);

// The whole scale value names (decimal digits, 1 to 1024), or std::nullopt.
std::optional<uint32_t> ParseScale(const std::string& value);

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_RESOLUTION_H_
