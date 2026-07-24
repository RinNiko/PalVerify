#include "palverify/installer_settings.hpp"

#include <chrono>
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

void require_contains(
    std::string_view text,
    std::string_view expected,
    std::string_view message
)
{
    require(text.find(expected) != std::string_view::npos, message);
}

void settings_update_preserves_existing_values()
{
    constexpr std::string_view existing =
        "; custom comment\r\n"
        "[PalModSettings]\r\n"
        "bGlobalEnableMod=False\r\n"
        "WorkshopRootDir=E:\\SteamLibrary\\steamapps\\workshop\\content\\1623730\r\n"
        "ActiveModList=ExistingMod\r\n"
        "ConfigVersion=1.0\r\n";

    const auto updated = palverify::enable_palverify_mod(existing);

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
        "bGlobalEnableMod=True\r\n",
        "global mod loading must be enabled"
    );
    require_contains(
        updated,
        "ActiveModList=PalVerify\r\n",
        "PalVerify must be activated"
    );
}

void settings_update_is_idempotent()
{
    constexpr std::string_view existing =
        "[PalModSettings]\n"
        "bGlobalEnableMod=True\n"
        "ActiveModList=PalVerify\n"
        "ConfigVersion=1.0\n";

    const auto once = palverify::enable_palverify_mod(existing);
    const auto twice = palverify::enable_palverify_mod(once);

    require(once == twice, "repeated install must not change settings");
    const auto first = once.find("ActiveModList=PalVerify");
    require(first != std::string::npos, "PalVerify entry must exist");
    require(
        once.find("ActiveModList=PalVerify", first + 1) == std::string::npos,
        "PalVerify entry must not be duplicated"
    );
}

void missing_section_is_created_without_discarding_input()
{
    constexpr std::string_view existing = "; keep me\nOtherSetting=42\n";

    const auto updated = palverify::enable_palverify_mod(existing);

    require_contains(updated, "; keep me\n", "existing input must survive");
    require_contains(
        updated,
        "[PalModSettings]\n"
        "bGlobalEnableMod=True\n"
        "ActiveModList=PalVerify\n",
        "missing section must be appended"
    );
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
        "bGlobalEnableMod=True\r\n",
        "installed settings must enable mods"
    );
    require_contains(
        installed_settings,
        "ActiveModList=PalVerify\r\n",
        "installed settings must activate PalVerify"
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
            "settings update preserves existing values",
            settings_update_preserves_existing_values,
        },
        {"settings update is idempotent", settings_update_is_idempotent},
        {
            "missing section is created without discarding input",
            missing_section_is_created_without_discarding_input,
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
