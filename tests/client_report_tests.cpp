#include "palverify/client_report.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
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
        json.find("Program Files") == std::string::npos
            && json.find("Info.json") == std::string::npos
            && json.find("cheatengine-x86_64.exe") == std::string::npos,
        "report must not upload paths, file inventory, or process names"
    );
}

void client_config_requires_https_except_loopback_tests()
{
    const auto config = palverify::parse_client_config(
        R"({
            "coordinator":"https://palworld-3-mien-website.vercel.app/api/palverify",
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
            R"({"coordinator":"http://127.0.0.1:18801","serverId":"bnb"})"
        )
            .has_value(),
        "loopback HTTP should remain available for tests"
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
            "report JSON stays compact",
            report_json_contains_only_compact_policy_fields,
        },
        {
            "client config requires secure transport",
            client_config_requires_https_except_loopback_tests,
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
