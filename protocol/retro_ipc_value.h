#ifndef ANYPS5_RETRO_IPC_VALUE_H_
#define ANYPS5_RETRO_IPC_VALUE_H_

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace anyps5::retro_ipc {

class Value;
using List = std::vector<Value>;
using Map = std::map<Value, Value>;
using Bytes = std::vector<uint8_t>;

namespace internal {
using ValueVariant = std::variant<std::monostate, bool, int32_t, int64_t, double, std::string, std::vector<uint8_t>,
                                  std::vector<int32_t>, std::vector<int64_t>, std::vector<double>, List, Map,
                                  std::vector<float>>;
}

// The value model of Flutter's standard method codec. It has the same
// alternatives and constructors as flutter::EncodableValue, so a method
// handler written against that type keeps its meaning on either side of
// the process boundary.
class Value : public internal::ValueVariant {
 public:
  using super = internal::ValueVariant;
  using super::super;
  using super::operator=;

  Value() = default;
  Value(const char* string) : super(std::string(string)) {}
  Value& operator=(const char* other) {
    *this = std::string(other);
    return *this;
  }

  bool IsNull() const { return std::holds_alternative<std::monostate>(*this); }

  int64_t LongValue() const {
    if (const auto* narrow = std::get_if<int32_t>(this)) return *narrow;
    if (const auto* wide = std::get_if<int64_t>(this)) return *wide;
    throw std::bad_variant_access();
  }

  friend bool operator<(const Value& lhs, const Value& rhs) {
    return static_cast<const super&>(lhs) < static_cast<const super&>(rhs);
  }
  friend bool operator==(const Value& lhs, const Value& rhs) {
    return static_cast<const super&>(lhs) == static_cast<const super&>(rhs);
  }
  friend bool operator!=(const Value& lhs, const Value& rhs) { return !(lhs == rhs); }
};

}  // namespace anyps5::retro_ipc

#endif  // ANYPS5_RETRO_IPC_VALUE_H_
