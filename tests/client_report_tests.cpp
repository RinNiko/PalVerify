#include "palverify/client_report.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
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

template <typename Actual, typename Expected>
void require_equal(
    const Actual& actual,
    const Expected& expected,
    std::string_view message
)
{
    if (actual != expected) {
        throw TestFailure{std::string{message}};
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

[[nodiscard]] auto temporary_root(std::string_view name)
    -> std::filesystem::path
{
    const auto root =
        std::filesystem::temp_directory_path()
        / ("palverify-" + std::string{name});
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    return root;
}

void write_file(
    const std::filesystem::path& path,
    std::string_view content
)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << content;
}

void steam_account_id_converts_to_authenticated_user_id_shape()
{
    require_equal(
        palverify::steam_user_id_from_account_id(356765355U),
        std::string{"steam_76561198317031083"},
        "Steam account ID conversion"
    );
}

void workshop_inventory_reports_package_not_file_list()
{
    const auto root = temporary_root("inventory");
    const auto package =
        root / "Mods" / "Workshop" / "PalVerify";
    write_file(
        package / "Info.json",
        R"({"PackageName":"PalVerify","Version":"0.3.0"})"
    );
    write_file(package / "client" / "Scripts" / "main.lua", "return true\n");
    write_file(
        root / "Mods" / "Workshop" / "BadMod" / "Info.json",
        R"({"PackageName":"InfiniteStamina","Version":"1.0"})"
    );

    const auto inventory = palverify::scan_mod_inventory(root);
    require_equal(inventory.size(), std::size_t{2}, "two package summaries");
    require_equal(
        inventory[0].id,
        std::string{"InfiniteStamina"},
        "inventory sorted by package id"
    );
    require_equal(
        inventory[1].id,
        std::string{"PalVerify"},
        "PalVerify package present"
    );
    for (const auto& mod : inventory) {
        require_equal(mod.digest.size(), std::size_t{64}, "SHA-256 digest");
        require(
            mod.id.find('\\') == std::string::npos
                && mod.id.find('/') == std::string::npos,
            "report must not contain local paths"
        );
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void configured_workshop_inventory_reports_external_packages()
{
    const auto root = temporary_root("external-workshop-inventory");
    const auto workshop_root = root / "SteamWorkshop";
    write_file(
        root / "Mods" / "PalModSettings.ini",
        "[PalModSettings]\n"
        "bGlobalEnableMod=True\n"
        "WorkshopRootDir=" + workshop_root.string() + "\n"
        "ConfigVersion=1.0\n"
    );
    write_file(
        root / "Mods" / "Workshop" / "PalVerify" / "Info.json",
        R"({"PackageName":"PalVerify","Version":"1.0.2"})"
    );
    const auto ue4ss_package = workshop_root / "3625223587";
    write_file(
        ue4ss_package / "Info.json",
        R"({
            "PackageName":"UE4SSExperimentalPW",
            "Version":"experimental-palworld-6"
        })"
    );
    write_file(
        ue4ss_package
            / "Mods"
            / "StatueMapMarkers"
            / "Scripts"
            / "main.lua",
        "return true\n"
    );
    write_file(
        ue4ss_package
            / "Mods"
            / "StatueMapMarkers"
            / "enabled.txt",
        ""
    );
    write_file(
        ue4ss_package
            / "Mods"
            / "BPModLoaderMod"
            / "Scripts"
            / "main.lua",
        "return true\n"
    );

    const auto inventory = palverify::scan_mod_inventory(root);
    require_equal(
        inventory.size(),
        std::size_t{3},
        "configured WorkshopRootDir and enabled nested mod must be reported"
    );
    require_equal(
        inventory[0].id,
        std::string{"PalVerify"},
        "installed PalVerify package remains present"
    );
    require_equal(
        inventory[1].id,
        std::string{"StatueMapMarkers"},
        "enabled nested UE4SS mod must be reported by folder name"
    );
    require_equal(
        inventory[1].version,
        std::string{"ue4ss"},
        "nested UE4SS mod version marker"
    );
    require_equal(
        inventory[1].digest.size(),
        std::size_t{64},
        "nested UE4SS mod digest"
    );
    require_equal(
        inventory[2].id,
        std::string{"UE4SSExperimentalPW"},
        "external UE4SS package must be reported"
    );
    require_equal(
        inventory[2].version,
        std::string{"experimental-palworld-6"},
        "external workshop version"
    );
    require_equal(
        inventory[2].digest.size(),
        std::size_t{64},
        "external workshop package digest"
    );

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void installed_workshop_package_wins_over_external_source_duplicate()
{
    const auto root = temporary_root("workshop-source-duplicate");
    const auto external_root = root / "SteamWorkshop";
    write_file(
        root / "Mods" / "PalModSettings.ini",
        "[PalModSettings]\n"
        "bGlobalEnableMod=True\n"
        "WorkshopRootDir=" + external_root.string() + "\n"
        "ConfigVersion=1.0\n"
    );

    const auto local_package =
        root / "Mods" / "Workshop" / "3625223587";
    write_file(
        local_package / "Info.json",
        R"({"PackageName":"UE4SSExperimentalPW","Version":"stable"})"
    );
    write_file(local_package / "UE4SS.dll", "installed-active-copy");

    const auto external_package = external_root / "3625223587";
    write_file(
        external_package / "Info.json",
        R"({"PackageName":"UE4SSExperimentalPW","Version":"stable"})"
    );
    write_file(external_package / "UE4SS.dll", "steam-source-copy");

    const auto inventory = palverify::scan_mod_inventory(root);
    std::size_t ue4ss_reports = 0;
    for (const auto& mod : inventory) {
        if (mod.id == "UE4SSExperimentalPW") {
            ++ue4ss_reports;
        }
    }
    require_equal(
        ue4ss_reports,
        std::size_t{1},
        "installed Workshop package must suppress its external source duplicate"
    );

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void managed_palverify_cache_does_not_change_ue4ss_parent_digest()
{
    const auto root = temporary_root("ue4ss-managed-palverify-cache");
    const auto package =
        root / "Mods" / "Workshop" / "3625223587";
    write_file(
        package / "Info.json",
        R"({"PackageName":"UE4SSExperimentalPW","Version":"stable"})"
    );
    write_file(package / "UE4SS.dll", "ue4ss-core");
    const auto managed_client =
        package / "Mods" / "PalVerify" / "Scripts" / "PalVerifyClient.exe";
    write_file(managed_client, "client-v1");

    const auto before = palverify::scan_mod_inventory(root);
    require_equal(before.size(), std::size_t{1}, "one UE4SS package");
    const auto original_digest = before.front().digest;

    write_file(managed_client, "client-v2");
    const auto after_client_update = palverify::scan_mod_inventory(root);
    require_equal(
        after_client_update.front().digest,
        original_digest,
        "managed PalVerify cache must not alter the UE4SS parent digest"
    );

    write_file(package / "UE4SS.dll", "tampered-ue4ss-core");
    const auto after_core_change = palverify::scan_mod_inventory(root);
    require(
        after_core_change.front().digest != original_digest,
        "UE4SS core changes must still alter its parent digest"
    );

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void runtime_inventory_refresh_does_not_block_heartbeat_snapshot()
{
    using namespace std::chrono_literals;

    const std::vector<palverify::ReportedMod> initial{{
        .id = "PalVerify",
        .version = "1.0.13",
        .digest = std::string(64, 'a'),
    }};
    const std::vector<palverify::ReportedMod> refreshed{{
        .id = "PalVerify",
        .version = "1.0.13",
        .digest = std::string(64, 'b'),
    }};

    std::promise<void> scan_started;
    auto scan_started_future = scan_started.get_future();
    std::promise<void> release_scan;
    auto release_scan_future = release_scan.get_future().share();
    std::atomic<unsigned int> scans{0};

    palverify::AsyncModInventory inventory{
        {},
        initial,
        [&](const std::filesystem::path&) {
            ++scans;
            scan_started.set_value();
            release_scan_future.wait();
            return refreshed;
        },
    };
    inventory.request_refresh();
    require(
        scan_started_future.wait_for(2s) == std::future_status::ready,
        "background inventory scan must start"
    );

    const auto snapshot_started = std::chrono::steady_clock::now();
    const auto during_scan = inventory.snapshot();
    const auto snapshot_elapsed =
        std::chrono::steady_clock::now() - snapshot_started;
    require(
        snapshot_elapsed < 100ms,
        "heartbeat snapshot must not wait for mod hashing"
    );
    require_equal(
        during_scan.front().digest,
        initial.front().digest,
        "last complete inventory must remain available while scanning"
    );

    release_scan.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (inventory.refresh_in_progress()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(
        !inventory.refresh_in_progress(),
        "background inventory scan must complete"
    );
    require_equal(scans.load(), 1U, "one refresh request must run one scan");
    require_equal(
        inventory.snapshot().front().digest,
        refreshed.front().digest,
        "completed inventory must replace the previous snapshot"
    );
}

void failed_inventory_refresh_is_fail_closed()
{
    using namespace std::chrono_literals;

    const std::vector<palverify::ReportedMod> initial{{
        .id = "PalVerify",
        .version = "1.0.13",
        .digest = std::string(64, 'a'),
    }};
    palverify::AsyncModInventory inventory{
        {},
        initial,
        [](const std::filesystem::path&)
            -> std::vector<palverify::ReportedMod> {
            throw std::runtime_error{"scan failed"};
        },
    };

    inventory.request_refresh();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (inventory.refresh_in_progress()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(
        inventory.refresh_failed(),
        "failed background scan must be visible to heartbeat enforcement"
    );
    require_equal(
        inventory.snapshot().front().digest,
        initial.front().digest,
        "failed scan must not publish a partial inventory"
    );
}

void report_json_contains_only_compact_policy_fields()
{
    const palverify::ClientReport report{
        .server_id = "bnb",
        .user_id = "steam_76561198317031083",
        .protocol_version = "3",
        .challenge =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .sequence = 9,
        .sent_at = "2026-07-24T07:00:00Z",
        .mods = {{
            .id = "PalVerify",
            .version = "0.3.0",
            .digest =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        }},
        .violations = {"CHEAT_ENGINE_RUNNING"},
        .violation_evidence = {{
            .rule = "INJECTED_MODULE_DETECTED",
            .source = "module",
            .file_name = "trainerlib_x64.dll",
            .sha256 =
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            .signer_name = "WeMod LLC",
            .file_description = "TrainerLib Plugin",
            .company_name = "WeMod LLC",
            .match_reason = "WEMOD_MODULE_SIGNATURE",
            .signature_valid = true,
        }},
    };

    const auto json = palverify::build_client_report_json(report);
    require(
        json.find("\"serverId\":\"bnb\"") != std::string::npos,
        "server id"
    );
    require(
        json.find("\"userId\":\"steam_76561198317031083\"")
            != std::string::npos,
        "user id"
    );
    require(
        json.find("\"challenge\":\"aaaaaaaaaaaaaaaa")
            != std::string::npos,
        "session challenge"
    );
    require(
        json.find("\"id\":\"PalVerify\"") != std::string::npos,
        "compact mod id"
    );
    require(
        json.find("CHEAT_ENGINE_RUNNING") != std::string::npos,
        "compact integrity rule"
    );
    require(
        json.find("\"fileName\":\"trainerlib_x64.dll\"")
                != std::string::npos
            && json.find("\"matchReason\":\"WEMOD_MODULE_SIGNATURE\"")
                != std::string::npos
            && json.find("\"signerName\":\"WeMod LLC\"")
                != std::string::npos
            && json.find("\"fileDescription\":\"TrainerLib Plugin\"")
                != std::string::npos
            && json.find("\"companyName\":\"WeMod LLC\"")
                != std::string::npos,
        "report must include bounded module evidence for false-positive review"
    );
    require(
        json.find("Program Files") == std::string::npos
            && json.find("Info.json") == std::string::npos
            && json.find("C:\\") == std::string::npos,
        "report must not upload paths or file inventories"
    );
}

void preflight_json_omits_player_identity_and_session_state()
{
    const palverify::ClientPreflight preflight{
        .server_id = "bnb",
        .protocol_version = "3",
        .mods = {{
            .id = "PalVerify",
            .version = "1.0.1",
            .digest =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        }},
        .violations = {"CHEAT_ENGINE_RUNNING"},
        .violation_evidence = {{
            .rule = "INJECTED_MODULE_DETECTED",
            .source = "module",
            .file_name = "trainerlib_x64.dll",
            .sha256 =
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            .signer_name = "WeMod LLC",
            .file_description = "TrainerLib Plugin",
            .company_name = "WeMod LLC",
            .match_reason = "WEMOD_MODULE_SIGNATURE",
            .signature_valid = true,
        }},
    };

    const auto json = palverify::build_client_preflight_json(preflight);
    require(
        json.find("\"serverId\":\"bnb\"") != std::string::npos,
        "preflight server id"
    );
    require(
        json.find("\"protocolVersion\":\"3\"") != std::string::npos,
        "preflight protocol"
    );
    require(
        json.find("\"id\":\"PalVerify\"") != std::string::npos,
        "preflight compact mod id"
    );
    require(
        json.find("CHEAT_ENGINE_RUNNING") != std::string::npos,
        "preflight compact integrity rule"
    );
    require(
        json.find("userId") == std::string::npos
            && json.find("challenge") == std::string::npos
            && json.find("sentAt") == std::string::npos,
        "preflight must not contain identity or session state"
    );
}

void runtime_alert_identifies_the_flagged_software_without_local_paths()
{
    const std::vector<std::string> violations{
        "INJECTED_MODULE_DETECTED",
    };
    const std::vector<palverify::IntegrityEvidence> evidence{{
        .rule = "INJECTED_MODULE_DETECTED",
        .source = "module",
        .file_name = "trainerlib_x64.dll",
        .sha256 =
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .signer_name = "WeMod LLC",
        .file_description = "TrainerLib Plugin",
        .company_name = "WeMod LLC",
        .match_reason = "WEMOD_MODULE_SIGNATURE",
        .signature_valid = true,
    }};

    const auto message =
        palverify::format_runtime_integrity_message(violations, evidence);
    require(
        message.find("trainerlib_x64.dll") != std::string::npos
            && message.find("TrainerLib Plugin") != std::string::npos
            && message.find("WeMod LLC") != std::string::npos
            && message.find("WEMOD_MODULE_SIGNATURE") != std::string::npos,
        "runtime alert must identify the exact flagged module and publisher"
    );
    require(
        message.find("bbbbbbbb") == std::string::npos
            && message.find("C:\\") == std::string::npos,
        "player alert must stay compact and never expose a local path"
    );
}

void preflight_response_requires_bounded_safe_codes()
{
    const auto accepted = palverify::parse_client_preflight_response(
        R"({"accepted":true,"reason":"VERIFIED"})"
    );
    require(accepted.has_value(), "accepted preflight response should parse");
    require(accepted->accepted, "accepted response flag");
    require_equal(
        accepted->reason,
        std::string{"VERIFIED"},
        "accepted response reason"
    );

    const auto rejected = palverify::parse_client_preflight_response(
        R"({
            "accepted":false,
            "reason":"UNAPPROVED_MOD",
            "detail":"PalVerify:DIGEST_MISMATCH"
        })"
    );
    require(rejected.has_value(), "rejected preflight response should parse");
    require(!rejected->accepted, "rejected response flag");
    require_equal(
        rejected->detail,
        std::string{"PalVerify:DIGEST_MISMATCH"},
        "safe rejection detail"
    );

    require(
        !palverify::parse_client_preflight_response(
             R"({"accepted":false,"reason":"BAD CODE","detail":"C:\\Users\\player"})"
         )
             .has_value(),
        "unsafe or path-bearing response must be rejected"
    );
}

void runtime_policy_rejection_explains_the_kick_to_the_player()
{
    const auto mod_message = palverify::format_policy_rejection_message({
        .accepted = false,
        .reason = "UNAPPROVED_MOD",
        .detail = "PalVerify:VERSION_MISMATCH",
    });
    require(
        mod_message.find("mod không được máy chủ chấp thuận")
                != std::string::npos
            && mod_message.find("PalVerify:VERSION_MISMATCH")
                != std::string::npos
            && mod_message.find("cập nhật") != std::string::npos,
        "unapproved-mod alert must explain both the cause and recovery"
    );

    const auto missing_message = palverify::format_policy_rejection_message({
        .accepted = false,
        .reason = "MISSING_PALVERIFY",
        .detail = "STALE_REPORT",
    });
    require(
        missing_message.find("heartbeat") != std::string::npos
            && missing_message.find("PalVerifyClient.exe")
                != std::string::npos,
        "missing-client alert must explain the heartbeat timeout"
    );
}

void client_config_requires_https_except_loopback_tests()
{
    const auto config = palverify::parse_client_config(
        R"({
            "coordinator":"https://palworld-3-mien-website.vercel.app/api/palverify",
            "website":"https://palworld-3-mien-website.vercel.app",
            "serverId":"bnb"
        })"
    );
    require(config.has_value(), "HTTPS client config should parse");
    require_equal(
        config->coordinator,
        std::string{
            "https://palworld-3-mien-website.vercel.app/api/palverify"
        },
        "coordinator"
    );
    require_equal(
        config->website,
        std::string{"https://palworld-3-mien-website.vercel.app"},
        "website"
    );
    require_equal(config->server_id, std::string{"bnb"}, "server id");

    require(
        !palverify::parse_client_config(
             R"({"coordinator":"http://public.example","serverId":"bnb"})"
         )
             .has_value(),
        "public plaintext endpoint must be rejected"
    );
    require(
        palverify::parse_client_config(
            R"({
                "coordinator":"http://127.0.0.1:18801",
                "website":"http://127.0.0.1:3000",
                "serverId":"bnb"
            })"
        )
            .has_value(),
        "loopback HTTP should remain available for tests"
    );
}

