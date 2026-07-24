#include "palverify/installer_settings.hpp"
#include "palverify/payload_archive.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
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

void require_contains(
    std::string_view text,
    std::string_view expected,
    std::string_view message
)
{
    require(text.find(expected) != std::string_view::npos, message);
}

void require_not_contains(
    std::string_view text,
    std::string_view unexpected,
    std::string_view message
)
{
    require(text.find(unexpected) == std::string_view::npos, message);
}

void settings_update_removes_legacy_palverify_activation()
{
    constexpr std::string_view existing =
        "; custom comment\r\n"
        "[PalModSettings]\r\n"
        "bGlobalEnableMod=False\r\n"
        "WorkshopRootDir=E:\\SteamLibrary\\steamapps\\workshop\\content\\1623730\r\n"
        "ActiveModList=PalVerify\r\n"
        "ActiveModList=ExistingMod\r\n"
        "ConfigVersion=1.0\r\n";

    const auto updated =
        palverify::remove_palverify_game_mod_activation(existing);

    require_contains(
        updated,
        "; custom comment\r\n",
        "custom comments must survive"
    );
    require_contains(
        updated,
        "WorkshopRootDir=E:\\SteamLibrary\\steamapps\\workshop\\content\\1623730\r\n",
        "workshop root must survive"
    );
    require_contains(
        updated,
        "ActiveModList=ExistingMod\r\n",
        "existing active mods must survive"
    );
    require_contains(
        updated,
        "bGlobalEnableMod=False\r\n",
        "global mod setting must survive"
    );
    require_not_contains(
        updated,
        "ActiveModList=PalVerify\r\n",
        "legacy PalVerify game-mod activation must be removed"
    );
}

void settings_update_is_idempotent()
{
    constexpr std::string_view existing =
        "[PalModSettings]\n"
        "bGlobalEnableMod=True\n"
        "ActiveModList=PalVerify\n"
        "ConfigVersion=1.0\n";

    const auto once =
        palverify::remove_palverify_game_mod_activation(existing);
    const auto twice =
        palverify::remove_palverify_game_mod_activation(once);

    require(once == twice, "repeated install must not change settings");
    require_not_contains(
        once,
        "ActiveModList=PalVerify",
        "PalVerify activation must stay removed"
    );
}

void missing_section_is_left_unchanged()
{
    constexpr std::string_view existing = "; keep me\nOtherSetting=42\n";

    const auto updated =
        palverify::remove_palverify_game_mod_activation(existing);

    require(updated == existing, "settings without the section must survive");
}

void steam_library_parser_decodes_vdf_paths()
{
    constexpr std::string_view library_folders =
        "\"libraryfolders\"\n"
        "{\n"
        "  \"0\"\n"
        "  {\n"
        "    \"path\" \"D:\\\\Games\\\\Steam\"\n"
        "  }\n"
        "  \"1\"\n"
        "  {\n"
        "    \"path\" \"E:\\\\SteamLibrary\"\n"
        "  }\n"
        "}\n";

    const auto paths =
        palverify::extract_steam_library_paths(library_folders);

    require(paths.size() == 2, "two Steam libraries must be parsed");
    require(
        paths[0] == std::filesystem::path{"D:\\Games\\Steam"},
        "first VDF path must be decoded"
    );
    require(
        paths[1] == std::filesystem::path{"E:\\SteamLibrary"},
        "second VDF path must be decoded"
    );
}

