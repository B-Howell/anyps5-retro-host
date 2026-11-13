// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <cstdio>
#include <string>

#include "json.h"
#include "paths.h"

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
  using retro_host::ParseJson;
  Expect(ParseJson(R"({"type":"ping","id":1})").has_value(), "a plain object parses");
  Expect(!ParseJson(R"({"a":1,"a":2})"), "a duplicate key is refused");
  Expect(!ParseJson(R"({"a":{"b":1,"b":1}})"), "a nested duplicate key is refused");
  Expect(!ParseJson(R"({"a":"\ud800"})"), "a lone high surrogate is refused");
  Expect(!ParseJson(R"({"a":"\udc00"})"), "a lone low surrogate is refused");
  Expect(ParseJson(R"({"a":"\ud83d\ude00"})").has_value(), "a surrogate pair parses");
  Expect(!ParseJson("{\"a\":\"\xff\"}"), "invalid UTF-8 is refused");
  Expect(!ParseJson("{\"a\":\"\xed\xa0\x80\"}"), "an encoded surrogate is refused");
  Expect(!ParseJson(R"({"a":1e400})"), "a number that is not finite is refused");
  Expect(!ParseJson(R"({"a":NaN})"), "NaN is refused");
  Expect(!ParseJson(R"({"a":01})"), "a leading zero is refused");
  Expect(!ParseJson(R"({"a":1} x)"), "trailing text is refused");
  const auto one = ParseJson(R"({"a":1.0,"b":1e0,"c":1.5})");
  Expect(one && one->Find("a")->Integer(1, 1) == 1, "1.0 is the integer 1");
  Expect(one && one->Find("b")->Integer(1, 1) == 1, "1e0 is the integer 1");
  Expect(one && !one->Find("c")->Integer(0, 2), "1.5 is not an integer");
  const auto text = ParseJson(R"({"s":"a\"b\\c\n\u00e9"})");
  Expect(text && text->Serialize() == "{\"s\":\"a\\\"b\\\\c\\n\xc3\xa9\"}", "strings round-trip");

  using retro_host::IsProtocolPath;
  Expect(IsProtocolPath("/home/a/b.cue", false), "a POSIX absolute path");
  Expect(!IsProtocolPath("/home/../b.cue", false), "a POSIX path with ..");
  Expect(!IsProtocolPath("/a//b", false), "a POSIX path with an empty segment");
  Expect(!IsProtocolPath("C:/a.cue", false), "a Windows path on POSIX");
  Expect(IsProtocolPath(R"(C:\Games\a.cue)", true), "a Windows drive path");
  Expect(IsProtocolPath("C:/Games/a.cue", true), "a Windows drive path with slashes");
  Expect(IsProtocolPath(R"(\\nas\games\a.cue)", true), "a UNC path");
  Expect(IsProtocolPath(R"(\\?\C:\Games\a.cue)", true), "a long drive path");
  Expect(IsProtocolPath(R"(\\?\UNC\nas\games\a.cue)", true), "a long UNC path");
  Expect(!IsProtocolPath(R"(\\?\C:)", true), "a long drive with no root");
  Expect(!IsProtocolPath(R"(\\nas\..\a.cue)", true), "a UNC share of ..");
  Expect(!IsProtocolPath(R"(\\.\PhysicalDrive0)", true), "a device path");
  Expect(!IsProtocolPath("C:a.cue", true), "a drive-relative path");
  Expect(!IsProtocolPath(R"(\a.cue)", true), "a root-relative path");
  Expect(!IsProtocolPath(std::string("/a\0b", 4), false), "a path with NUL");
  Expect(retro_host::HasExtension("/a/Game.CUE", ".cue"), "extensions ignore case");

  if (failures == 0) std::printf("All JSON and path checks passed.\n");
  return failures == 0 ? 0 : 1;
}
