#include "palverify/mod_policy.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <unordered_set>

namespace palverify {
namespace {

[[nodiscard]] auto ascii_lower(std::string_view value) -> std::string
{
    std::string lowered{value};
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char item) {
        if (item >= 'A' && item <= 'Z') {
            return static_cast<char>(item - 'A' + 'a');
        }
        return static_cast<char>(item);
    });
    return lowered;
}

}  // namespace

auto evaluate_mod_whitelist(
    const std::vector<ReportedMod>& reported,
    const std::vector<AllowedMod>& allowed
) -> std::vector<std::string>
{
    std::vector<std::string> rejected;
    std::unordered_set<std::string> seen;

    for (const auto& mod : reported) {
        const auto normalized_id = ascii_lower(mod.id);
        const auto duplicate = !seen.insert(normalized_id).second;
        const auto approved = std::ranges::find_if(
            allowed,
            [&](const AllowedMod& entry) {
                return ascii_lower(entry.id) == normalized_id;
            }
        );
        if (duplicate || approved == allowed.end()
            || approved->version != mod.version
            || ascii_lower(approved->digest) != ascii_lower(mod.digest)) {
            rejected.push_back(mod.id);
        }
    }

    for (const auto& entry : allowed) {
        if (!seen.contains(ascii_lower(entry.id))) {
            rejected.push_back(entry.id);
        }
    }

    std::ranges::sort(rejected);
    rejected.erase(
        std::ranges::unique(rejected).begin(),
        rejected.end()
    );
    return rejected;
}

}  // namespace palverify
