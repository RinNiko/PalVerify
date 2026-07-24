#pragma once

#include <string>
#include <vector>

namespace palverify {

struct AllowedMod {
    std::string id;
    std::string version;
    std::string digest;
};

struct ReportedMod {
    std::string id;
    std::string version;
    std::string digest;
};

[[nodiscard]] auto evaluate_mod_whitelist(
    const std::vector<ReportedMod>& reported,
    const std::vector<AllowedMod>& allowed
) -> std::vector<std::string>;

}  // namespace palverify
