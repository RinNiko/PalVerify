#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

struct LauncherManifest {
    std::string launcher_version;
    std::string minimum_launcher_version;
    std::string launcher_download_url;
    std::string launcher_sha256;
    std::string palverify_version;
    std::string required_palworld_build_id;
    std::string palworld_version;
    bool server_online;
    std::string website_url;
    std::string news_url;
};

struct SteamAppState {
    bool installed;
    std::string build_id;
    bool update_pending;
};

struct LauncherFailureContext {
    std::string code;
    std::string detail;
    std::string launcher_version;
    std::string palverify_version;
    std::string local_palworld_build;
    std::string required_palworld_build;
};

enum class LauncherStatus {
    Ready,
    LauncherUpdateRequired,
    GameUpdateRequired,
    GameMissing,
    ServerUnavailable,
};

[[nodiscard]] auto parse_launcher_manifest(std::string_view json)
    -> std::optional<LauncherManifest>;

[[nodiscard]] auto github_release_asset_url(
    std::string_view json,
    std::string_view asset_name
) -> std::optional<std::string>;

[[nodiscard]] auto validated_https_redirect(
    std::string_view location
) -> std::optional<std::string>;

[[nodiscard]] auto steam_launch_uri() -> std::string_view;

[[nodiscard]] auto should_retry_http(
    std::optional<unsigned long> status,
    unsigned long win32_error,
    unsigned int attempt,
    unsigned int maximum_attempts
) -> bool;

[[nodiscard]] auto parse_steam_app_state(std::string_view vdf)
    -> std::optional<SteamAppState>;

[[nodiscard]] auto evaluate_launcher(
    std::string_view local_launcher_version,
    std::string_view embedded_palverify_version,
    const SteamAppState& steam,
    const LauncherManifest& manifest
) -> LauncherStatus;

[[nodiscard]] auto launcher_can_start(
    LauncherStatus status,
    bool payload_installed,
    bool preflight_succeeded
) -> bool;

[[nodiscard]] auto launcher_can_prepare_payload(
    LauncherStatus status,
    bool game_running
) -> bool;

[[nodiscard]] auto build_launcher_support_log(
    const LauncherFailureContext& failure
) -> std::string;

[[nodiscard]] auto extract_not_whitelisted_mod_ids(
    std::string_view preflight_output
) -> std::vector<std::string>;

[[nodiscard]] auto client_log_has_ready_signal(
    std::string_view appended_log
) -> bool;

}  // namespace palverify