void client_ui_commands_are_strict_and_build_safe_fragments()
{
    const auto verify =
        palverify::parse_client_ui_command("verify|123456");
    require(verify.has_value(), "six-digit verify command should parse");
    require_equal(
        verify->kind,
        palverify::ClientUiCommandKind::verify,
        "verify command kind"
    );
    require_equal(verify->value, std::string{"123456"}, "verify code");
    require_equal(
        palverify::build_client_ui_url(
            "https://palworld-3-mien-website.vercel.app/",
            *verify
        ),
        std::string{
            "https://palworld-3-mien-website.vercel.app/"
            "#gacha-verify?verify=123456"
        },
        "verify deep link"
    );

    const auto giftcode =
        palverify::parse_client_ui_command("giftcode|PAL-3MIEN");
    require(giftcode.has_value(), "giftcode command should parse");
    require_equal(
        giftcode->kind,
        palverify::ClientUiCommandKind::giftcode,
        "giftcode command kind"
    );
    require_equal(
        palverify::build_client_ui_url(
            "https://palworld-3-mien-website.vercel.app",
            *giftcode
        ),
        std::string{
            "https://palworld-3-mien-website.vercel.app/"
            "#giftcode?giftcode=PAL-3MIEN"
        },
        "giftcode deep link"
    );

    require(
        !palverify::parse_client_ui_command("verify|12345").has_value(),
        "short verify code must be rejected"
    );
    require(
        !palverify::parse_client_ui_command(
             "giftcode|https://evil.example"
         ).has_value(),
        "arbitrary URL must be rejected"
    );
    require(
        !palverify::parse_client_ui_command("shell|calc.exe").has_value(),
        "unknown command must be rejected"
    );
}

