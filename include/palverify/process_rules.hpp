#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace palverify {

enum class ProcessRuleId {
    CheatEngineRunning,
    WeModRunning,
    InjectedModuleDetected,
    ManualMapDetected,
};

struct ProcessEvidence {
    std::string_view image_name;
    std::string_view file_description;
    std::string_view company_name;
    std::string_view signer_name;
    std::string_view sha256;
    bool signature_valid{false};
};

[[nodiscard]] auto detect_process_rules(
    std::span<const std::string_view> process_images
) -> std::vector<ProcessRuleId>;

[[nodiscard]] auto detect_process_rules(
    std::span<const ProcessEvidence> processes
) -> std::vector<ProcessRuleId>;

[[nodiscard]] auto to_string(ProcessRuleId rule) -> std::string_view;

}  // namespace palverify
