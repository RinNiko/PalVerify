#include "palverify/installer_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace palverify {
namespace {

[[nodiscard]] auto trim(std::string_view value) -> std::string_view
{
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] auto ascii_equals_ignore_case(
    std::string_view left,
    std::string_view right
) -> bool
{
    return left.size() == right.size()
        && std::ranges::equal(
            left,
            right,
            [](char left_value, char right_value) {
                return std::tolower(
                           static_cast<unsigned char>(left_value)
                       )
                    == std::tolower(
                           static_cast<unsigned char>(right_value)
                       );
            }
        );
}

[[nodiscard]] auto split_lines(std::string_view text)
    -> std::vector<std::string>
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto length =
            end == std::string_view::npos ? text.size() - start : end - start;
        auto line = std::string{text.substr(start, length)};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] auto section_name(std::string_view line)
    -> std::string_view
{
    auto cleaned = trim(line);
    if (cleaned.size() < 2 || cleaned.front() != '['
        || cleaned.back() != ']') {
        return {};
    }
    cleaned.remove_prefix(1);
    cleaned.remove_suffix(1);
    return trim(cleaned);
}

[[nodiscard]] auto key_value(std::string_view line)
    -> std::pair<std::string_view, std::string_view>
{
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) {
        return {};
    }
    return {
        trim(line.substr(0, equals)),
        trim(line.substr(equals + 1)),
    };
}

}  // namespace

auto enable_palverify_mod(std::string_view existing_settings) -> std::string
{
    const auto newline =
        existing_settings.find("\r\n") != std::string_view::npos ? "\r\n"
                                                                 : "\n";
    auto lines = split_lines(existing_settings);

    auto section_start = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (ascii_equals_ignore_case(
                section_name(lines[index]),
                "PalModSettings"
            )) {
            section_start = index;
            break;
        }
    }

    if (section_start == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[PalModSettings]");
        lines.emplace_back("bGlobalEnableMod=True");
        lines.emplace_back("ActiveModList=PalVerify");
    } else {
        auto section_end = lines.size();
        for (std::size_t index = section_start + 1; index < lines.size();
             ++index) {
            if (!section_name(lines[index]).empty()) {
                section_end = index;
                break;
            }
        }

        auto global_index = lines.size();
        bool palverify_active = false;
        for (std::size_t index = section_start + 1; index < section_end;
             ++index) {
            const auto [key, value] = key_value(lines[index]);
            if (ascii_equals_ignore_case(key, "bGlobalEnableMod")) {
                global_index = index;
            } else if (
                ascii_equals_ignore_case(key, "ActiveModList")
                && ascii_equals_ignore_case(value, "PalVerify")
            ) {
                palverify_active = true;
            }
        }

        if (global_index == lines.size()) {
            global_index = section_start + 1;
            lines.insert(
                lines.begin() + static_cast<std::ptrdiff_t>(global_index),
                "bGlobalEnableMod=True"
            );
        } else {
            lines[global_index] = "bGlobalEnableMod=True";
        }

        if (!palverify_active) {
            lines.insert(
                lines.begin()
                    + static_cast<std::ptrdiff_t>(global_index + 1),
                "ActiveModList=PalVerify"
            );
        }
    }

    std::string updated;
    for (const auto& line : lines) {
        updated += line;
        updated += newline;
    }
    return updated;
}

auto extract_steam_library_paths(std::string_view vdf)
    -> std::vector<std::filesystem::path>
{
    static const std::regex path_pattern{
        R"regex("path"\s*"([^"]+)")regex",
        std::regex::ECMAScript | std::regex::icase,
    };

    const std::string source{vdf};
    std::vector<std::filesystem::path> paths;
    for (
        auto match =
            std::sregex_iterator{source.begin(), source.end(), path_pattern};
        match != std::sregex_iterator{};
        ++match
    ) {
        auto encoded = (*match)[1].str();
        std::string decoded;
        decoded.reserve(encoded.size());
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            if (encoded[index] == '\\' && index + 1 < encoded.size()
                && encoded[index + 1] == '\\') {
                ++index;
            }
            decoded.push_back(encoded[index]);
        }
        paths.emplace_back(std::move(decoded));
    }
    return paths;
}

auto find_palworld_install(
    const std::vector<std::filesystem::path>& steam_library_roots
) -> std::optional<std::filesystem::path>
{
    for (const auto& library_root : steam_library_roots) {
        const auto game_root =
            library_root / "steamapps" / "common" / "Palworld";
        const auto executable =
            game_root / "Pal" / "Binaries" / "Win64"
            / "Palworld-Win64-Shipping.exe";
        if (std::filesystem::is_regular_file(executable)) {
            return game_root;
        }
    }
    return std::nullopt;
}

auto install_palverify_payload(
    const std::filesystem::path& game_root,
    const std::filesystem::path& payload_root
) -> InstallResult
{
    const auto game_executable =
        game_root / "Pal" / "Binaries" / "Win64"
        / "Palworld-Win64-Shipping.exe";
    const auto info_path = payload_root / "Info.json";
    const auto client_script =
        payload_root / "client" / "Scripts" / "main.lua";
    const auto client_agent =
        payload_root / "client" / "Scripts" / "PalVerifyClient.exe";

    if (!std::filesystem::is_regular_file(game_executable)) {
        return {.success = false, .detail = "invalid-game-root"};
    }
    if (!std::filesystem::is_regular_file(info_path)
        || !std::filesystem::is_regular_file(client_script)
        || !std::filesystem::is_regular_file(client_agent)) {
        return {.success = false, .detail = "incomplete-payload"};
    }

    try {
        const auto mods_root = game_root / "Mods";
        const auto package_target =
            mods_root / "Workshop" / "PalVerify";
        const auto settings_path = mods_root / "PalModSettings.ini";
        const auto backup_path =
            mods_root / "PalModSettings.ini.palverify-backup";
        const auto temporary_path =
            mods_root / "PalModSettings.ini.palverify-tmp";

        std::filesystem::create_directories(package_target);
        std::filesystem::copy(
            payload_root,
            package_target,
            std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::overwrite_existing
                | std::filesystem::copy_options::copy_symlinks
        );

        std::string existing_settings;
        if (std::filesystem::is_regular_file(settings_path)) {
            std::ifstream input{settings_path, std::ios::binary};
            existing_settings.assign(
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{}
            );
            if (!std::filesystem::exists(backup_path)) {
                std::filesystem::copy_file(settings_path, backup_path);
            }
        }

        const auto updated_settings =
            enable_palverify_mod(existing_settings);
        {
            std::ofstream output{
                temporary_path,
                std::ios::binary | std::ios::trunc,
            };
            output.write(
                updated_settings.data(),
                static_cast<std::streamsize>(updated_settings.size())
            );
            if (!output) {
                return {.success = false, .detail = "settings-write-failed"};
            }
        }
        std::filesystem::copy_file(
            temporary_path,
            settings_path,
            std::filesystem::copy_options::overwrite_existing
        );
        std::filesystem::remove(temporary_path);
    } catch (const std::filesystem::filesystem_error& error) {
        return {.success = false, .detail = error.code().message()};
    }

    return {.success = true, .detail = "installed"};
}

}  // namespace palverify
