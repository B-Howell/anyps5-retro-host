#include "retro_ipc_json.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace anyps5::retro_ipc {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr size_t kMaxDepth = 64;

void AppendString(std::string& out, const std::string& text) {
  out += '"';
  for (const unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04x", c);
          out += escape;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void AppendDouble(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += std::isnan(value) ? "{\"$f\":\"nan\"}" : value > 0 ? "{\"$f\":\"inf\"}" : "{\"$f\":\"-inf\"}";
    return;
  }
  char text[40];
  std::snprintf(text, sizeof(text), "%.17g", value);
  out += text;
  if (!std::strpbrk(text, ".eE")) out += ".0";
}

template <typename Number>
void AppendNumbers(std::string& out, const char* tag, const std::vector<Number>& values) {
  out += "{\"";
  out += tag;
  out += "\":[";
  for (size_t index = 0; index < values.size(); ++index) {
    if (index) out += ',';
    if constexpr (std::is_floating_point_v<Number>) {
      AppendDouble(out, static_cast<double>(values[index]));
    } else {
      out += std::to_string(values[index]);
    }
  }
  out += "]}";
}

bool PlainKeys(const Map& map) {
  for (const auto& entry : map) {
    const auto* key = std::get_if<std::string>(&entry.first);
    if (!key || (!key->empty() && key->front() == '$')) return false;
  }
  return true;
}

void Append(std::string& out, const Value& value) {
  if (value.IsNull()) {
    out += "null";
  } else if (const auto* flag = std::get_if<bool>(&value)) {
    out += *flag ? "true" : "false";
  } else if (const auto* narrow = std::get_if<int32_t>(&value)) {
    out += std::to_string(*narrow);
  } else if (const auto* wide = std::get_if<int64_t>(&value)) {
    out += std::to_string(*wide);
  } else if (const auto* number = std::get_if<double>(&value)) {
    AppendDouble(out, *number);
  } else if (const auto* text = std::get_if<std::string>(&value)) {
    AppendString(out, *text);
  } else if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
    out += "{\"$u8\":\"";
    out += EncodeBase64(bytes->data(), bytes->size());
    out += "\"}";
  } else if (const auto* ints = std::get_if<std::vector<int32_t>>(&value)) {
    AppendNumbers(out, "$i32", *ints);
  } else if (const auto* longs = std::get_if<std::vector<int64_t>>(&value)) {
    AppendNumbers(out, "$i64", *longs);
  } else if (const auto* doubles = std::get_if<std::vector<double>>(&value)) {
    AppendNumbers(out, "$f64", *doubles);
  } else if (const auto* floats = std::get_if<std::vector<float>>(&value)) {
    AppendNumbers(out, "$f32", *floats);
  } else if (const auto* list = std::get_if<List>(&value)) {
    out += '[';
    for (size_t index = 0; index < list->size(); ++index) {
      if (index) out += ',';
      Append(out, (*list)[index]);
    }
    out += ']';
  } else if (const auto* map = std::get_if<Map>(&value)) {
    if (PlainKeys(*map)) {
      out += '{';
      bool first = true;
      for (const auto& [key, item] : *map) {
        if (!first) out += ',';
        first = false;
        AppendString(out, std::get<std::string>(key));
        out += ':';
        Append(out, item);
      }
      out += '}';
    } else {
      out += "{\"$map\":[";
      bool first = true;
      for (const auto& [key, item] : *map) {
        if (!first) out += ',';
        first = false;
        out += '[';
        Append(out, key);
        out += ',';
        Append(out, item);
        out += ']';
      }
      out += "]}";
    }
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Value Document() {
    auto value = Parse(0);
    Skip();
    if (position_ != text_.size()) Fail("trailing characters");
    return value;
  }

 private:
  [[noreturn]] void Fail(const char* reason) const {
    throw std::runtime_error(std::string("Invalid retro host message: ") + reason);
  }

  void Skip() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\n' || text_[position_] == '\r' ||
            text_[position_] == '\t'))
      ++position_;
  }

  char Peek() {
    Skip();
    if (position_ >= text_.size()) Fail("unexpected end");
    return text_[position_];
  }

  void Expect(char c) {
    if (Peek() != c) Fail("unexpected character");
    ++position_;
  }

  bool Literal(std::string_view word) {
    if (text_.substr(position_, word.size()) != word) return false;
    position_ += word.size();
    return true;
  }

  Value Parse(size_t depth) {
    if (depth > kMaxDepth) Fail("nesting too deep");
    const char c = Peek();
    if (c == '{') return Object(depth);
    if (c == '[') return Value(Array(depth));
    if (c == '"') return Value(String());
    if (Literal("null")) return Value();
    if (Literal("true")) return Value(true);
    if (Literal("false")) return Value(false);
    return Number();
  }

  List Array(size_t depth) {
    Expect('[');
    List list;
    if (Peek() == ']') {
      ++position_;
      return list;
    }
    while (true) {
      list.push_back(Parse(depth + 1));
      if (Peek() == ',') {
        ++position_;
        continue;
      }
      Expect(']');
      return list;
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

  uint32_t Hex4() {
    if (position_ + 4 > text_.size()) Fail("short escape");
    uint32_t code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_++];
      code <<= 4;
      if (c >= '0' && c <= '9') code |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') code |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= static_cast<uint32_t>(c - 'A' + 10);
      else Fail("bad escape");
    }
    return code;
  }

  std::string String() {
    Expect('"');
    std::string out;
    while (true) {
      if (position_ >= text_.size()) Fail("unterminated string");
      const char c = text_[position_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (position_ >= text_.size()) Fail("unterminated escape");
      switch (text_[position_++]) {
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
          if (code >= 0xD800 && code < 0xDC00 && Literal("\\u")) {
            const auto low = Hex4();
            if (low < 0xDC00 || low >= 0xE000) Fail("bad surrogate");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(out, code);
          break;
        }
        default: Fail("bad escape");
      }
    }
  }

  Value Number() {
    const auto start = position_;
    bool fraction = false;
    if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) ++position_;
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c >= '0' && c <= '9') {
        ++position_;
      } else if (c == '.' || c == 'e' || c == 'E' || ((c == '-' || c == '+') && fraction)) {
        fraction = true;
        ++position_;
      } else {
        break;
      }
    }
    if (position_ == start) Fail("unexpected character");
    const std::string token(text_.substr(start, position_ - start));
    char* end = nullptr;
    if (fraction) {
      const double value = std::strtod(token.c_str(), &end);
      if (*end) Fail("bad number");
      return Value(value);
    }
    errno = 0;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (*end || errno == ERANGE) Fail("bad integer");
    if (value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max())
      return Value(static_cast<int32_t>(value));
    return Value(static_cast<int64_t>(value));
  }

  template <typename Number>
  std::vector<Number> Numbers(const Value& value) {
    const auto* list = std::get_if<List>(&value);
    if (!list) Fail("typed list expected");
    std::vector<Number> out;
    out.reserve(list->size());
    for (const auto& item : *list) {
      if constexpr (std::is_floating_point_v<Number>) {
        if (const auto* number = std::get_if<double>(&item)) out.push_back(static_cast<Number>(*number));
        else if (std::holds_alternative<int32_t>(item) || std::holds_alternative<int64_t>(item))
          out.push_back(static_cast<Number>(item.LongValue()));
        else Fail("number expected");
      } else {
        if (!std::holds_alternative<int32_t>(item) && !std::holds_alternative<int64_t>(item)) Fail("integer expected");
        const auto number = item.LongValue();
        if (number < std::numeric_limits<Number>::min() || number > std::numeric_limits<Number>::max())
          Fail("integer out of range");
        out.push_back(static_cast<Number>(number));
      }
    }
    return out;
  }

  Value Tagged(const std::string& tag, const Value& value) {
    if (tag == "$u8") {
      const auto* text = std::get_if<std::string>(&value);
      if (!text) Fail("base64 expected");
      return Value(DecodeBase64(*text));
    }
    if (tag == "$i32") return Value(Numbers<int32_t>(value));
    if (tag == "$i64") return Value(Numbers<int64_t>(value));
    if (tag == "$f64") return Value(Numbers<double>(value));
    if (tag == "$f32") return Value(Numbers<float>(value));
    if (tag == "$f") {
      const auto* text = std::get_if<std::string>(&value);
      if (!text) Fail("special double expected");
      if (*text == "nan") return Value(std::numeric_limits<double>::quiet_NaN());
      if (*text == "inf") return Value(std::numeric_limits<double>::infinity());
      if (*text == "-inf") return Value(-std::numeric_limits<double>::infinity());
      Fail("unknown special double");
    }
    if (tag == "$map") {
      const auto* pairs = std::get_if<List>(&value);
      if (!pairs) Fail("map pairs expected");
      Map map;
      for (const auto& pair : *pairs) {
        const auto* entry = std::get_if<List>(&pair);
        if (!entry || entry->size() != 2) Fail("map pair expected");
        map[(*entry)[0]] = (*entry)[1];
      }
      return Value(std::move(map));
    }
    Fail("unknown tag");
  }

  Value Object(size_t depth) {
    Expect('{');
    Map map;
    std::string first_key;
    size_t count = 0;
    if (Peek() == '}') {
      ++position_;
      return Value(std::move(map));
    }
    while (true) {
      if (Peek() != '"') Fail("key expected");
      auto key = String();
      Expect(':');
      auto value = Parse(depth + 1);
      if (count == 0) first_key = key;
      ++count;
      map[Value(std::move(key))] = std::move(value);
      if (Peek() == ',') {
        ++position_;
        continue;
      }
      Expect('}');
      break;
    }
    if (count == 1 && !first_key.empty() && first_key.front() == '$')
      return Tagged(first_key, map.begin()->second);
    for (const auto& entry : map) {
      const auto& key = std::get<std::string>(entry.first);
      if (!key.empty() && key.front() == '$') Fail("reserved key in a plain map");
    }
    return Value(std::move(map));
  }

  std::string_view text_;
  size_t position_ = 0;
};

}  // namespace

