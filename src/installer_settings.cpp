#include "palverify/installer_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
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

[[nodiscard]] auto safe_payload_path(
    const std::filesystem::path& relative_path
) -> bool
{
    if (relative_path.empty() || relative_path.is_absolute()
        || relative_path.has_root_name()
        || relative_path.has_root_directory()) {
        return false;
    }
    for (const auto& component : relative_path) {
        if (component.empty() || component == "."
            || component == "..") {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto required_payload_files_present(
    std::span<const PayloadFile> files
) -> bool
{
    const std::set<std::string> paths = [&files] {
        std::set<std::string> values;
        for (const auto& file : files) {
            values.insert(file.relative_path.generic_string());
        }
        return values;
    }();
    return paths.contains("Info.json")
        && paths.contains("client/Scripts/main.lua")
        && paths.contains("client/Scripts/PalVerifyClient.exe");
}

[[nodiscard]] auto update_palmod_settings(
    const std::filesystem::path& mods_root
) -> InstallResult
{
    const auto settings_path = mods_root / "PalModSettings.ini";
    const auto backup_path =
        mods_root / "PalModSettings.ini.palverify-backup";
    const auto temporary_path =
        mods_root / "PalModSettings.ini.palverify-tmp";

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
        remove_palverify_game_mod_activation(existing_settings);
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
    return {.success = true, .detail = "settings-updated"};
}

}  // namespace

auto remove_palverify_game_mod_activation(
    std::string_view existing_settings
) -> std::string
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
        return std::string{existing_settings};
    }

    auto section_end = lines.size();
    for (std::size_t index = section_start + 1; index < lines.size();
         ++index) {
        if (!section_name(lines[index]).empty()) {
            section_end = index;
            break;
        }
    }
    lines.erase(
        std::remove_if(
            lines.begin() + static_cast<std::ptrdiff_t>(section_start + 1),
            lines.begin() + static_cast<std::ptrdiff_t>(section_end),
            [](const std::string& line) {
                const auto [key, value] = key_value(line);
                return ascii_equals_ignore_case(key, "ActiveModList")
                    && ascii_equals_ignore_case(value, "PalVerify");
            }
        ),
        lines.begin() + static_cast<std::ptrdiff_t>(section_end)
    );

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
    try {
        std::vector<PayloadFile> files;
        if (!std::filesystem::is_directory(payload_root)) {
            return {.success = false, .detail = "incomplete-payload"};
        }
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator{payload_root}) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream input{entry.path(), std::ios::binary};
            std::vector<char> raw{
                std::istreambuf_iterator<char>{input},
                std::istreambuf_iterator<char>{},
            };
            std::vector<std::byte> content(raw.size());
            std::ranges::transform(
                raw,
                content.begin(),
                [](char value) {
                    return static_cast<std::byte>(
                        static_cast<unsigned char>(value)
                    );
                }
            );
            files.push_back({
                .relative_path =
                    std::filesystem::relative(entry.path(), payload_root),
                .content = std::move(content),
                .sha256 = {},
            });
            files.back().sha256 = sha256_hex(files.back().content);
        }
        return install_palverify_payload(game_root, files);
    } catch (const std::filesystem::filesystem_error& error) {
        return {.success = false, .detail = error.code().message()};
    }
}

auto install_palverify_payload(
    const std::filesystem::path& game_root,
    std::span<const PayloadFile> files
) -> InstallResult
{
    const auto game_executable =
        game_root / "Pal" / "Binaries" / "Win64"
        / "Palworld-Win64-Shipping.exe";
    if (!std::filesystem::is_regular_file(game_executable)) {
        return {.success = false, .detail = "invalid-game-root"};
    }
    if (!required_payload_files_present(files)) {
        return {.success = false, .detail = "incomplete-payload"};
    }

    std::set<std::string> unique_paths;
    for (const auto& file : files) {
        const auto normalized = file.relative_path.lexically_normal();
        const auto path_key = normalized.generic_string();
        if (!safe_payload_path(normalized)
            || !unique_paths.insert(path_key).second) {
            return {.success = false, .detail = "invalid-payload-path"};
        }
        if (file.sha256.size() != 64
            || sha256_hex(file.content) != file.sha256) {
            return {.success = false, .detail = "payload-hash-mismatch"};
        }
    }

    try {
        const auto mods_root = game_root / "Mods";
        const auto package_target =
            mods_root / "Workshop" / "PalVerify";
        std::filesystem::create_directories(package_target);
        for (const auto& file : files) {
            const auto target =
                package_target / file.relative_path.lexically_normal();
            std::filesystem::create_directories(target.parent_path());
            const auto temporary =
                std::filesystem::path{target.wstring() + L".palverify-tmp"};
            {
                std::ofstream output{
                    temporary,
                    std::ios::binary | std::ios::trunc,
                };
                output.write(
                    reinterpret_cast<const char*>(file.content.data()),
                    static_cast<std::streamsize>(file.content.size())
                );
                if (!output) {
                    return {
                        .success = false,
                        .detail = "payload-write-failed",
                    };
                }
            }
            std::filesystem::copy_file(
                temporary,
                target,
                std::filesystem::copy_options::overwrite_existing
            );
            std::filesystem::remove(temporary);
        }
        const auto settings = update_palmod_settings(mods_root);
        if (!settings.success) {
            return settings;
        }
    } catch (const std::filesystem::filesystem_error& error) {
        return {.success = false, .detail = error.code().message()};
    }
    return {.success = true, .detail = "installed"};
}

}  // namespace palverify