void challenge_response_requires_a_compact_nonce()
{
    const auto challenge = palverify::parse_challenge_json(
        R"({"challenge":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"
    );
    require(challenge.has_value(), "valid challenge response");
    require_equal(
        *challenge,
        std::string{
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        },
        "challenge value"
    );
    require(
        !palverify::parse_challenge_json(
             R"({"challenge":"contains spaces"})"
         )
             .has_value(),
        "non-compact challenge must be rejected"
    );
}

void challenge_request_binds_server_and_steam_user()
{
    const auto json = palverify::build_challenge_request_json(
        "bnb",
        "steam_76561198317031083"
    );
    require(
        json
            == R"({"serverId":"bnb","userId":"steam_76561198317031083"})",
        "challenge request identity"
    );
}

void report_sequence_uses_wall_clock_floor_after_restart()
{
    require_equal(
        palverify::next_report_sequence(44, 1'721'807'400'000ULL),
        std::uint64_t{1'721'807'400'000ULL},
        "restarted client sequence must jump above an old small counter"
    );
    require_equal(
        palverify::next_report_sequence(
            1'721'807'400'000ULL,
            1'721'807'399'999ULL
        ),
        std::uint64_t{1'721'807'400'001ULL},
        "sequence must stay monotonic when the clock does not advance"
    );
}

