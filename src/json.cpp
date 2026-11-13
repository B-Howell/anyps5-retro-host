// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace retro_host {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Json> Document() {
    SkipSpace();
    auto value = Value(0);
    SkipSpace();
    if (!value || at_ != text_.size()) return std::nullopt;
    return value;
  }

 private:
  bool Done() const { return at_ >= text_.size(); }
  char Peek() const { return Done() ? '\0' : text_[at_]; }

  void SkipSpace() {
    while (!Done() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r')) ++at_;
  }

  bool Literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }

  std::optional<Json> Value(int depth) {
    if (depth > kMaxDepth) return std::nullopt;
    switch (Peek()) {
      case '{': return Object(depth);
      case '[': return Array(depth);
      case '"': {
        auto text = String();
        if (!text) return std::nullopt;
        return Json(std::move(*text));
      }
      case 't': return Literal("true") ? std::optional<Json>(Json(true)) : std::nullopt;
      case 'f': return Literal("false") ? std::optional<Json>(Json(false)) : std::nullopt;
      case 'n': return Literal("null") ? std::optional<Json>(Json(nullptr)) : std::nullopt;
      default: return Number();
    }
  }

  std::optional<Json> Object(int depth) {
    ++at_;
    JsonObject members;
    SkipSpace();
    if (Peek() == '}') {
      ++at_;
      return Json(std::move(members));
    }
    for (;;) {
      SkipSpace();
      if (Peek() != '"') return std::nullopt;
      auto key = String();
      if (!key) return std::nullopt;
      for (const auto& member : members) {
        if (member.first == *key) return std::nullopt;
      }
      SkipSpace();
      if (Peek() != ':') return std::nullopt;
      ++at_;
      SkipSpace();
      auto value = Value(depth + 1);
      if (!value) return std::nullopt;
      members.emplace_back(std::move(*key), std::move(*value));
      SkipSpace();
      if (Peek() == ',') {
        ++at_;
        continue;
      }
      if (Peek() != '}') return std::nullopt;
      ++at_;
      return Json(std::move(members));
    }
  }

  std::optional<Json> Array(int depth) {
    ++at_;
    JsonArray items;
    SkipSpace();
    if (Peek() == ']') {
      ++at_;
      return Json(std::move(items));
    }
    for (;;) {
      SkipSpace();
      auto value = Value(depth + 1);
      if (!value) return std::nullopt;
      items.push_back(std::move(*value));
      SkipSpace();
      if (Peek() == ',') {
        ++at_;
        continue;
      }
      if (Peek() != ']') return std::nullopt;
      ++at_;
      return Json(std::move(items));
    }
  }

  static void AppendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  std::optional<uint32_t> Hex4() {
    if (at_ + 4 > text_.size()) return std::nullopt;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else return std::nullopt;
    }
    return value;
  }

  // Copies one UTF-8 encoded character, refusing overlong forms, surrogates and values past
  // U+10FFFF.
  bool CopyUtf8(std::string& out) {
    const auto lead = static_cast<unsigned char>(text_[at_]);
    int extra = 0;
    uint32_t code = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      extra = 1;
      code = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      extra = 2;
      code = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      extra = 3;
      code = lead & 0x07;
    } else {
      return false;
    }
    if (at_ + 1 + extra > text_.size()) return false;
    for (int i = 1; i <= extra; ++i) {
      const auto next = static_cast<unsigned char>(text_[at_ + i]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3F);
    }
    if ((extra == 2 && code < 0x800) || (extra == 3 && (code < 0x10000 || code > 0x10FFFF)) ||
        (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    out.append(text_.substr(at_, 1 + extra));
    at_ += 1 + extra;
    return true;
  }

  std::optional<std::string> String() {
    ++at_;
    std::string out;
    for (;;) {
      if (Done()) return std::nullopt;
      const auto c = static_cast<unsigned char>(text_[at_]);
      if (c == '"') {
        ++at_;
        return out;
      }
      if (c < 0x20) return std::nullopt;
      if (c >= 0x80) {
        if (!CopyUtf8(out)) return std::nullopt;
        continue;
      }
      if (c != '\\') {
        out += static_cast<char>(c);
        ++at_;
        continue;
      }
      ++at_;
      if (Done()) return std::nullopt;
      const char escape = text_[at_++];
      switch (escape) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          auto code = Hex4();
          if (!code) return std::nullopt;
          if (*code >= 0xDC00 && *code <= 0xDFFF) return std::nullopt;
          if (*code >= 0xD800 && *code <= 0xDBFF) {
            if (!Literal("\\u")) return std::nullopt;
            auto low = Hex4();
            if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
            *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
          }
          AppendUtf8(out, *code);
          break;
        }
        default: return std::nullopt;
      }
    }
  }

  std::optional<Json> Number() {
    const size_t start = at_;
    if (Peek() == '-') ++at_;
    if (Peek() == '0') {
      ++at_;
    } else if (Peek() >= '1' && Peek() <= '9') {
      while (Peek() >= '0' && Peek() <= '9') ++at_;
    } else {
      return std::nullopt;
    }
    if (Peek() == '.') {
      ++at_;
      if (!(Peek() >= '0' && Peek() <= '9')) return std::nullopt;
      while (Peek() >= '0' && Peek() <= '9') ++at_;
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++at_;
      if (Peek() == '+' || Peek() == '-') ++at_;
      if (!(Peek() >= '0' && Peek() <= '9')) return std::nullopt;
      while (Peek() >= '0' && Peek() <= '9') ++at_;
    }
    const std::string digits(text_.substr(start, at_ - start));
    char* end = nullptr;
    const double value = std::strtod(digits.c_str(), &end);
    if (end != digits.c_str() + digits.size() || !std::isfinite(value)) return std::nullopt;
    return Json(value);
  }

  std::string_view text_;
  size_t at_ = 0;
};

void AppendString(std::string& out, const std::string& text) {
  out += '"';
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof escape, "\\u%04x", c);
          out += escape;
        } else {
          out += raw;
        }
    }
  }
  out += '"';
}

}  // namespace

const Json* Json::Find(std::string_view key) const {
  if (!IsObject()) return nullptr;
  for (const auto& member : AsObject()) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

std::optional<int64_t> Json::Integer(int64_t low, int64_t high) const {
  if (!IsNumber()) return std::nullopt;
  const double value = AsNumber();
  if (value != std::floor(value) || value < static_cast<double>(low) || value > static_cast<double>(high)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

void Json::SerializeTo(std::string& out) const {
  if (IsNull()) {
    out += "null";
  } else if (IsBool()) {
    out += AsBool() ? "true" : "false";
  } else if (IsNumber()) {
    const double value = AsNumber();
    char text[32];
    if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
      std::snprintf(text, sizeof text, "%lld", static_cast<long long>(value));
    } else {
      std::snprintf(text, sizeof text, "%.17g", value);
    }
    out += text;
  } else if (IsString()) {
    AppendString(out, AsString());
  } else if (IsArray()) {
    out += '[';
    bool first = true;
    for (const auto& item : AsArray()) {
      if (!first) out += ',';
      first = false;
      item.SerializeTo(out);
    }
    out += ']';
  } else {
    out += '{';
    bool first = true;
    for (const auto& member : AsObject()) {
      if (!first) out += ',';
      first = false;
      AppendString(out, member.first);
      out += ':';
      member.second.SerializeTo(out);
    }
    out += '}';
  }
}

std::string Json::Serialize() const {
  std::string out;
  SerializeTo(out);
  return out;
}

std::optional<Json> ParseJson(std::string_view text) { return Parser(text).Document(); }

}  // namespace retro_host
