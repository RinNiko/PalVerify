#include "palverify/launcher_state.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <regex>
#include <string>
#include <vector>

namespace palverify {
namespace {

[[nodiscard]] auto json_string(
    std::string_view json,
    std::string_view field
) -> std::string
{
    const std::regex pattern{
        "\"" + std::string{field}
            + R"regex("\s*:\s*"([^"]*)")regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(json.begin(), json.end(), match, pattern)) {
        return {};
    }
    return match[1].str();
}

[[nodiscard]] auto json_bool(
    std::string_view json,
    std::string_view field
) -> std::optional<bool>
{
    const std::regex pattern{
        "\"" + std::string{field} + R"regex("\s*:\s*(true|false))regex",
        std::regex::ECMAScript | std::regex::icase,
    };
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(json.begin(), json.end(), match, pattern)) {
        return std::nullopt;
    }
    auto value = match[1].str();
    std::ranges::transform(value, value.begin(), [](char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
    });
    return value == "true";
}

[[nodiscard]] auto vdf_value(
    std::string_view vdf,
    std::string_view field
) -> std::string
{
    const std::regex pattern{
        "\"" + std::string{field} + R"regex("\s*"([^"]*)")regex",
        std::regex::ECMAScript | std::regex::icase,
    };
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(vdf.begin(), vdf.end(), match, pattern)) {
        return {};
    }
    return match[1].str();
}

[[nodiscard]] auto decimal(std::string_view value) -> bool
{
    return !value.empty()
        && std::ranges::all_of(value, [](char character) {
               return std::isdigit(
                          static_cast<unsigned char>(character)
                      )
                   != 0;
           });
}

[[nodiscard]] auto hexadecimal(std::string_view value) -> bool
{
    return value.size() == 64
        && std::ranges::all_of(value, [](char character) {
               return std::isxdigit(
                          static_cast<unsigned char>(character)
                      )
                   != 0;
           });
}

[[nodiscard]] auto parse_unsigned(std::string_view value)
    -> std::optional<unsigned long long>
{
    unsigned long long result = 0;
    const auto conversion = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result
    );
    if (conversion.ec != std::errc{}
        || conversion.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] auto version_parts(std::string_view version)
    -> std::optional<std::array<unsigned int, 4>>
{
    if (!version.empty() && (version.front() == 'v'
                             || version.front() == 'V')) {
        version.remove_prefix(1);
    }
    std::array<unsigned int, 4> parts{};
    std::size_t part = 0;
    while (!version.empty() && part < parts.size()) {
        const auto separator = version.find('.');
        const auto token = version.substr(0, separator);
        unsigned int value = 0;
        const auto conversion = std::from_chars(
            token.data(),
            token.data() + token.size(),
            value
        );
        if (token.empty() || conversion.ec != std::errc{}
            || conversion.ptr != token.data() + token.size()) {
            return std::nullopt;
        }
        parts[part++] = value;
        if (separator == std::string_view::npos) {
            version = {};
        } else {
            version.remove_prefix(separator + 1);
        }
    }
    if (!version.empty() || part == 0) {
        return std::nullopt;
    }
    return parts;
}

[[nodiscard]] auto version_less(
    std::string_view left,
    std::string_view right
) -> bool
{
    const auto left_parts = version_parts(left);
    const auto right_parts = version_parts(right);
    if (!left_parts.has_value() || !right_parts.has_value()) {
        return true;
    }
    return *left_parts < *right_parts;
}

[[nodiscard]] auto clean_log_value(std::string_view value) -> std::string
{
    if (value.empty()) {
        return "unknown";
    }
    std::string clean;
    clean.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        clean.push_back(
            byte < 0x20 || byte == 0x7F ? ' ' : character
        );
    }
    return clean;
}

