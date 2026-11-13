// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_JSON_H_
#define ANYPS5_RETRO_HOST_JSON_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace retro_host {

class Json;
using JsonArray = std::vector<Json>;
using JsonObject = std::vector<std::pair<std::string, Json>>;

class Json {
 public:
  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool value) : value_(value) {}
  Json(int value) : value_(static_cast<double>(value)) {}
  Json(int64_t value) : value_(static_cast<double>(value)) {}
  Json(uint32_t value) : value_(static_cast<double>(value)) {}
  Json(uint64_t value) : value_(static_cast<double>(value)) {}
  Json(double value) : value_(value) {}
  Json(const char* value) : value_(std::string(value)) {}
  Json(std::string value) : value_(std::move(value)) {}
  Json(JsonArray value) : value_(std::move(value)) {}
  Json(JsonObject value) : value_(std::move(value)) {}

  bool IsNull() const { return std::holds_alternative<std::monostate>(value_); }
  bool IsBool() const { return std::holds_alternative<bool>(value_); }
  bool IsNumber() const { return std::holds_alternative<double>(value_); }
  bool IsString() const { return std::holds_alternative<std::string>(value_); }
  bool IsArray() const { return std::holds_alternative<JsonArray>(value_); }
  bool IsObject() const { return std::holds_alternative<JsonObject>(value_); }

  bool AsBool() const { return std::get<bool>(value_); }
  double AsNumber() const { return std::get<double>(value_); }
  const std::string& AsString() const { return std::get<std::string>(value_); }
  const JsonArray& AsArray() const { return std::get<JsonArray>(value_); }
  const JsonObject& AsObject() const { return std::get<JsonObject>(value_); }

  // The member named key, or nullptr when this is not an object or has no such member.
  const Json* Find(std::string_view key) const;
  // An integer from low to high, decided by value (1.0 and 1e0 are 1).
  std::optional<int64_t> Integer(int64_t low, int64_t high) const;

  std::string Serialize() const;

 private:
  void SerializeTo(std::string& out) const;
  std::variant<std::monostate, bool, double, std::string, JsonArray, JsonObject> value_;
};

// Parses one JSON value as the protocol reads it: RFC 8259, a duplicate key in any object, a
// lone surrogate escape, invalid UTF-8 or a number that is not finite as a double are errors.
std::optional<Json> ParseJson(std::string_view text);

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_JSON_H_
