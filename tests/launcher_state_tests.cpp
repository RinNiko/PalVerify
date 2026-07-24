#include "palverify/launcher_state.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::exception {
public:
    explicit TestFailure(std::string message) : message_{std::move(message)}
    {
    }

    [[nodiscard]] auto what() const noexcept -> const char* override
    {
        return message_.c_str();
    }

private:
    std::string message_;
};

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw TestFailure{std::string{message}};
    }
}

void manifest_parser_reads_update_and_game_requirements()
{
    const auto manifest = palverify::parse_launcher_manifest(R"({
        "launcherVersion":"0.5.0",
        "minimumLauncherVersion":"0.5.0",
        "launcherDownloadUrl":"https://downloads.minerua.net/Pal3Mien-setup.exe",
        "launcherSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "palVerifyVersion":"0.5.0",
        "requiredPalworldBuildId":"24181527",
        "palworldVersion":"v1.0.1.100619",
        "serverOnline":true,
        "websiteUrl":"https://minerua.net/",
        "newsUrl":"https://minerua.net/tin-tuc"
    })");

    require(manifest.has_value(), "valid manifest");
    require(
        manifest->required_palworld_build_id == "24181527",
        "required Palworld build"
    );
    require(manifest->server_online, "server online");
}

void steam_launch_uri_opens_palworld_without_server_arguments()
{
    require(
        palverify::steam_launch_uri()
            == "steam://rungameid/1623730",
        "launcher must open Palworld normally through Steam"
    );
    require(
        palverify::steam_launch_uri().find("-connect")
            == std::string_view::npos,
        "launcher must not pass a server address to Steam"
    );
}

void github_latest_release_resolves_exact_manifest_asset()
{
    const auto url = palverify::github_release_asset_url(
        R"({
            "tag_name":"v0.5.7",
            "assets":[
                {
                    "name":"PalVerify_0.5.7_x64-setup.exe",
                    "browser_download_url":"https://github.com/RinNiko/PalVerify/releases/download/v0.5.7/PalVerify_0.5.7_x64-setup.exe"
                },
                {
                    "name":"palverify-launcher-manifest.json",
                    "browser_download_url":"https://github.com/RinNiko/PalVerify/releases/download/v0.5.7/palverify-launcher-manifest.json"
                }
            ]
        })",
        "palverify-launcher-manifest.json"
    );

    require(url.has_value(), "GitHub manifest asset must resolve");
    require(
        *url
            == "https://github.com/RinNiko/PalVerify/releases/download/"
               "v0.5.7/palverify-launcher-manifest.json",
        "launcher must use the exact GitHub release asset"
    );
}

void github_latest_release_rejects_untrusted_or_wrong_assets()
{
    const auto untrusted = palverify::github_release_asset_url(
        R"({
            "assets":[{
                "name":"palverify-launcher-manifest.json",
                "browser_download_url":"https://downloads.example/palverify-launcher-manifest.json"
            }]
        })",
        "palverify-launcher-manifest.json"
    );
    require(
        !untrusted.has_value(),
        "release metadata must not redirect the launcher outside GitHub"
    );

    const auto wrong_name = palverify::github_release_asset_url(
        R"({
            "assets":[{
                "name":"almost-palverify-launcher-manifest.json",
                "browser_download_url":"https://github.com/RinNiko/PalVerify/releases/download/v0.5.7/almost-palverify-launcher-manifest.json"
            }]
        })",
        "palverify-launcher-manifest.json"
    );
    require(
        !wrong_name.has_value(),
        "release asset matching must require the exact manifest name"
    );
}