void installer_copies_payload_and_creates_a_settings_backup()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-installer-test-" + unique);
    const auto game_root = test_root / "game";
    const auto payload_root = test_root / "payload";
    const auto settings_path = game_root / "Mods" / "PalModSettings.ini";

    std::filesystem::create_directories(
        game_root / "Pal" / "Binaries" / "Win64"
    );
    std::filesystem::create_directories(payload_root / "client" / "Scripts");
    std::filesystem::create_directories(settings_path.parent_path());

    {
        std::ofstream game_executable{
            game_root / "Pal" / "Binaries" / "Win64"
                / "Palworld-Win64-Shipping.exe",
            std::ios::binary,
        };
        game_executable << "fixture";
    }
    {
        std::ofstream info{payload_root / "Info.json", std::ios::binary};
        info << "{\"PackageName\":\"PalVerify\"}";
    }
    {
        std::ofstream client_script{
            payload_root / "client" / "Scripts" / "main.lua",
            std::ios::binary,
        };
        client_script << "print('fixture')";
    }
    {
        std::ofstream client_agent{
            payload_root / "client" / "Scripts" / "PalVerifyClient.exe",
            std::ios::binary,
        };
        client_agent << "fixture";
    }
    {
        std::ofstream settings{settings_path, std::ios::binary};
        settings << "[PalModSettings]\r\n"
                    "bGlobalEnableMod=False\r\n"
                    "ConfigVersion=1.0\r\n";
    }

    const auto result =
        palverify::install_palverify_payload(game_root, payload_root);

    require(result.success, "valid payload must install");
    require(
        std::filesystem::exists(
            game_root / "Mods" / "Workshop" / "PalVerify" / "Info.json"
        ),
        "Info.json must be copied to the Workshop package"
    );
    require(
        std::filesystem::exists(
            game_root / "Mods" / "Workshop" / "PalVerify" / "client"
                / "Scripts" / "PalVerifyClient.exe"
        ),
        "client agent must be copied"
    );
    require(
        std::filesystem::exists(
            game_root / "Mods" / "PalModSettings.ini.palverify-backup"
        ),
        "original settings must be backed up"
    );

    std::ifstream settings_stream{settings_path, std::ios::binary};
    const std::string installed_settings{
        std::istreambuf_iterator<char>{settings_stream},
        std::istreambuf_iterator<char>{},
    };
    settings_stream.close();
    require_contains(
        installed_settings,
        "bGlobalEnableMod=False\r\n",
        "installer must preserve the global mod setting"
    );
    require_not_contains(
        installed_settings,
        "ActiveModList=PalVerify\r\n",
        "installer must not activate PalVerify in the game mod loader"
    );

    const auto resolved_test_root = std::filesystem::weakly_canonical(test_root);
    const auto resolved_temp =
        std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path()
        );
    require(
        resolved_test_root.string().starts_with(resolved_temp.string()),
        "test cleanup must stay inside the temporary directory"
    );
    std::filesystem::remove_all(resolved_test_root);
}

