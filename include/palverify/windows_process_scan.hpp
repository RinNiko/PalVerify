#pragma once

#include "palverify/process_rules.hpp"

#include <vector>

namespace palverify {

struct ProcessScanResult {
    bool available;
    std::vector<ProcessRuleId> rules;
};

[[nodiscard]] auto scan_running_processes() -> ProcessScanResult;

}  // namespace palverify
