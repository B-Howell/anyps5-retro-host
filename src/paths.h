// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_PATHS_H_
#define ANYPS5_RETRO_HOST_PATHS_H_

#include <string>
#include <string_view>

namespace retro_host {

// A path as protocol.md "Paths" allows it for this system: absolute, with no NUL and no ".",
// ".." or empty segment.
bool IsProtocolPath(std::string_view path, bool windows);

// Whether path ends with extension (".cue"), ignoring ASCII case.
bool HasExtension(std::string_view path, std::string_view extension);

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_PATHS_H_