void github_redirects_require_absolute_https_urls()
{
    const auto release_asset = palverify::validated_https_redirect(
        "https://release-assets.githubusercontent.com/download/file.exe"
    );
    require(
        release_asset.has_value()
            && *release_asset
                == "https://release-assets.githubusercontent.com/download/file.exe",
        "GitHub HTTPS release asset redirect must be accepted"
    );
    require(
        !palverify::validated_https_redirect(
             "http://release-assets.githubusercontent.com/download/file.exe"
         )
             .has_value(),
        "redirects must never downgrade HTTPS to HTTP"
    );
    require(
        !palverify::validated_https_redirect("/relative/file.exe").has_value(),
        "ambiguous relative redirects must be rejected"
    );
}

void steam_manifest_parser_detects_pending_update()
{
    const auto ready = palverify::parse_steam_app_state(R"(
        "AppState"
        {
            "appid" "1623730"
            "StateFlags" "4"
            "buildid" "24181527"
            "BytesToDownload" "0"
            "TargetBuildID" "0"
        }
    )");
    require(ready.has_value(), "Steam manifest parsed");
    require(ready->build_id == "24181527", "Steam build id");
    require(!ready->update_pending, "ready Steam build");

    const auto downloaded = palverify::parse_steam_app_state(R"(
        "AppState"
        {
            "StateFlags" "4"
            "buildid" "24181527"
            "BytesToDownload" "217376"
            "BytesDownloaded" "217376"
            "TargetBuildID" "0"
        }
    )");
    require(downloaded.has_value(), "downloaded Steam manifest parsed");
    require(
        !downloaded->update_pending,
        "fully downloaded Steam update is not pending"
    );

    const auto pending = palverify::parse_steam_app_state(R"(
        "AppState"
        {
            "StateFlags" "1026"
            "buildid" "24000000"
            "BytesToDownload" "1234"
            "BytesDownloaded" "600"
            "TargetBuildID" "24181527"
        }
    )");
    require(pending.has_value(), "pending Steam manifest parsed");
    require(pending->update_pending, "pending update detected");
}

[[nodiscard]] auto current_manifest() -> palverify::LauncherManifest
{
    return {
        .launcher_version = "0.5.0",
        .minimum_launcher_version = "0.5.0",
        .launcher_download_url =
            "https://downloads.minerua.net/Pal3Mien-setup.exe",
        .launcher_sha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .palverify_version = "0.5.0",
        .required_palworld_build_id = "24181527",
        .palworld_version = "v1.0.1.100619",
        .server_online = true,
        .website_url = "https://minerua.net/",
        .news_url = "https://minerua.net/tin-tuc",
    };
}

void launcher_decision_forces_launcher_and_payload_updates()
{
    const palverify::SteamAppState steam{
        .installed = true,
        .build_id = "24181527",
        .update_pending = false,
    };
    const auto manifest = current_manifest();

    require(
        palverify::evaluate_launcher(
            "0.4.0",
            "0.5.0",
            steam,
            manifest
        )
            == palverify::LauncherStatus::LauncherUpdateRequired,
        "outdated launcher must update"
    );
    require(
        palverify::evaluate_launcher(
            "0.5.0",
            "0.4.0",
            steam,
            manifest
        )
            == palverify::LauncherStatus::LauncherUpdateRequired,
        "outdated bundled PalVerify must update"
    );
}

void launcher_decision_locks_game_until_steam_build_matches()
{
    auto manifest = current_manifest();
    palverify::SteamAppState steam{
        .installed = true,
        .build_id = "24000000",
        .update_pending = true,
    };
    require(
        palverify::evaluate_launcher(
            "0.5.0",
            "0.5.0",
            steam,
            manifest
        )
            == palverify::LauncherStatus::GameUpdateRequired,
        "pending Steam update must lock start"
    );

    steam.build_id = "24181527";
    steam.update_pending = false;
    require(
        palverify::evaluate_launcher(
            "0.5.0",
            "0.5.0",
            steam,
            manifest
        )
            == palverify::LauncherStatus::Ready,
        "matching versions must allow start"
    );

    manifest.server_online = false;
    require(
        palverify::evaluate_launcher(
            "0.5.0",
            "0.5.0",
            steam,
            manifest
        )
            == palverify::LauncherStatus::ServerUnavailable,
        "offline server must lock start"
    );
}

