#include "rpcs3/libretro/anyps5_patch_attestation.h"
#include <cassert>

int main()
{
    using namespace anyps5_ps3;
    constexpr const char* name = "Patchwork Combined Patch";
    constexpr const char* names[] = {name};
    constexpr unsigned writes[] = {26};
    constexpr auto digest = "b7eb535e5e5c13a7fba292ff62ff2fb8cc4fa7a58de5573c6487a0059b834476";
    constexpr auto ppu = "PPU-545c1abbf1c562d60fca7435401f020beab76b53";
    const auto reviewed = make_patch_requirement("BCUS98208", "01.30", digest, ppu, 1, names, writes);
    assert(reviewed);
    assert(reviewed->matches_executable("BCUS98208", "01.30", ppu));
    assert(!reviewed->matches_executable("BCUS98208", "01.00", ppu));
    assert(!reviewed->matches_executable("BCUS98148", "01.30", ppu));
    assert(reviewed->writes == (patch_writes{{name, 26}}));
    assert(reviewed->writes != (patch_writes{{name, 25}}));
    assert(reviewed->writes != (patch_writes{{name, 26}, {"other", 1}}));
    constexpr const char* duplicate[] = {name, name};
    constexpr unsigned duplicate_writes[] = {26, 26};
    assert(!make_patch_requirement("BCUS98208", "01.30", digest, ppu, 2, duplicate, duplicate_writes));
    assert(!make_patch_requirement("../escape", "01.30", digest, ppu, 1, names, writes));
    assert(!make_patch_requirement("BCUS98208", "01.30", "truncated", ppu, 1, names, writes));
    const auto identity = make_rpcn_identity("127.0.0.1:3657", digest);
    assert(identity && identity->host == "127.0.0.1:3657" && identity->certificate_sha256 == digest);
    assert(!make_rpcn_identity("https://fixture.invalid", digest));
    assert(!make_rpcn_identity("fixture.invalid", "truncated"));
}