[[nodiscard]] auto regex_escape(std::string_view value) -> std::string
{
    std::string escaped;
    escaped.reserve(value.size() * 2);
    for (const auto character : value) {
        if (std::string_view{R"(\.^$|()[]{}*+?)"}.find(character)
            != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

}  // namespace

auto parse_launcher_manifest(std::string_view json)
    -> std::optional<LauncherManifest>
{
    const auto server_online = json_bool(json, "serverOnline");
    LauncherManifest manifest{
        .launcher_version = json_string(json, "launcherVersion"),
        .minimum_launcher_version =
            json_string(json, "minimumLauncherVersion"),
        .launcher_download_url =
            json_string(json, "launcherDownloadUrl"),
        .launcher_sha256 = json_string(json, "launcherSha256"),
        .palverify_version = json_string(json, "palVerifyVersion"),
        .required_palworld_build_id =
            json_string(json, "requiredPalworldBuildId"),
        .palworld_version = json_string(json, "palworldVersion"),
        .server_online = server_online.value_or(false),
        .website_url = json_string(json, "websiteUrl"),
        .news_url = json_string(json, "newsUrl"),
    };
    if (!version_parts(manifest.launcher_version).has_value()
        || !version_parts(manifest.minimum_launcher_version).has_value()
        || !version_parts(manifest.palverify_version).has_value()
        || !manifest.launcher_download_url.starts_with("https://")
        || !hexadecimal(manifest.launcher_sha256)
        || !decimal(manifest.required_palworld_build_id)
        || manifest.palworld_version.empty() || !server_online.has_value()
        || !manifest.website_url.starts_with("https://")
        || !manifest.news_url.starts_with("https://")) {
        return std::nullopt;
    }
    return manifest;
}

auto github_release_asset_url(
    std::string_view json,
    std::string_view asset_name
) -> std::optional<std::string>
{
    if (asset_name.empty()) {
        return std::nullopt;
    }

    const std::regex name_pattern{
        R"regex("name"\s*:\s*")regex" + regex_escape(asset_name)
            + R"regex(")regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> name_match;
    if (!std::regex_search(
            json.begin(),
            json.end(),
            name_match,
            name_pattern
        )) {
        return std::nullopt;
    }

    const auto after_name = json.substr(
        static_cast<std::size_t>(name_match.position())
        + static_cast<std::size_t>(name_match.length())
    );
    const std::regex url_pattern{
        R"regex("browser_download_url"\s*:\s*"([^"]+)")regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> url_match;
    if (!std::regex_search(
            after_name.begin(),
            after_name.end(),
            url_match,
            url_pattern
        )) {
        return std::nullopt;
    }

    const std::regex next_asset_name{
        R"regex("name"\s*:)regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> next_name_match;
    if (std::regex_search(
            after_name.begin(),
            after_name.end(),
            next_name_match,
            next_asset_name
        )
        && next_name_match.position() < url_match.position()) {
        return std::nullopt;
    }

    const auto url = url_match[1].str();
    if (!url.starts_with("https://github.com/")
        || url.find("/releases/download/") == std::string::npos) {
        return std::nullopt;
    }
    return url;
}

auto validated_https_redirect(
    std::string_view location
) -> std::optional<std::string>
{
    if (!location.starts_with("https://") || location.size() <= 8) {
        return std::nullopt;
    }
    return std::string{location};
}

auto steam_launch_uri() -> std::string_view
{
    return "steam://rungameid/1623730";
}

auto should_retry_http(
    std::optional<unsigned long> status,
    unsigned long win32_error,
    unsigned int attempt,
    unsigned int maximum_attempts
) -> bool
{
    if (attempt >= maximum_attempts) {
        return false;
    }
    if (status.has_value()) {
        return *status == 408 || *status == 429 || *status >= 500;
    }
    constexpr std::array<unsigned long, 6> transient_errors{
        12002,  // ERROR_WINHTTP_TIMEOUT
        12007,  // ERROR_WINHTTP_NAME_NOT_RESOLVED
        12029,  // ERROR_WINHTTP_CANNOT_CONNECT
        12030,  // ERROR_WINHTTP_CONNECTION_ERROR
        12031,  // ERROR_WINHTTP_RESEND_REQUEST
        12152,  // ERROR_HTTP_INVALID_SERVER_RESPONSE
    };
    return std::ranges::find(transient_errors, win32_error)
        != transient_errors.end();
}

auto parse_steam_app_state(std::string_view vdf)
    -> std::optional<SteamAppState>
{
    const auto build_id = vdf_value(vdf, "buildid");
    const auto build_id_number = parse_unsigned(build_id);
    const auto state_flags = parse_unsigned(vdf_value(vdf, "StateFlags"));
    const auto bytes_to_download =
        parse_unsigned(vdf_value(vdf, "BytesToDownload"));
    const auto bytes_downloaded =
        parse_unsigned(vdf_value(vdf, "BytesDownloaded"));
    const auto target_build =
        parse_unsigned(vdf_value(vdf, "TargetBuildID"));
    if (!decimal(build_id) || !build_id_number.has_value()
        || !state_flags.has_value()) {
        return std::nullopt;
    }
    constexpr auto fully_installed = 4ULL;
    constexpr auto app_running = 64ULL;
    const auto blocking_state_flags =
        *state_flags & ~(fully_installed | app_running);
    return SteamAppState{
        .installed = true,
        .build_id = build_id,
        .update_pending =
            (*state_flags & fully_installed) == 0
            || blocking_state_flags != 0
            || (bytes_to_download.value_or(0) > 0
                && bytes_downloaded.value_or(0)
                    < bytes_to_download.value_or(0))
            || (target_build.has_value() && *target_build != 0
                && *target_build != *build_id_number),
    };
}

auto evaluate_launcher(
    std::string_view local_launcher_version,
    std::string_view embedded_palverify_version,
    const SteamAppState& steam,
    const LauncherManifest& manifest
) -> LauncherStatus
{
    if (version_less(
            local_launcher_version,
            manifest.minimum_launcher_version
        )
        || version_less(
            embedded_palverify_version,
            manifest.palverify_version
        )) {
        return LauncherStatus::LauncherUpdateRequired;
    }
    if (!manifest.server_online) {
        return LauncherStatus::ServerUnavailable;
    }
    if (!steam.installed) {
        return LauncherStatus::GameMissing;
    }
    if (steam.update_pending
        || steam.build_id != manifest.required_palworld_build_id) {
        return LauncherStatus::GameUpdateRequired;
    }
    return LauncherStatus::Ready;
}

auto launcher_can_start(
    LauncherStatus status,
    bool payload_installed,
    bool preflight_succeeded
) -> bool
{
    return status == LauncherStatus::Ready && payload_installed
        && preflight_succeeded;
}

auto launcher_can_prepare_payload(
    LauncherStatus status,
    bool game_running
) -> bool
{
    return status == LauncherStatus::Ready && !game_running;
}

auto build_launcher_support_log(
    const LauncherFailureContext& failure
) -> std::string
{
    std::string log{"PALVERIFY SUPPORT LOG\n"};
    const auto append = [&log](
                            std::string_view key,
                            std::string_view value
                        ) {
        log.append(key);
        log.push_back('=');
        log.append(clean_log_value(value));
        log.push_back('\n');
    };
    append("code", failure.code);
    append("detail", failure.detail);
    append("launcher", failure.launcher_version);
    append("palverify", failure.palverify_version);
    append("local_palworld_build", failure.local_palworld_build);
    append("required_palworld_build", failure.required_palworld_build);
    return log;
}

auto extract_not_whitelisted_mod_ids(
    std::string_view preflight_output
) -> std::vector<std::string>
{
    constexpr std::string_view rejected_reason =
        "reason=UNAPPROVED_MOD";
    if (preflight_output.find(rejected_reason) == std::string_view::npos) {
        return {};
    }

    constexpr std::string_view detail_prefix = "detail=";
    const auto detail_position = preflight_output.find(detail_prefix);
    if (detail_position == std::string_view::npos) {
        return {};
    }
    auto detail = preflight_output.substr(
        detail_position + detail_prefix.size()
    );
    if (const auto whitespace = detail.find_first_of(" \t\r\n");
        whitespace != std::string_view::npos) {
        detail = detail.substr(0, whitespace);
    }

    constexpr std::array<std::string_view, 5> managed_mod_ids{
        "PalVerify",
        "UE4SSExperimentalPW",
        "StatueMapMarkers",
        "PalHud",
        "Pal3MienAutoJoin",
    };
    const auto safe_id = [](std::string_view value) {
        return !value.empty() && value.size() <= 96
            && std::ranges::all_of(value, [](char character) {
                   const auto byte =
                       static_cast<unsigned char>(character);
                   return std::isalnum(byte) != 0 || character == '.'
                       || character == '_' || character == '-'
                       || character == ':';
               });
    };
    const auto managed_id = [&managed_mod_ids](std::string_view value) {
        return std::ranges::any_of(
            managed_mod_ids,
            [value](std::string_view managed) {
                return value.size() == managed.size()
                    && std::ranges::equal(
                        value,
                        managed,
                        [](char left, char right) {
                            return std::tolower(
                                       static_cast<unsigned char>(left)
                                   )
                                == std::tolower(
                                       static_cast<unsigned char>(right)
                                   );
                        }
                    );
            }
        );
    };

    std::vector<std::string> ids;
    std::size_t start = 0;
    while (start < detail.size()) {
        const auto comma = detail.find(',', start);
        const auto entry = detail.substr(
            start,
            comma == std::string_view::npos
                ? detail.size() - start
                : comma - start
        );
        const auto rule_separator = entry.rfind(':');
        if (rule_separator != std::string_view::npos
            && entry.substr(rule_separator + 1) == "NOT_WHITELISTED") {
            const auto id = entry.substr(0, rule_separator);
            if (safe_id(id) && !managed_id(id)
                && std::ranges::find(ids, id) == ids.end()) {
                ids.emplace_back(id);
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return ids;
}

}  // namespace palverify
