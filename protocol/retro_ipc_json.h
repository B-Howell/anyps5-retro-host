#ifndef ANYPS5_RETRO_IPC_JSON_H_
#define ANYPS5_RETRO_IPC_JSON_H_

#include "retro_ipc_value.h"

#include <string>
#include <string_view>

namespace anyps5::retro_ipc {

// JSON text for a Value, lossless for every alternative the method codec
// carries. Integers decode as int32 when they fit and int64 otherwise, the
// rule Flutter's Windows codec applies to Dart integers. Doubles always keep
// a fraction or exponent. Byte lists and typed numeric lists, maps whose
// keys are not all plain strings, and non-finite doubles use one-member
// objects whose key starts with "$" (see the retro host protocol document).
std::string EncodeJson(const Value& value);
Value DecodeJson(std::string_view text);

std::string EncodeBase64(const uint8_t* data, size_t size);
Bytes DecodeBase64(std::string_view text);

}  // namespace anyps5::retro_ipc

#endif  // ANYPS5_RETRO_IPC_JSON_H_