std::string EncodeJson(const Value& value) {
  std::string out;
  Append(out, value);
  return out;
}

Value DecodeJson(std::string_view text) { return Parser(text).Document(); }

std::string EncodeBase64(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t index = 0;
  for (; index + 2 < size; index += 3) {
    const uint32_t block = (data[index] << 16) | (data[index + 1] << 8) | data[index + 2];
    out += kAlphabet[(block >> 18) & 63];
    out += kAlphabet[(block >> 12) & 63];
    out += kAlphabet[(block >> 6) & 63];
    out += kAlphabet[block & 63];
  }
  if (index < size) {
    uint32_t block = data[index] << 16;
    if (index + 1 < size) block |= data[index + 1] << 8;
    out += kAlphabet[(block >> 18) & 63];
    out += kAlphabet[(block >> 12) & 63];
    out += index + 1 < size ? kAlphabet[(block >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

Bytes DecodeBase64(std::string_view text) {
  static const auto table = [] {
    std::array<int8_t, 256> values{};
    values.fill(-1);
    for (int i = 0; i < 64; ++i) values[static_cast<uint8_t>(kAlphabet[i])] = static_cast<int8_t>(i);
    return values;
  }();
  if (text.size() % 4) throw std::runtime_error("Invalid retro host message: base64 length");
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  for (size_t index = 0; index < text.size(); index += 4) {
    uint32_t block = 0;
    int padding = 0;
    for (size_t offset = 0; offset < 4; ++offset) {
      const auto c = static_cast<uint8_t>(text[index + offset]);
      block <<= 6;
      if (c == '=' && index + 4 == text.size() && offset >= 2) {
        ++padding;
        continue;
      }
      if (padding || table[c] < 0) throw std::runtime_error("Invalid retro host message: base64 data");
      block |= static_cast<uint32_t>(table[c]);
    }
    out.push_back(static_cast<uint8_t>(block >> 16));
    if (padding < 2) out.push_back(static_cast<uint8_t>(block >> 8));
    if (padding < 1) out.push_back(static_cast<uint8_t>(block));
  }
  return out;
}

}  // namespace anyps5::retro_ipc
