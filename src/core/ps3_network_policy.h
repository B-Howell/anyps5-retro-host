#ifndef ANYPS5_RETRO_PS3_NETWORK_POLICY_H_
#define ANYPS5_RETRO_PS3_NETWORK_POLICY_H_

#include <stdexcept>
#include <string>
#include <vector>

namespace anyps5::retro {

struct Ps3NetworkPolicy {
  struct Protection {
    std::string title;
    std::string version;
    std::string sha256;
    std::string ppu_hash;
    std::vector<std::string> names;
    std::vector<unsigned> writes;
  };

  std::string xfloat;
  std::string dns;
  bool upnp;
  std::string host;
  std::string certificate_sha256;
  Protection protection;
};

using Ps3OfflinePolicy = bool (*)(unsigned, const char*, const char*, bool);
using Ps3VerifiedOnlinePolicy = bool (*)(const char*, const char*, bool, const char*, const char*,
                                         const char*, const char*, const char*, const char*, unsigned,
                                         const char* const*, const unsigned*);

inline constexpr char kPs3OfflinePolicyExport[] = "retro_anyps5_ps3_network_policy_v1";
inline constexpr char kPs3VerifiedOnlinePolicyExport[] = "retro_anyps5_ps3_network_policy_v3";

inline void ApplyPs3OfflinePolicy(Ps3OfflinePolicy select, bool require_export) {
  if (require_export && !select)
    throw std::runtime_error("This RPCS3 core does not support isolated PS3 profiles.");
  if (select && !select(0, nullptr, nullptr, false))
    throw std::runtime_error("RPCS3 rejected the network policy.");
}

inline void ApplyPs3OnlinePolicy(Ps3VerifiedOnlinePolicy select, const Ps3NetworkPolicy& online) {
  if (!select)
    throw std::runtime_error("This RPCS3 core cannot verify the PS3 online provider.");

  const auto& patch = online.protection;
  if (patch.names.empty() || patch.names.size() > 8 || patch.names.size() != patch.writes.size())
    throw std::runtime_error("Invalid PS3 online protection.");

  std::vector<const char*> names;
  names.reserve(patch.names.size());
  for (const auto& name : patch.names) names.push_back(name.c_str());
  if (!select(online.xfloat.c_str(), online.dns.c_str(), online.upnp,
              online.host.c_str(), online.certificate_sha256.c_str(),
              patch.title.c_str(), patch.version.c_str(), patch.sha256.c_str(), patch.ppu_hash.c_str(),
              static_cast<unsigned>(names.size()), names.data(), patch.writes.data()))
    throw std::runtime_error("RPCS3 rejected the required online protection.");
}

}  // namespace anyps5::retro

#endif  // ANYPS5_RETRO_PS3_NETWORK_POLICY_H_
