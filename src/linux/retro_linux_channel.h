#ifndef ANYPS5_RETRO_LINUX_CHANNEL_H_
#define ANYPS5_RETRO_LINUX_CHANNEL_H_

#include "retro_ipc_value.h"

#include <cstdint>
#include <algorithm>
#include <optional>
#include <stdexcept>

#include "retro_session.h"

namespace anyps5::retro::linux_channel {

using retro_ipc::Value;

inline const Value* Field(const Value* map, const char* key) {
  const auto* fields = map ? std::get_if<retro_ipc::Map>(map) : nullptr;
  if (!fields) return nullptr;
  const auto found = fields->find(Value(key));
  return found == fields->end() ? nullptr : &found->second;
}

inline bool IsInt(const Value* value) {
  return value && (std::holds_alternative<int32_t>(*value) || std::holds_alternative<int64_t>(*value));
}
inline int64_t Int(const Value* value) { return value->LongValue(); }
inline bool IsBool(const Value* value) { return value && std::holds_alternative<bool>(*value); }
inline bool IsNull(const Value* value) { return value && value->IsNull(); }
inline const std::string* String(const Value* value) { return value ? std::get_if<std::string>(value) : nullptr; }
inline const retro_ipc::List* List(const Value* value) { return value ? std::get_if<retro_ipc::List>(value) : nullptr; }
inline const retro_ipc::Map* Map(const Value* value) { return value ? std::get_if<retro_ipc::Map>(value) : nullptr; }
inline const retro_ipc::Bytes* Bytes(const Value* value) { return value ? std::get_if<retro_ipc::Bytes>(value) : nullptr; }

// A null payload is valid; an absent optional means the command is stale.
inline std::optional<const Value*> OwnedArguments(const Value* args, int64_t active_session) {
  const auto* owner = Field(args, "owner");
  if (!IsInt(owner) || active_session <= 0 || Int(owner) != active_session) return std::nullopt;
  return Field(args, "value");
}

inline std::optional<int64_t> ReadCheckpointSource(const Value* args, int64_t texture_id) {
  const auto* texture = Field(args, "texture");
  const auto* source = Field(args, "source");
  if (!IsInt(texture) || Int(texture) != texture_id || !IsInt(source) || Int(source) <= 0) return std::nullopt;
  return Int(source);
}

inline ps1_checkpoint::Digest ReadCheckpointDigest(const Value* args) {
  const auto* digest = Bytes(Field(args, "discHash"));
  if (!digest || digest->size() != 32) throw std::runtime_error("Expected a 32-byte verified disc digest");
  ps1_checkpoint::Digest result;
  std::copy_n(digest->begin(), result.size(), result.begin());
  return result;
}

inline uint32_t ReadMatchFrame(const Value* frame) {
  if (!IsInt(frame) || Int(frame) < 0 || Int(frame) > ps1_netplay::Timeline::kMaxFrame)
    throw std::runtime_error("Invalid PS1 match frame.");
  return static_cast<uint32_t>(Int(frame));
}

inline ps1_checkpoint::Snapshot ReadMatchCheckpoint(const Value* args) {
  const auto frame = ReadMatchFrame(Field(args, "frame"));
  const auto* state = Bytes(Field(args, "state"));
  if (!state || state->empty() || state->size() > ps1_checkpoint::kMaximumBytes)
    throw std::runtime_error("Invalid portable checkpoint size.");
  return {frame, *state};
}

inline std::optional<Ps1MatchConfig> ReadPs1Match(const Value* args) {
  for (const auto* flag : {"ps1NetplayCpu", "ps1PresentationCore"}) {
    if (Field(args, flag)) throw std::runtime_error("This online worker is not yet available on Linux");
  }
  const auto* seat = Field(args, "ps1NetplaySeat");
  const auto* bios = Field(args, "ps1NetplayBios");
  const auto* dualshock = Field(args, "ps1NetplayDualShock");
  const auto* staging = Field(args, "ps1LocalStaging");
  const auto* rollback = Field(args, "ps1NetplayRollback");
  if (!seat && !bios && !dualshock && !staging && !rollback) return std::nullopt;
  if (!IsInt(seat) || Int(seat) < 0 || Int(seat) > 1 || !String(bios) || (dualshock && !IsBool(dualshock)) ||
      (staging && !IsBool(staging)) || (rollback && !IsBool(rollback)))
    throw std::runtime_error("Invalid synchronized PS1 seat or BIOS.");
  return Ps1MatchConfig{static_cast<unsigned>(Int(seat)), *String(bios),
                       dualshock && std::get<bool>(*dualshock), staging && std::get<bool>(*staging),
                       rollback && std::get<bool>(*rollback)};
}

inline std::optional<Ps3NetworkPolicy> ReadPs3Online(const Value* args, Console console) {
  const auto* policy = Field(args, "ps3Online");
  if (!policy) return std::nullopt;
  if (console != Console::kPs3 || !Map(policy) || Map(policy)->size() != 5)
    throw std::runtime_error("Invalid PS3 online profile.");
  const auto read = [](const Value* map, const char* key) -> std::string {
    const auto* value = String(Field(map, key));
    if (!value || value->empty()) throw std::runtime_error("Invalid PS3 online profile field.");
    return *value;
  };
  const auto* identity = Field(policy, "rpcnIdentity");
  const auto* protection = Field(policy, "patchProtection");
  const auto* upnp = Field(policy, "upnp");
  const auto* patches = List(Field(protection, "patches"));
  if (!Map(identity) || Map(identity)->size() != 2 || !Map(protection) || Map(protection)->size() != 5 ||
      !IsBool(upnp) || !patches || patches->empty() || patches->size() > 8)
    throw std::runtime_error("Invalid PS3 online protection or provider identity.");

  Ps3NetworkPolicy result{read(policy, "xfloat"), read(policy, "dns"), std::get<bool>(*upnp),
                          read(identity, "host"), read(identity, "certificateSha256"),
                          {read(protection, "titleId"), read(protection, "updateVersion"),
                           read(protection, "sha256"), read(protection, "ppuHash"), {}, {}}};
  for (const auto& entry : *patches) {
    const auto* patch = &entry;
    const auto* writes = Field(patch, "writes");
    if (!Map(patch) || Map(patch)->size() != 3 || !IsInt(writes) || Int(writes) < 1 || Int(writes) > 4096)
      throw std::runtime_error("Invalid PS3 online patch entry.");
    read(patch, "option");
    result.protection.names.push_back(read(patch, "name"));
    result.protection.writes.push_back(static_cast<unsigned>(Int(writes)));
  }
  return result;
}

struct MatchExchange {
  std::vector<ps1_netplay::FrameInput> remote;
  std::optional<uint32_t> acknowledged;
};

inline ps1_netplay::PadInput ReadMatchPad(const Value* sample, size_t offset = 0) {
  const auto* values = List(sample);
  if (!values || values->size() != offset + 5) throw std::runtime_error("Invalid synchronized PS1 pad.");
  const auto integer = [&](size_t index, int64_t min, int64_t max) {
    const auto* value = &(*values)[index + offset];
    if (!IsInt(value)) throw std::runtime_error("Invalid synchronized PS1 input.");
    const auto number = Int(value);
    if (number < min || number > max) throw std::runtime_error("Invalid synchronized PS1 input.");
    return number;
  };
  ps1_netplay::PadInput pad{};
  pad.buttons = static_cast<uint16_t>(integer(0, 0, UINT16_MAX));
  for (size_t axis = 0; axis < pad.axes.size(); ++axis)
    pad.axes[axis] = static_cast<int16_t>(integer(axis + 1, INT16_MIN, INT16_MAX));
  return pad;
}

inline MatchExchange ReadMatchExchange(const Value* args) {
  MatchExchange exchange;
  const auto* ack = Field(args, "acknowledgedFrame");
  if (ack && !IsNull(ack)) exchange.acknowledged = ReadMatchFrame(ack);
  const auto* frames = List(Field(args, "remote"));
  if (!frames || frames->size() > 32) throw std::runtime_error("Invalid synchronized PS1 input batch.");
  for (const auto& sample : *frames) {
    const auto* row = List(&sample);
    if (!row || row->size() != 6) throw std::runtime_error("Invalid synchronized PS1 frame.");
    exchange.remote.push_back({ReadMatchFrame(&row->front()), ReadMatchPad(&sample, 1)});
  }
  return exchange;
}

inline std::vector<Ps1Ports> ReadMatchReplay(const Value* args) {
  const auto* frames = List(Field(args, "frames"));
  if (!frames || frames->empty() || frames->size() > kPs1CatchupBatch)
    throw std::runtime_error("Invalid PS1 catch-up batch.");
  std::vector<Ps1Ports> result;
  for (const auto& entry : *frames) {
    const auto* ports = List(&entry);
    if (!ports || ports->size() != 2) throw std::runtime_error("Invalid PS1 catch-up controller ports.");
    result.push_back({ReadMatchPad(&(*ports)[0]), ReadMatchPad(&(*ports)[1])});
  }
  return result;
}

inline std::optional<LanFrames> ReadLanFrames(const Value* args, int64_t texture_id) {
  const auto* texture = Field(args, "texture");
  if (!IsInt(texture) || Int(texture) != texture_id) return std::nullopt;
  const auto* frames = List(Field(args, "frames"));
  if (!frames || frames->size() > 16) throw std::runtime_error("Expected a bounded PS2 LAN frame batch");
  LanFrames input;
  for (const auto& entry : *frames) {
    const auto* frame = Bytes(&entry);
    if (!frame || frame->size() < 14 || frame->size() > 1514) throw std::runtime_error("Invalid PS2 Ethernet frame");
    input.push_back(*frame);
  }
  return input;
}

}  // namespace anyps5::retro::linux_channel

#endif  // ANYPS5_RETRO_LINUX_CHANNEL_H_
