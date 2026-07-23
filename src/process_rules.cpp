#include "palverify/process_rules.hpp"

#include <array>

namespace palverify {
namespace {

[[nodiscard]] constexpr auto ascii_lower(char value) -> char
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] constexpr auto ascii_equals_ignore_case(
    std::string_view left,
    std::string_view right
) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto is_cheat_engine(std::string_view image_name) -> bool
{
    constexpr std::array known_names{
        std::string_view{"cheatengine.exe"},
        std::string_view{"cheatengine-i386.exe"},
        std::string_view{"cheatengine-x86_64.exe"},
    };

    for (const auto known_name : known_names) {
        if (ascii_equals_ignore_case(image_name, known_name)) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto detect_process_rules(std::span<const std::string_view> process_images)
    -> std::vector<ProcessRuleId>
{
    bool cheat_engine_running = false;
    bool wemod_running = false;

    for (const auto image_name : process_images) {
        cheat_engine_running =
            cheat_engine_running || is_cheat_engine(image_name);
        wemod_running =
            wemod_running
            || ascii_equals_ignore_case(image_name, "wemod.exe");
    }

    std::vector<ProcessRuleId> rules;
    if (cheat_engine_running) {
        rules.push_back(ProcessRuleId::CheatEngineRunning);
    }
    if (wemod_running) {
        rules.push_back(ProcessRuleId::WeModRunning);
    }
    return rules;
}

auto to_string(ProcessRuleId rule) -> std::string_view
{
    switch (rule) {
    case ProcessRuleId::CheatEngineRunning:
        return "PROCESS_CHEAT_ENGINE_RUNNING";
    case ProcessRuleId::WeModRunning:
        return "PROCESS_WEMOD_RUNNING";
    }

    return "PROCESS_UNKNOWN";
}

}  // namespace palverify
