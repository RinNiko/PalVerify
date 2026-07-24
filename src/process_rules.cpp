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
        std::string_view{"cheatengine-x86_64-SSE4-AVX2.exe"},
    };

    for (const auto known_name : known_names) {
        if (ascii_equals_ignore_case(image_name, known_name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto is_cheat_engine(const ProcessEvidence& process) -> bool
{
    return is_cheat_engine(process.image_name)
        || (
            ascii_equals_ignore_case(
                process.file_description,
                "Cheat Engine"
            )
            && ascii_equals_ignore_case(
                process.company_name,
                "Cheat Engine"
            )
        )
        || (
            process.signature_valid
            && ascii_equals_ignore_case(
                process.signer_name,
                "Cheat Engine EZ"
            )
        )
        || ascii_equals_ignore_case(
            process.sha256,
            "9d861d651ab9d1dc3c09ae34c8ed5dee"
            "3d1a29b080784c3c48773494c9350230"
        );
}

[[nodiscard]] auto is_wemod(const ProcessEvidence& process) -> bool
{
    return ascii_equals_ignore_case(process.image_name, "wemod.exe")
        || ascii_equals_ignore_case(process.image_name, "wand.exe")
        || ascii_equals_ignore_case(
            process.image_name,
            "wandauxiliaryservice.exe"
        )
        || (
            ascii_equals_ignore_case(
                process.file_description,
                "WeMod - Cheats and Mods"
            )
            && ascii_equals_ignore_case(process.company_name, "WeMod")
        )
        || (
            ascii_equals_ignore_case(process.file_description, "Wand")
            && (
                ascii_equals_ignore_case(process.company_name, "WeMod")
                || ascii_equals_ignore_case(
                    process.company_name,
                    "WeMod LLC"
                )
            )
        )
        || (
            process.signature_valid
            && ascii_equals_ignore_case(process.signer_name, "WeMod LLC")
        );
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
            || ascii_equals_ignore_case(image_name, "wemod.exe")
            || ascii_equals_ignore_case(image_name, "wand.exe")
            || ascii_equals_ignore_case(
                image_name,
                "wandauxiliaryservice.exe"
            );
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

auto detect_process_rules(std::span<const ProcessEvidence> processes)
    -> std::vector<ProcessRuleId>
{
    bool cheat_engine_running = false;
    bool wemod_running = false;

    for (const auto& process : processes) {
        cheat_engine_running =
            cheat_engine_running || is_cheat_engine(process);
        wemod_running = wemod_running || is_wemod(process);
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
    case ProcessRuleId::InjectedModuleDetected:
        return "INJECTED_MODULE_DETECTED";
    case ProcessRuleId::ManualMapDetected:
        return "MANUAL_MAP_DETECTED";
    }

    return "PROCESS_UNKNOWN";
}

}  // namespace palverify
