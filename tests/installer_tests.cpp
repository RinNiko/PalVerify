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

[[nodiscard]] auto read_file(const std::filesystem::path& path)
    -> std::string
{
    std::ifstream input{path, std::ios::binary};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
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

void installer_isolates_managed_mods_from_external_workshop()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-statue-markers-test-" + unique);
    const auto game_root = test_root / "game";
    const auto payload_root = test_root / "payload";
    const auto workshop_root = test_root / "workshop";
    const auto ue4ss_root = workshop_root / "3625223587";
    const auto local_workshop_root = game_root / "Mods" / "Workshop";
    const auto local_ue4ss_root = local_workshop_root / "3625223587";
    const auto settings_path = game_root / "Mods" / "PalModSettings.ini";

    std::filesystem::create_directories(
        game_root / "Pal" / "Binaries" / "Win64"
    );
    std::filesystem::create_directories(payload_root / "client" / "Scripts");
    std::filesystem::create_directories(
        payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
            / "StatueMapMarkers" / "Scripts"
    );
    std::filesystem::create_directories(
        payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
            / "PalHud" / "Scripts"
    );
    std::filesystem::create_directories(
        payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
            / "PalHud" / "Assets"
    );
    std::filesystem::create_directories(ue4ss_root / "Mods");
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
        client_script << "return true";
    }
    {
        std::ofstream client_config{
            payload_root / "client" / "Scripts" / "config.json",
            std::ios::binary,
        };
        client_config << R"({"serverId":"fixture"})";
    }
    {
        std::ofstream client_agent{
            payload_root / "client" / "Scripts" / "PalVerifyClient.exe",
            std::ios::binary,
        };
        client_agent << "fixture";
    }
    {
        std::ofstream fallback_info{
            payload_root / "managed" / "UE4SSExperimentalPW" / "Info.json",
            std::ios::binary,
        };
        fallback_info
            << "{\"PackageName\":\"UE4SSExperimentalPW\"}";
    }
    {
        std::ofstream marker_script{
            payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
                / "StatueMapMarkers" / "Scripts" / "main.lua",
            std::ios::binary,
        };
        marker_script << "return true";
    }
    {
        std::ofstream marker_enabled{
            payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
                / "StatueMapMarkers" / "enabled.txt",
            std::ios::binary,
        };
    }
    {
        std::ofstream hud_script{
            payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
                / "PalHud" / "Scripts" / "main.lua",
            std::ios::binary,
        };
        hud_script << "return true";
    }
    {
        std::ofstream hud_logo{
            payload_root / "managed" / "UE4SSExperimentalPW" / "Mods"
                / "PalHud" / "Assets" / "logo-wordmark-hud.png",
            std::ios::binary,
        };
        hud_logo << "png-fixture";
    }
    {
        std::ofstream external_info{
            ue4ss_root / "Info.json",
            std::ios::binary,
        };
        external_info
            << "{\"PackageName\":\"UE4SSExperimentalPW\"}";
    }
    {
        std::ofstream mods_json{
            ue4ss_root / "Mods" / "mods.json",
            std::ios::binary,
        };
        mods_json
            << "[\n"
               "  {\"mod_name\":\"ExistingMod\",\"mod_enabled\":true}\n"
               "]\n";
    }
    {
        std::ofstream mods_txt{
            ue4ss_root / "Mods" / "mods.txt",
            std::ios::binary,
        };
        mods_txt << "ExistingMod : 1\n";
    }
    {
        std::ofstream settings{settings_path, std::ios::binary};
        settings
            << "[PalModSettings]\r\n"
               "bGlobalEnableMod=False\r\n"
               "WorkshopRootDir="
            << workshop_root.string()
            << "\r\nConfigVersion=1.0\r\n";
    }

    const auto result =
        palverify::install_palverify_payload(game_root, payload_root);

    require(result.success, "managed StatueMapMarkers payload must install");
    require(
        std::filesystem::is_regular_file(
            local_ue4ss_root / "Mods" / "StatueMapMarkers" / "Scripts"
                / "main.lua"
        ),
        "StatueMapMarkers must be copied into launcher-managed Workshop"
    );
    require(
        std::filesystem::is_regular_file(
            local_ue4ss_root / "Mods" / "PalHud" / "Scripts" / "main.lua"
        ),
        "PalHud must be copied into launcher-managed Workshop"
    );
    require(
        std::filesystem::is_regular_file(
            local_ue4ss_root / "Mods" / "PalHud" / "Assets"
                / "logo-wordmark-hud.png"
        ),
        "PalHud background logo must be installed with the client HUD"
    );
    require(
        !std::filesystem::exists(
            ue4ss_root / "Mods" / "StatueMapMarkers"
        ),
        "launcher must not mutate the player's external Steam Workshop"
    );
    require(
        !std::filesystem::exists(
            game_root / "Mods" / "Workshop" / "PalVerify" / "managed"
        ),
        "managed files must not be copied into the PalVerify package"
    );

    std::ifstream json_stream{
        local_ue4ss_root / "Mods" / "mods.json",
        std::ios::binary,
    };
    const std::string mods_json{
        std::istreambuf_iterator<char>{json_stream},
        std::istreambuf_iterator<char>{},
    };
    json_stream.close();
    require_contains(
        mods_json,
        "\"mod_name\": \"StatueMapMarkers\"",
        "StatueMapMarkers must be added to mods.json"
    );
    require_contains(
        mods_json,
        "\"mod_name\": \"PalHud\"",
        "PalHud must be added to mods.json"
    );
    require_contains(
        mods_json,
        "\"mod_enabled\": true",
        "StatueMapMarkers must be enabled in mods.json"
    );

    std::ifstream text_stream{
        local_ue4ss_root / "Mods" / "mods.txt",
        std::ios::binary,
    };
    const std::string mods_txt{
        std::istreambuf_iterator<char>{text_stream},
        std::istreambuf_iterator<char>{},
    };
    text_stream.close();
    require_contains(
        mods_txt,
        "StatueMapMarkers : 1",
        "StatueMapMarkers must be enabled in mods.txt"
    );
    require_contains(
        mods_txt,
        "PalHud : 1",
        "PalHud must be enabled in mods.txt"
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
        "managed game mods require the global mod loader"
    );
    require_contains(
        installed_settings,
        "ActiveModList=UE4SSExperimentalPW\r\n",
        "managed StatueMapMarkers requires the UE4SS package to be active"
    );
    require_contains(
        installed_settings,
        "WorkshopRootDir=" + local_workshop_root.string() + "\r\n",
        "launcher-managed Workshop must replace the external Steam root"
    );
    require_not_contains(
        installed_settings,
        workshop_root.string(),
        "external Steam subscriptions must not enter the server profile"
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

void installer_points_clean_machine_at_local_ue4ss_fallback()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-local-ue4ss-test-" + unique);
    const auto game_root = test_root / "game";
    const auto settings_path = game_root / "Mods" / "PalModSettings.ini";
    const auto stale_workshop_root = test_root / "missing-workshop";
    const auto local_workshop_root =
        game_root / "Mods" / "Workshop";
    const auto active_ue4ss_mods =
        game_root / "Mods" / "NativeMods" / "UE4SS" / "Mods";
    const auto active_watchdog_root =
        active_ue4ss_mods / "PalVerify" / "Scripts";
    const auto active_palhud_root =
        active_ue4ss_mods / "PalHud";

    std::filesystem::create_directories(
        game_root / "Pal" / "Binaries" / "Win64"
    );
    std::filesystem::create_directories(settings_path.parent_path());
    std::filesystem::create_directories(active_watchdog_root);
    std::filesystem::create_directories(
        active_palhud_root / "Scripts"
    );
    std::filesystem::create_directories(
        active_palhud_root / "Assets"
    );
    {
        std::ofstream stale_script{
            active_watchdog_root / "main.lua",
            std::ios::binary,
        };
        stale_script
            << "os.execute('PalVerifyClient.exe')\n"
               "LoopAsync(5000, function() end)\n";
    }
    {
        std::ofstream stale_palhud{
            active_palhud_root / "Scripts" / "main.lua",
            std::ios::binary,
        };
        stale_palhud << "local VERSION = \"1.4.0\"\n";
    }
    {
        std::ofstream stale_logo{
            active_palhud_root / "Assets" / "logo-wordmark-hud.png",
            std::ios::binary,
        };
        stale_logo << "stale-png";
    }
    {
        std::ofstream active_mods_json{
            active_ue4ss_mods / "mods.json",
            std::ios::binary,
        };
        active_mods_json
            << "[{\"mod_name\":\"PalHud\",\"mod_enabled\":false}]";
    }
    {
        std::ofstream active_mods_text{
            active_ue4ss_mods / "mods.txt",
            std::ios::binary,
        };
        active_mods_text << "PalHud : 0\n";
    }
    {
        std::ofstream executable{
            game_root / "Pal" / "Binaries" / "Win64"
                / "Palworld-Win64-Shipping.exe",
            std::ios::binary,
        };
        executable << "fixture";
    }
    {
        std::ofstream settings{settings_path, std::ios::binary};
        settings
            << "[PalModSettings]\r\n"
               "bGlobalEnableMod=True\r\n"
               "WorkshopRootDir="
            << stale_workshop_root.string()
            << "\r\nConfigVersion=1.0\r\n";
    }

    const auto payload_file = [](std::filesystem::path path,
                                 std::string_view text) {
        auto content = bytes(text);
        auto digest = palverify::sha256_hex(content);
        return palverify::PayloadFile{
            .relative_path = std::move(path),
            .content = std::move(content),
            .sha256 = std::move(digest),
        };
    };
    const std::vector<palverify::PayloadFile> payload{
        payload_file(
            "Info.json",
            R"({"PackageName":"PalVerify"})"
        ),
        payload_file("client/Scripts/main.lua", "return true"),
        payload_file(
            "client/Scripts/config.json",
            R"({"serverId":"fixture"})"
        ),
        payload_file(
            "client/Scripts/PalVerifyClient.exe",
            "client-fixture"
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/Info.json",
            R"({"PackageName":"UE4SSExperimentalPW"})"
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/UE4SS.dll",
            "ue4ss-fixture"
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/Mods/StatueMapMarkers/"
            "Scripts/main.lua",
            "return true"
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/Mods/StatueMapMarkers/"
            "enabled.txt",
            ""
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/Mods/PalHud/Scripts/main.lua",
            "return true"
        ),
        payload_file(
            "managed/UE4SSExperimentalPW/Mods/PalHud/Assets/"
            "logo-wordmark-hud.png",
            "png-fixture"
        ),
    };

    const auto result = palverify::install_palverify_payload(
        game_root,
        std::span<const palverify::PayloadFile>{payload}
    );

    require(result.success, "local managed UE4SS fallback must install");
    require(
        std::filesystem::is_regular_file(
            local_workshop_root / "3625223587"
                / "Mods" / "StatueMapMarkers" / "Scripts" / "main.lua"
        ),
        "clean machines must receive the local StatueMapMarkers payload"
    );
    require(
        std::filesystem::is_regular_file(
            local_workshop_root / "3625223587"
                / "Mods" / "PalHud" / "Assets" / "logo-wordmark-hud.png"
        ),
        "clean machines must receive the PalHud client assets"
    );
    const auto watchdog_root =
        local_workshop_root / "3625223587" / "Mods" / "PalVerify"
        / "Scripts";
    require(
        std::filesystem::is_regular_file(watchdog_root / "main.lua"),
        "clean machines must load the PalVerify watchdog through UE4SS"
    );
    require(
        std::filesystem::is_regular_file(
            watchdog_root / "PalVerifyClient.exe"
        ),
        "the UE4SS watchdog must have an executable beside its script"
    );
    require(
        std::filesystem::is_regular_file(watchdog_root / "config.json"),
        "the supervised client must retain its coordinator config"
    );
    std::ifstream active_script_stream{
        active_watchdog_root / "main.lua",
        std::ios::binary,
    };
    const std::string active_script{
        std::istreambuf_iterator<char>{active_script_stream},
        std::istreambuf_iterator<char>{},
    };
    active_script_stream.close();
    require(
        active_script == "return true",
        "installer must replace a stale active NativeMods watchdog"
    );
    require(
        read_file(active_palhud_root / "Scripts" / "main.lua")
            == "return true",
        "installer must replace a stale active NativeMods PalHud script"
    );
    require(
        read_file(
            active_palhud_root / "Assets" / "logo-wordmark-hud.png"
        ) == "png-fixture",
        "installer must replace stale active NativeMods PalHud assets"
    );
    require_contains(
        read_file(active_ue4ss_mods / "mods.json"),
        R"("mod_name":"PalHud","mod_enabled":true)",
        "the active NativeMods runtime must enable PalHud in mods.json"
    );
    require_contains(
        read_file(active_ue4ss_mods / "mods.txt"),
        "PalHud : 1",
        "the active NativeMods runtime must enable PalHud in mods.txt"
    );
    std::ifstream mods_text_stream{
        local_workshop_root / "3625223587" / "Mods" / "mods.txt",
        std::ios::binary,
    };
    const std::string mods_text{
        std::istreambuf_iterator<char>{mods_text_stream},
        std::istreambuf_iterator<char>{},
    };
    mods_text_stream.close();
    require_contains(
        mods_text,
        "PalVerify : 1",
        "the managed UE4SS runtime must enable the PalVerify watchdog"
    );
    require_contains(
        mods_text,
        "PalHud : 1",
        "the managed UE4SS runtime must enable PalHud"
    );

    std::ifstream settings_stream{settings_path, std::ios::binary};
    const std::string installed_settings{
        std::istreambuf_iterator<char>{settings_stream},
        std::istreambuf_iterator<char>{},
    };
    settings_stream.close();
    require_contains(
        installed_settings,
        "WorkshopRootDir=" + local_workshop_root.string() + "\r\n",
        "local fallback must become Palworld's active Workshop root"
    );
    require_not_contains(
        installed_settings,
        stale_workshop_root.string(),
        "an unavailable Workshop root must not survive local fallback"
    );

    const auto resolved_test_root =
        std::filesystem::weakly_canonical(test_root);
    const auto resolved_temp =
        std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path()
        );
    require(
        resolved_test_root.string().starts_with(resolved_temp.string()),
        "local fallback cleanup must stay inside the temporary directory"
    );
    std::filesystem::remove_all(resolved_test_root);
}

