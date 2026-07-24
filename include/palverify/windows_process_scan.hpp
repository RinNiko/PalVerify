#pragma once

#include "palverify/process_rules.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

struct ProcessFileEvidence {
    std::string file_description;
    std::string company_name;
    std::string signer_name;
    std::string sha256;
    bool signature_valid;
};

struct ProcessScanResult {
    bool available;
    std::vector<ProcessRuleId> rules;
};

struct ModuleEvidence {
    std::string_view image_name;
    std::string_view sha256;
    bool signature_valid;
    bool game_location;
    bool system_location;
    std::string_view signer_name;
    std::string_view file_description;
    std::string_view company_name;
};

struct ModuleScanResult {
    bool available;
    std::vector<ProcessRuleId> rules;
};

[[nodiscard]] auto inspect_process_executable(
    const std::filesystem::path& executable
) -> std::optional<ProcessFileEvidence>;

[[nodiscard]] auto scan_running_processes() -> ProcessScanResult;

[[nodiscard]] auto detect_module_rules(
    std::span<const ModuleEvidence> modules
) -> std::vector<ProcessRuleId>;

[[nodiscard]] auto looks_like_manual_map_candidate(
    std::span<const std::byte> header
) -> bool;

[[nodiscard]] auto scan_palworld_modules(
    const std::filesystem::path& game_root
) -> ModuleScanResult;

}  // namespace palverify