[[nodiscard]] auto bytes(std::string_view text) -> std::vector<std::byte>
{
    const auto* first =
        reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

void sha256_matches_standard_vectors()
{
    require(
        palverify::sha256_hex({}) ==
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector must match FIPS 180-4"
    );
    const auto abc = bytes("abc");
    require(
        palverify::sha256_hex(abc) ==
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector must match FIPS 180-4"
    );
}

void payload_archive_round_trips_and_rejects_tampering()
{
    std::vector<palverify::PayloadFile> source{
        {
            .relative_path = "Info.json",
            .content = bytes("{\"PackageName\":\"PalVerify\"}"),
            .sha256 = {},
        },
        {
            .relative_path = "client/Scripts/main.lua",
            .content = bytes("print('embedded')"),
            .sha256 = {},
        },
    };

    const auto packed = palverify::pack_payload_archive(source);
    require(packed.success, "valid files must produce a compressed archive");
    require(!packed.archive.empty(), "compressed archive must not be empty");

    const auto unpacked = palverify::unpack_payload_archive(packed.archive);
    require(unpacked.success, "valid compressed archive must unpack");
    require(unpacked.files.size() == source.size(), "all files must round-trip");
    require(
        unpacked.files[1].content == source[1].content,
        "payload bytes must survive compression"
    );
    require(
        unpacked.files[1].sha256 ==
            palverify::sha256_hex(source[1].content),
        "archive must carry and verify each file SHA-256"
    );

    auto tampered = packed.archive;
    tampered.back() ^= std::byte{0x01};
    const auto rejected = palverify::unpack_payload_archive(tampered);
    require(
        !rejected.success,
        "tampered compressed archive must be rejected"
    );
}

void installer_rejects_hash_mismatch_before_writing_payload()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-embedded-hash-test-" + unique);
    const auto game_root = test_root / "game";
    std::filesystem::create_directories(
        game_root / "Pal" / "Binaries" / "Win64"
    );
    {
        std::ofstream executable{
            game_root / "Pal" / "Binaries" / "Win64"
                / "Palworld-Win64-Shipping.exe",
            std::ios::binary,
        };
        executable << "fixture";
    }

    std::vector<palverify::PayloadFile> payload{
        {
            .relative_path = "Info.json",
            .content = bytes("{\"PackageName\":\"PalVerify\"}"),
            .sha256 = std::string(64, '0'),
        },
        {
            .relative_path = "client/Scripts/main.lua",
            .content = bytes("print('fixture')"),
            .sha256 = palverify::sha256_hex(bytes("print('fixture')")),
        },
        {
            .relative_path =
                "client/Scripts/PalVerifyClient.exe",
            .content = bytes("fixture"),
            .sha256 = palverify::sha256_hex(bytes("fixture")),
        },
    };

    const auto result = palverify::install_palverify_payload(
        game_root,
        std::span<const palverify::PayloadFile>{payload}
    );

    require(!result.success, "mismatched SHA-256 must fail installation");
    require(
        result.detail == "payload-hash-mismatch",
        "hash mismatch must return a stable reason"
    );
    require(
        !std::filesystem::exists(
            game_root / "Mods" / "Workshop" / "PalVerify"
        ),
        "no payload file may be written after hash validation fails"
    );

    const auto resolved_test_root = std::filesystem::weakly_canonical(test_root);
    const auto resolved_temp =
        std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path()
        );
    require(
        resolved_test_root.string().starts_with(resolved_temp.string()),
        "test cleanup must stay inside the temporary directory"
    );
    std::filesystem::remove_all(resolved_test_root);
}

void install_discovery_uses_library_with_palworld_executable()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-discovery-test-" + unique);
    const auto first_library = test_root / "first";
    const auto second_library = test_root / "second";
    const auto expected =
        second_library / "steamapps" / "common" / "Palworld";

    std::filesystem::create_directories(
        expected / "Pal" / "Binaries" / "Win64"
    );
    {
        std::ofstream executable{
            expected / "Pal" / "Binaries" / "Win64"
                / "Palworld-Win64-Shipping.exe",
            std::ios::binary,
        };
        executable << "fixture";
    }

    const std::vector<std::filesystem::path> libraries{
        first_library,
        second_library,
    };
    const auto discovered =
        palverify::find_palworld_install(libraries);

    require(discovered.has_value(), "Palworld install must be discovered");
    require(*discovered == expected, "discovered path must use valid library");

    const auto resolved_test_root = std::filesystem::weakly_canonical(test_root);
    const auto resolved_temp =
        std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path()
        );
    require(
        resolved_test_root.string().starts_with(resolved_temp.string()),
        "discovery cleanup must stay inside the temporary directory"
    );
    std::filesystem::remove_all(resolved_test_root);
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

auto main() -> int
{
    const std::vector<TestCase> tests{
        {
            "settings update removes legacy PalVerify activation",
            settings_update_removes_legacy_palverify_activation,
        },
        {"settings update is idempotent", settings_update_is_idempotent},
        {
            "missing section is left unchanged",
            missing_section_is_left_unchanged,
        },
        {
            "Steam library parser decodes VDF paths",
            steam_library_parser_decodes_vdf_paths,
        },
        {
            "installer copies payload and backs up settings",
            installer_copies_payload_and_creates_a_settings_backup,
        },
        {
            "SHA-256 matches standard vectors",
            sha256_matches_standard_vectors,
        },
        {
            "payload archive round-trips and rejects tampering",
            payload_archive_round_trips_and_rejects_tampering,
        },
        {
            "installer rejects payload hash mismatch before writing",
            installer_rejects_hash_mismatch_before_writing_payload,
        },
        {
            "install discovery selects valid Steam library",
            install_discovery_uses_library_with_palworld_executable,
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