void unapproved_mods_are_quarantined_without_touching_allowed_mods()
{
    const auto unique =
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    const auto test_root =
        std::filesystem::temp_directory_path()
        / ("palverify-quarantine-test-" + unique);
    const auto game_root = test_root / "game";
    const auto workshop_root = game_root / "Mods" / "Workshop";
    const auto ue4ss_root = workshop_root / "3625223587";
    const auto paks_root =
        game_root / "Pal" / "Content" / "Paks" / "~mods";
    std::filesystem::create_directories(
        game_root / "Pal" / "Binaries" / "Win64"
    );
    std::filesystem::create_directories(
        workshop_root / "MapUnlocker"
    );
    std::filesystem::create_directories(
        workshop_root / "AllowedPackage"
    );
    std::filesystem::create_directories(
        ue4ss_root / "Mods" / "BadLua"
    );
    std::filesystem::create_directories(
        ue4ss_root / "Mods" / "StatueMapMarkers"
    );
    std::filesystem::create_directories(
        ue4ss_root / "Mods" / "PalHud"
    );
    std::filesystem::create_directories(paks_root);
    {
        std::ofstream executable{
            game_root / "Pal" / "Binaries" / "Win64"
                / "Palworld-Win64-Shipping.exe",
            std::ios::binary,
        };
        executable << "fixture";
    }
    {
        std::ofstream info{
            workshop_root / "MapUnlocker" / "Info.json",
            std::ios::binary,
        };
        info << R"({"PackageName":"MapUnlocker"})";
    }
    {
        std::ofstream info{
            workshop_root / "AllowedPackage" / "Info.json",
            std::ios::binary,
        };
        info << R"({"PackageName":"AllowedPackage"})";
    }
    {
        std::ofstream info{ue4ss_root / "Info.json", std::ios::binary};
        info << R"({"PackageName":"UE4SSExperimentalPW"})";
    }
    {
        std::ofstream enabled{
            ue4ss_root / "Mods" / "BadLua" / "enabled.txt",
            std::ios::binary,
        };
    }
    {
        std::ofstream enabled{
            ue4ss_root / "Mods" / "StatueMapMarkers" / "enabled.txt",
            std::ios::binary,
        };
    }
    {
        std::ofstream enabled{
            ue4ss_root / "Mods" / "PalHud" / "enabled.txt",
            std::ios::binary,
        };
    }
    {
        std::ofstream pak{
            paks_root / "CheatMap.pak",
            std::ios::binary,
        };
        pak << "fixture";
    }

    const std::vector<std::string> rejected{
        "MapUnlocker",
        "BadLua",
        "legacy-pak:CheatMap.pak",
        "StatueMapMarkers",
        "PalHud",
    };
    const auto result = palverify::quarantine_unapproved_mods(
        game_root,
        std::span<const std::string>{rejected}
    );

    require(result.success, "exact rejected mods must be quarantined");
    require(
        result.quarantined == 3,
        "direct, nested and legacy rejected mods must be counted"
    );
    require(
        !std::filesystem::exists(workshop_root / "MapUnlocker"),
        "rejected Workshop package must leave the active root"
    );
    require(
        !std::filesystem::exists(ue4ss_root / "Mods" / "BadLua"),
        "rejected UE4SS mod must leave the active package"
    );
    require(
        !std::filesystem::exists(paks_root / "CheatMap.pak"),
        "rejected legacy pak must leave the active directory"
    );
    require(
        std::filesystem::is_regular_file(
            workshop_root / "AllowedPackage" / "Info.json"
        ),
        "allowed Workshop packages must remain untouched"
    );
    require(
        std::filesystem::is_regular_file(
            ue4ss_root / "Mods" / "StatueMapMarkers" / "enabled.txt"
        ),
        "launcher-managed mods must never be quarantined"
    );
    require(
        std::filesystem::is_regular_file(
            ue4ss_root / "Mods" / "PalHud" / "enabled.txt"
        ),
        "PalHud must never be quarantined as an unapproved player mod"
    );

    std::size_t quarantined_files = 0;
    const auto quarantine_root =
        game_root / "Mods" / ".pal3mien-quarantine";
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{quarantine_root}) {
        quarantined_files += entry.is_regular_file() ? 1U : 0U;
    }
    require(
        quarantined_files >= 3,
        "quarantine must retain recoverable copies of removed mod files"
    );

    const auto resolved_test_root =
        std::filesystem::weakly_canonical(test_root);
    const auto resolved_temp =
        std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path()
        );
    require(
        resolved_test_root.string().starts_with(resolved_temp.string()),
        "quarantine test cleanup must stay inside the temporary directory"
    );
    std::filesystem::remove_all(resolved_test_root);
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
            "installer isolates managed mods from external Workshop",
            installer_isolates_managed_mods_from_external_workshop,
        },
        {
            "installer activates local UE4SS fallback on clean machines",
            installer_points_clean_machine_at_local_ue4ss_fallback,
        },
        {
            "unapproved mods are quarantined safely",
            unapproved_mods_are_quarantined_without_touching_allowed_mods,
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
