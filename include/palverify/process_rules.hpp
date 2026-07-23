#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace palverify {

enum class ProcessRuleId {
    CheatEngineRunning,
    WeModRunning,
};

[[nodiscard]] auto detect_process_rules(
    std::span<const std::string_view> process_images
) -> std::vector<ProcessRuleId>;

[[nodiscard]] auto to_string(ProcessRuleId rule) -> std::string_view;

}  // namespace palverify