void preflight_http_retries_only_bounded_transient_failures()
{
    constexpr unsigned long timeout_error = 12002;
    require(
        palverify::should_retry_client_http(
            std::nullopt,
            timeout_error,
            1,
            2
        ),
        "WinHTTP timeout should retry before the final attempt"
    );
    require(
        palverify::should_retry_client_http(503, 0, 1, 2),
        "coordinator 5xx should retry"
    );
    require(
        !palverify::should_retry_client_http(409, 0, 1, 2),
        "expected challenge conflict should not retry"
    );
    require(
        !palverify::should_retry_client_http(
            std::nullopt,
            timeout_error,
            2,
            2
        ),
        "client retries must remain bounded"
    );
}

}  // namespace

auto main() -> int
{
    const std::vector<TestCase> tests{
        {
            "Steam account id converts to user id",
            steam_account_id_converts_to_authenticated_user_id_shape,
        },
        {
            "Workshop inventory stays compact",
            workshop_inventory_reports_package_not_file_list,
        },
        {
            "configured workshop root reports external packages",
            configured_workshop_inventory_reports_external_packages,
        },
        {
            "installed workshop package wins over external source duplicate",
            installed_workshop_package_wins_over_external_source_duplicate,
        },
        {
            "managed PalVerify cache is excluded from UE4SS parent digest",
            managed_palverify_cache_does_not_change_ue4ss_parent_digest,
        },
        {
            "runtime inventory refresh stays off heartbeat thread",
            runtime_inventory_refresh_does_not_block_heartbeat_snapshot,
        },
        {
            "failed runtime inventory refresh is fail closed",
            failed_inventory_refresh_is_fail_closed,
        },
        {
            "report JSON stays compact",
            report_json_contains_only_compact_policy_fields,
        },
        {
            "preflight JSON omits player identity",
            preflight_json_omits_player_identity_and_session_state,
        },
        {
            "runtime alert identifies flagged software safely",
            runtime_alert_identifies_the_flagged_software_without_local_paths,
        },
        {
            "preflight response codes stay safe",
            preflight_response_requires_bounded_safe_codes,
        },
        {
            "runtime policy rejection explains kick reason",
            runtime_policy_rejection_explains_the_kick_to_the_player,
        },
        {
            "client config requires secure transport",
            client_config_requires_https_except_loopback_tests,
        },
        {
            "client UI commands stay strict",
            client_ui_commands_are_strict_and_build_safe_fragments,
        },
        {
            "challenge response requires compact nonce",
            challenge_response_requires_a_compact_nonce,
        },
        {
            "challenge request binds server and Steam user",
            challenge_request_binds_server_and_steam_user,
        },
        {
            "report sequence uses wall-clock floor",
            report_sequence_uses_wall_clock_floor_after_restart,
        },
        {
            "client HTTP retries stay bounded",
            preflight_http_retries_only_bounded_transient_failures,
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
    std::cout << passed << " passed, " << tests.size() - passed << " failed\n";
    return passed == tests.size() ? EXIT_SUCCESS : EXIT_FAILURE;
}
