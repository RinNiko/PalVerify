#pragma once

#include "palverify/mod_policy.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

struct IntegrityEvidence {
    std::string rule;
    std::string source;
    std::string file_name;
    std::string sha256;
    std::string signer_name;
    std::string file_description;
    std::string company_name;
    std::string match_reason;
    bool signature_valid;
};

struct ClientReport {
    std::string server_id;
    std::string user_id;
    std::string protocol_version;
    std::string challenge;
    std::uint64_t sequence;
    std::string sent_at;
    std::vector<ReportedMod> mods;
    std::vector<std::string> violations;
    std::vector<IntegrityEvidence> violation_evidence;
};

struct ClientPreflight {
    std::string server_id;
    std::string protocol_version;
    std::vector<ReportedMod> mods;
    std::vector<std::string> violations;
    std::vector<IntegrityEvidence> violation_evidence;
};

struct ClientPreflightResponse {
    bool accepted;
    std::string reason;
    std::string detail;
};

enum class ClientPreflightExit : unsigned long {
    accepted = 0,
    invalid_config = 20,
    game_root_unavailable = 21,
    steam_user_unavailable = 22,
    scan_unavailable = 23,
    transport_failed = 24,
    http_rejected = 25,
    invalid_response = 26,
    integrity_violation = 27,
    unapproved_mod = 28,
    rejected = 29,
};

struct ClientConfig {
    std::string coordinator;
    std::string website;
    std::string server_id;
};

enum class ClientUiCommandKind {
    verify,
    giftcode,
};

struct ClientUiCommand {
    ClientUiCommandKind kind;
    std::string value;
};

[[nodiscard]] auto steam_user_id_from_account_id(
    std::uint32_t account_id
) -> std::string;

[[nodiscard]] auto next_report_sequence(
    std::uint64_t previous,
    std::uint64_t wall_clock_milliseconds
) -> std::uint64_t;

[[nodiscard]] auto should_retry_client_http(
    std::optional<unsigned long> status,
    unsigned long win32_error,
    unsigned int attempt,
    unsigned int maximum_attempts
) -> bool;

[[nodiscard]] auto scan_mod_inventory(
    const std::filesystem::path& game_root
) -> std::vector<ReportedMod>;

[[nodiscard]] auto build_client_report_json(const ClientReport& report)
    -> std::string;

[[nodiscard]] auto format_runtime_integrity_message(
    std::span<const std::string> violations,
    std::span<const IntegrityEvidence> evidence
) -> std::string;

[[nodiscard]] auto build_client_preflight_json(
    const ClientPreflight& preflight
) -> std::string;

[[nodiscard]] auto parse_client_preflight_response(std::string_view json)
    -> std::optional<ClientPreflightResponse>;

[[nodiscard]] auto build_challenge_request_json(
    std::string_view server_id,
    std::string_view user_id
) -> std::string;

[[nodiscard]] auto parse_client_config(std::string_view json)
    -> std::optional<ClientConfig>;

[[nodiscard]] auto parse_challenge_json(std::string_view json)
    -> std::optional<std::string>;

[[nodiscard]] auto parse_client_ui_command(std::string_view value)
    -> std::optional<ClientUiCommand>;

[[nodiscard]] auto build_client_ui_url(
    std::string_view website,
    const ClientUiCommand& command
) -> std::string;

}  // namespace palverify
