#pragma once

#include "palverify/mod_policy.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

struct ClientReport {
    std::string server_id;
    std::string user_id;
    std::string protocol_version;
    std::string challenge;
    std::uint64_t sequence;
    std::string sent_at;
    std::vector<ReportedMod> mods;
    std::vector<std::string> violations;
};

struct ClientConfig {
    std::string coordinator;
    std::string server_id;
};

[[nodiscard]] auto steam_user_id_from_account_id(
    std::uint32_t account_id
) -> std::string;

[[nodiscard]] auto next_report_sequence(
    std::uint64_t previous,
    std::uint64_t wall_clock_milliseconds
) -> std::uint64_t;

[[nodiscard]] auto scan_mod_inventory(
    const std::filesystem::path& game_root
) -> std::vector<ReportedMod>;

[[nodiscard]] auto build_client_report_json(const ClientReport& report)
    -> std::string;

[[nodiscard]] auto build_challenge_request_json(
    std::string_view server_id,
    std::string_view user_id
) -> std::string;

[[nodiscard]] auto parse_client_config(std::string_view json)
    -> std::optional<ClientConfig>;

[[nodiscard]] auto parse_challenge_json(std::string_view json)
    -> std::optional<std::string>;

}  // namespace palverify