void launcher_stays_locked_when_payload_installation_fails()
{
    require(
        !palverify::launcher_can_start(
            palverify::LauncherStatus::Ready,
            false
        ),
        "failed PalVerify installation must lock Start"
    );
    require(
        palverify::launcher_can_start(
            palverify::LauncherStatus::Ready,
            true
        ),
        "installed PalVerify payload allows Start"
    );
}

void transient_http_failures_are_retried_with_a_bound()
{
    constexpr unsigned long timeout_error = 12002;
    require(
        palverify::should_retry_http(
            std::nullopt,
            timeout_error,
            1,
            3
        ),
        "WinHTTP timeout must be retried before the final attempt"
    );
    require(
        palverify::should_retry_http(503, 0, 2, 3),
        "GitHub transient 5xx responses must be retried"
    );
    require(
        !palverify::should_retry_http(
            std::nullopt,
            timeout_error,
            3,
            3
        ),
        "retry count must remain bounded"
    );
    require(
        !palverify::should_retry_http(404, 0, 1, 3),
        "permanent HTTP failures must not be retried"
    );
    require(
        !palverify::should_retry_http(200, 0, 1, 3),
        "successful responses must not be retried"
    );
}

void support_log_is_copy_ready_and_sanitized()
{
    const auto log = palverify::build_launcher_support_log({
        .code = "INSTALL_PAYLOAD_FAILED",
        .detail = "payload-write-failed\r\nforged=entry",
        .launcher_version = "0.5.7",
        .palverify_version = "0.5.7",
        .local_palworld_build = "24181527",
        .required_palworld_build = "24181527",
    });

    require(
        log.starts_with("PALVERIFY SUPPORT LOG\n"),
        "support log must have a recognizable header"
    );
    require(
        log.find("code=INSTALL_PAYLOAD_FAILED\n")
            != std::string::npos,
        "support log must include the stable failure code"
    );
    require(
        log.find("detail=payload-write-failed  forged=entry\n")
            != std::string::npos,
        "support log values must remain on one line"
    );
    require(
        log.find("launcher=0.5.7\n") != std::string::npos
            && log.find("palverify=0.5.7\n") != std::string::npos,
        "support log must identify launcher and PalVerify versions"
    );
    require(
        log.find("local_palworld_build=24181527\n")
            != std::string::npos
            && log.find("required_palworld_build=24181527\n")
                != std::string::npos,
        "support log must identify local and required Palworld builds"
    );
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

auto main() -> int
{
    const std::vector<TestCase> tests{
        {"launcher manifest parses", manifest_parser_reads_update_and_game_requirements},
        {
            "Steam launch has no server arguments",
            steam_launch_uri_opens_palworld_without_server_arguments,
        },
        {
            "GitHub latest release resolves its manifest asset",
            github_latest_release_resolves_exact_manifest_asset,
        },
        {
            "GitHub release assets stay exact and trusted",
            github_latest_release_rejects_untrusted_or_wrong_assets,
        },
        {
            "GitHub redirects stay HTTPS",
            github_redirects_require_absolute_https_urls,
        },
        {"Steam update state parses", steam_manifest_parser_detects_pending_update},
        {
            "launcher and payload updates are mandatory",
            launcher_decision_forces_launcher_and_payload_updates,
        },
        {
            "Steam build must match server",
            launcher_decision_locks_game_until_steam_build_matches,
        },
        {
            "PalVerify payload must be installed",
            launcher_stays_locked_when_payload_installation_fails,
        },
        {
            "transient HTTP failures retry with a bound",
            transient_http_failures_are_retried_with_a_bound,
        },
        {
            "launcher failures produce copy-ready support logs",
            support_log_is_copy_ready_and_sanitized,
        },
    };

    std::size_t passed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << " passed, " << tests.size() - passed
              << " failed\n";
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
