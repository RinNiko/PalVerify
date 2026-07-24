#include "palverify/installer_settings.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
    std::optional<std::filesystem::path> game_root;
    bool install_only{false};
    bool silent{false};
};

[[nodiscard]] auto module_directory() -> std::filesystem::path
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    buffer.resize(length);
    return std::filesystem::path{buffer}.parent_path();
}

[[nodiscard]] auto parse_arguments() -> Arguments
{
    int count = 0;
    auto** values = CommandLineToArgvW(GetCommandLineW(), &count);
    Arguments arguments;
    if (values == nullptr) {
        return arguments;
    }

    for (int index = 1; index < count; ++index) {
        const std::wstring_view value{values[index]};
        if (value == L"--install-only") {
            arguments.install_only = true;
        } else if (value == L"--silent") {
            arguments.silent = true;
        } else if (value == L"--game-root" && index + 1 < count) {
            arguments.game_root = values[++index];
        }
    }
    LocalFree(values);
    return arguments;
}

[[nodiscard]] auto steam_root_from_registry()
    -> std::optional<std::filesystem::path>
{
    DWORD bytes = 0;
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam",
            L"SteamPath",
            RRF_RT_REG_SZ,
            nullptr,
            nullptr,
            &bytes
        )
        != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam",
            L"SteamPath",
            RRF_RT_REG_SZ,
            nullptr,
            value.data(),
            &bytes
        )
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen_s(value.data(), value.size()));
    return std::filesystem::path{value};
}

[[nodiscard]] auto discover_palworld_install()
    -> std::optional<std::filesystem::path>
{
    const auto steam_root = steam_root_from_registry();
    if (!steam_root.has_value()) {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> libraries{*steam_root};
    const auto library_file =
        *steam_root / "steamapps" / "libraryfolders.vdf";
    std::ifstream input{library_file, std::ios::binary};
    if (input) {
        const std::string vdf{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{},
        };
        auto parsed = palverify::extract_steam_library_paths(vdf);
        libraries.insert(libraries.end(), parsed.begin(), parsed.end());
    }

    std::ranges::sort(libraries);
    const auto unique_end = std::ranges::unique(libraries).begin();
    libraries.erase(unique_end, libraries.end());
    return palverify::find_palworld_install(libraries);
}

void show_result(bool silent, UINT flags, const wchar_t* message)
{
    if (!silent) {
        MessageBoxW(nullptr, message, L"PalVerify", flags);
    }
}

[[nodiscard]] auto start_client_agent(
    const std::filesystem::path& game_root
) -> bool
{
    const auto executable =
        game_root / "Mods" / "Workshop" / "PalVerify" / "client"
        / "Scripts" / "PalVerifyClient.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto working_directory = executable.parent_path().wstring();
    const auto created = CreateProcessW(
        executable.c_str(),
        nullptr,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process
    );
    if (created == FALSE) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace

auto WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int
) -> int
{
    const auto arguments = parse_arguments();
    const auto game_root = arguments.game_root.has_value()
        ? arguments.game_root
        : discover_palworld_install();
    if (!game_root.has_value()) {
        show_result(
            arguments.silent,
            MB_OK | MB_ICONERROR,
            L"Khong tim thay Palworld Steam tren may nay."
        );
        return 2;
    }

    const auto payload_root = module_directory() / "payload";
    const auto result =
        palverify::install_palverify_payload(*game_root, payload_root);
    if (!result.success) {
        show_result(
            arguments.silent,
            MB_OK | MB_ICONERROR,
            L"Khong the cai PalVerify. Hay kiem tra quyen ghi va payload."
        );
        return 3;
    }

    if (!arguments.install_only) {
        if (!start_client_agent(*game_root)) {
            show_result(
                arguments.silent,
                MB_OK | MB_ICONERROR,
                L"Khong the khoi dong PalVerifyClient.exe."
            );
            return 4;
        }
        ShellExecuteW(
            nullptr,
            L"open",
            L"steam://rungameid/1623730",
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
    }
    show_result(
        arguments.silent,
        MB_OK | MB_ICONINFORMATION,
        L"Da cai PalVerify thanh cong."
    );
    return 0;
}
