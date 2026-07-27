#include "palverify/installer_settings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

[[nodiscard]] auto write_file_atomically(
    const std::filesystem::path& target,
    std::string_view content,
    std::string_view temporary_suffix
) -> bool
{
    std::filesystem::create_directories(target.parent_path());
    const auto temporary =
        std::filesystem::path{
            target.wstring()
            + std::wstring{
                temporary_suffix.begin(),
                temporary_suffix.end(),
            }
        };
    {
        std::ofstream output{
            temporary,
            std::ios::binary | std::ios::trunc,
        };
        output.write(
            content.data(),
            static_cast<std::streamsize>(content.size())
        );
        if (!output) {
            return false;
        }
    }
    std::filesystem::copy_file(
        temporary,
        target,
        std::filesystem::copy_options::overwrite_existing
    );
    std::filesystem::remove(temporary);
    return true;
}

[[nodiscard]] auto write_payload_file(
    const std::filesystem::path& target,
    const PayloadFile& file
) -> bool
{
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
            return false;
        }
    }
    std::filesystem::copy_file(
        temporary,
        target,
        std::filesystem::copy_options::overwrite_existing
    );
    std::filesystem::remove(temporary);
    return true;
}

[[nodiscard]] auto managed_ue4ss_relative_path(
    const std::filesystem::path& path
) -> std::optional<std::filesystem::path>
{
    const auto generic = path.generic_string();
    constexpr std::string_view prefix = "managed/UE4SSExperimentalPW/";
    if (!generic.starts_with(prefix) || generic.size() == prefix.size()) {
        return std::nullopt;
    }
    return std::filesystem::path{
        generic.substr(prefix.size())
    }.lexically_normal();
}

[[nodiscard]] auto is_ue4ss_package(
    const std::filesystem::path& package_root
) -> bool
{
    const auto info_path = package_root / "Info.json";
    if (!std::filesystem::is_regular_file(info_path)) {
        return false;
    }
    static const std::regex package_pattern{
        R"regex("PackageName"\s*:\s*"UE4SSExperimentalPW")regex",
        std::regex::ECMAScript,
    };
    return std::regex_search(read_file(info_path), package_pattern);
}

[[nodiscard]] auto enable_global_mod_loading(
    std::string_view existing_settings,
    const std::optional<std::filesystem::path>& workshop_root_override
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
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[PalModSettings]");
        lines.emplace_back("bGlobalEnableMod=True");
        if (workshop_root_override.has_value()) {
            lines.emplace_back(
                "WorkshopRootDir=" + workshop_root_override->string()
            );
        }
        lines.emplace_back("ActiveModList=UE4SSExperimentalPW");
        lines.emplace_back("ConfigVersion=1.0");
    } else {
        auto section_end = lines.size();
        for (std::size_t index = section_start + 1; index < lines.size();
             ++index) {
            if (!section_name(lines[index]).empty()) {
                section_end = index;
                break;
            }
        }
        bool found_global_enable = false;
        bool found_workshop_root = false;
        bool found_ue4ss_activation = false;
        for (std::size_t index = section_start + 1; index < section_end;
             ++index) {
            const auto [key, value] = key_value(lines[index]);
            if (ascii_equals_ignore_case(key, "bGlobalEnableMod")) {
                lines[index] = "bGlobalEnableMod=True";
                found_global_enable = true;
            }
            if (ascii_equals_ignore_case(key, "WorkshopRootDir")) {
                if (workshop_root_override.has_value()) {
                    lines[index] =
                        "WorkshopRootDir="
                        + workshop_root_override->string();
                }
                found_workshop_root = true;
            }
            if (ascii_equals_ignore_case(key, "ActiveModList")
                && ascii_equals_ignore_case(
                    value,
                    "UE4SSExperimentalPW"
                )) {
                found_ue4ss_activation = true;
            }
        }
        if (!found_global_enable) {
            lines.insert(
                lines.begin()
                    + static_cast<std::ptrdiff_t>(section_start + 1),
                "bGlobalEnableMod=True"
            );
            ++section_end;
        }
        if (workshop_root_override.has_value() && !found_workshop_root) {
            lines.insert(
                lines.begin() + static_cast<std::ptrdiff_t>(section_end),
                "WorkshopRootDir=" + workshop_root_override->string()
            );
            ++section_end;
        }
        if (!found_ue4ss_activation) {
            lines.insert(
                lines.begin() + static_cast<std::ptrdiff_t>(section_end),
                "ActiveModList=UE4SSExperimentalPW"
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

[[nodiscard]] auto enable_statue_markers_json(
    std::string_view existing
) -> std::string
{
    const std::string source{existing};
    static const std::regex enabled_pattern{
        R"regex((\{[^{}]*"mod_name"\s*:\s*"StatueMapMarkers"[^{}]*"mod_enabled"\s*:\s*)(?:true|false))regex",
        std::regex::ECMAScript | std::regex::icase,
    };
    if (std::regex_search(source, enabled_pattern)) {
        return std::regex_replace(
            source,
            enabled_pattern,
            "$1true",
            std::regex_constants::format_first_only
        );
    }

    const auto closing = source.rfind(']');
    const std::string entry =
        "  {\n"
        "    \"mod_name\": \"StatueMapMarkers\",\n"
        "    \"mod_enabled\": true\n"
        "  }\n";
    if (closing == std::string::npos) {
        return "[\n" + entry + "]\n";
    }

    auto updated = source;
    const auto has_entries =
        source.substr(0, closing).find('{') != std::string::npos;
    std::string insertion;
    if (has_entries) {
        auto last_content = closing;
        while (last_content > 0
               && std::isspace(
                      static_cast<unsigned char>(source[last_content - 1])
                  )
                    != 0) {
            --last_content;
        }
        insertion = ",\n" + entry;
        updated.insert(last_content, insertion);
    } else {
        updated.insert(closing, "\n" + entry);
    }
    return updated;
}

[[nodiscard]] auto enable_statue_markers_text(
    std::string_view existing
) -> std::string
{
    const auto newline =
        existing.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    auto lines = split_lines(existing);
    bool found = false;
    for (auto& line : lines) {
        const auto colon = line.find(':');
        if (colon == std::string::npos
            || !ascii_equals_ignore_case(
                trim(std::string_view{line}.substr(0, colon)),
                "StatueMapMarkers"
            )) {
            continue;
        }
        line = "StatueMapMarkers : 1";
        found = true;
    }
    if (!found) {
        lines.emplace_back("StatueMapMarkers : 1");
    }

    std::string updated;
    for (const auto& line : lines) {
        updated += line;
        updated += newline;
    }
    return updated;
}

[[nodiscard]] auto enable_palverify_watchdog_json(
    std::string_view existing
) -> std::string
{
    const std::string source{existing};
    static const std::regex enabled_pattern{
        R"regex((\{[^{}]*"mod_name"\s*:\s*"PalVerify"[^{}]*"mod_enabled"\s*:\s*)(?:true|false))regex",
        std::regex::ECMAScript | std::regex::icase,
    };
    if (std::regex_search(source, enabled_pattern)) {
        return std::regex_replace(
            source,
            enabled_pattern,
            "$1true",
            std::regex_constants::format_first_only
        );
    }

    const auto closing = source.rfind(']');
    const std::string entry =
        "  {\n"
        "    \"mod_name\": \"PalVerify\",\n"
        "    \"mod_enabled\": true\n"
        "  }\n";
    if (closing == std::string::npos) {
        return "[\n" + entry + "]\n";
    }

    auto updated = source;
    const auto has_entries =
        source.substr(0, closing).find('{') != std::string::npos;
    if (has_entries) {
        auto last_content = closing;
        while (last_content > 0
               && std::isspace(
                      static_cast<unsigned char>(source[last_content - 1])
                  )
                    != 0) {
            --last_content;
        }
        updated.insert(last_content, ",\n" + entry);
    } else {
        updated.insert(closing, "\n" + entry);
    }
    return updated;
}

[[nodiscard]] auto enable_palverify_watchdog_text(
    std::string_view existing
) -> std::string
{
    const auto newline =
        existing.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    auto lines = split_lines(existing);
    bool found = false;
    for (auto& line : lines) {
        const auto colon = line.find(':');
        if (colon == std::string::npos
            || !ascii_equals_ignore_case(
                trim(std::string_view{line}.substr(0, colon)),
                "PalVerify"
            )) {
            continue;
        }
        line = "PalVerify : 1";
        found = true;
    }
    if (!found) {
        lines.emplace_back("PalVerify : 1");
    }

    std::string updated;
    for (const auto& line : lines) {
        updated += line;
        updated += newline;
    }
    return updated;
}

[[nodiscard]] auto install_managed_statue_markers(
    const std::filesystem::path& mods_root,
    std::span<const PayloadFile> files,
    std::optional<std::filesystem::path>& workshop_root_override
) -> InstallResult
{
    workshop_root_override.reset();
    bool has_managed_payload = false;
    for (const auto& file : files) {
        if (managed_ue4ss_relative_path(
                file.relative_path.lexically_normal()
            ).has_value()) {
            has_managed_payload = true;
            break;
        }
    }
    if (!has_managed_payload) {
        return {.success = true, .detail = "managed-mod-not-present"};
    }

    const auto local_workshop_root = mods_root / "Workshop";
    workshop_root_override = local_workshop_root;
    const auto target = local_workshop_root / "3625223587";
    bool copied_statue_markers = false;
    for (const auto& file : files) {
        const auto relative = managed_ue4ss_relative_path(
            file.relative_path.lexically_normal()
        );
        if (!relative.has_value()) {
            continue;
        }
        if (!write_payload_file(target / *relative, file)) {
            return {
                .success = false,
                .detail = "managed-mod-write-failed",
            };
        }
        copied_statue_markers = copied_statue_markers
            || relative->generic_string().starts_with(
                "Mods/StatueMapMarkers/"
            );
    }
    if (!copied_statue_markers) {
        return {
            .success = false,
            .detail = "managed-statue-markers-incomplete",
        };
    }

    const auto mods_directory = target / "Mods";
    std::vector<std::filesystem::path> watchdog_targets{
        mods_directory / "PalVerify" / "Scripts",
    };
    const auto active_watchdog =
        mods_root / "NativeMods" / "UE4SS" / "Mods" / "PalVerify"
        / "Scripts";
    std::error_code active_error;
    if (std::filesystem::is_directory(active_watchdog, active_error)
        && !active_error) {
        watchdog_targets.push_back(active_watchdog);
    }
    const std::array<std::pair<std::string_view, std::string_view>, 3>
        watchdog_files{{
            {"client/Scripts/main.lua", "main.lua"},
            {"client/Scripts/config.json", "config.json"},
            {
                "client/Scripts/PalVerifyClient.exe",
                "PalVerifyClient.exe",
            },
        }};
    for (const auto& [source_path, target_name] : watchdog_files) {
        const auto source = std::ranges::find_if(
            files,
            [source_path](const PayloadFile& file) {
                return file.relative_path.generic_string() == source_path;
            }
        );
        if (source == files.end()) {
            return {
                .success = false,
                .detail = "managed-watchdog-write-failed",
            };
        }
        for (const auto& watchdog_scripts : watchdog_targets) {
            if (!write_payload_file(
                    watchdog_scripts / target_name,
                    *source
                )) {
                return {
                    .success = false,
                    .detail = "managed-watchdog-write-failed",
                };
            }
        }
    }
    const auto json_path = mods_directory / "mods.json";
    const auto text_path = mods_directory / "mods.txt";
    const auto json = enable_palverify_watchdog_json(
        enable_statue_markers_json(read_file(json_path))
    );
    const auto text = enable_palverify_watchdog_text(
        enable_statue_markers_text(read_file(text_path))
    );
    if (!write_file_atomically(json_path, json, ".pal3mien-tmp")
        || !write_file_atomically(text_path, text, ".pal3mien-tmp")) {
        return {
            .success = false,
            .detail = "managed-mod-config-write-failed",
        };
    }
    const auto legacy_target =
        local_workshop_root / "UE4SSExperimentalPW";
    if (legacy_target != target && is_ue4ss_package(legacy_target)) {
        std::error_code removal_error;
        std::filesystem::remove_all(legacy_target, removal_error);
        if (removal_error) {
            return {
                .success = false,
                .detail = "managed-mod-legacy-cleanup-failed",
            };
        }
    }
    return {.success = true, .detail = "managed-mod-installed"};
}

[[nodiscard]] auto update_palmod_settings(
    const std::filesystem::path& mods_root,
    bool enable_managed_mods,
    const std::optional<std::filesystem::path>& workshop_root_override
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

    const auto without_legacy_activation =
        remove_palverify_game_mod_activation(existing_settings);
    const auto updated_settings = enable_managed_mods
        ? enable_global_mod_loading(
              without_legacy_activation,
              workshop_root_override
          )
        : without_legacy_activation;
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

[[nodiscard]] auto safe_mod_id(std::string_view value) -> bool
{
    return !value.empty() && value.size() <= 96
        && std::ranges::all_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return std::isalnum(byte) != 0 || character == '.'
                   || character == '_' || character == '-'
                   || character == ':';
           });
}

[[nodiscard]] auto protected_mod_id(std::string_view value) -> bool
{
    constexpr std::array<std::string_view, 3> protected_ids{
        "PalVerify",
        "UE4SSExperimentalPW",
        "StatueMapMarkers",
    };
    return std::ranges::any_of(
        protected_ids,
        [value](std::string_view protected_id) {
            return ascii_equals_ignore_case(value, protected_id);
        }
    );
}

[[nodiscard]] auto compact_mod_id(std::string_view value) -> std::string
{
    std::string compact;
    compact.reserve(std::min<std::size_t>(value.size(), 96));
    for (const auto character : value) {
        if (compact.size() == 96) {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '.'
            || character == '_' || character == '-'
            || character == ':') {
            compact.push_back(character);
        } else {
            compact.push_back('_');
        }
    }
    return compact.empty() ? "unknown-mod" : compact;
}

[[nodiscard]] auto package_name(
    const std::filesystem::path& package_root
) -> std::string
{
    static const std::regex package_pattern{
        R"regex("PackageName"\s*:\s*"([^"]*)")regex",
        std::regex::ECMAScript,
    };
    const auto info = read_file(package_root / "Info.json");
    std::smatch match;
    if (!std::regex_search(info, match, package_pattern)) {
        return compact_mod_id(package_root.filename().string());
    }
    return compact_mod_id(match[1].str());
}

[[nodiscard]] auto requested_mod_id(
    std::span<const std::string> requested,
    std::string_view candidate
) -> bool
{
    return std::ranges::any_of(
        requested,
        [candidate](const std::string& requested_id) {
            return ascii_equals_ignore_case(requested_id, candidate);
        }
    );
}

void collect_workshop_remediation_targets(
    const std::filesystem::path& workshop_root,
    std::span<const std::string> requested,
    std::vector<std::filesystem::path>& targets
)
{
    std::error_code error;
    for (std::filesystem::directory_iterator package{
             workshop_root,
             std::filesystem::directory_options::skip_permission_denied,
             error,
         },
         end;
         package != end;
         package.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!package->is_directory(error) || error) {
            error.clear();
            continue;
        }
        const auto info_path = package->path() / "Info.json";
        if (!std::filesystem::is_regular_file(info_path, error) || error) {
            error.clear();
            continue;
        }
        if (requested_mod_id(requested, package_name(package->path()))) {
            targets.push_back(package->path());
            continue;
        }

        const auto nested_root = package->path() / "Mods";
        std::error_code nested_error;
        for (std::filesystem::directory_iterator nested{
                 nested_root,
                 std::filesystem::directory_options::skip_permission_denied,
                 nested_error,
             },
             nested_end;
             nested != nested_end;
             nested.increment(nested_error)) {
            if (nested_error) {
                nested_error.clear();
                continue;
            }
            if (!nested->is_directory(nested_error) || nested_error) {
                nested_error.clear();
                continue;
            }
            const auto enabled = nested->path() / "enabled.txt";
            if (!std::filesystem::is_regular_file(
                    enabled,
                    nested_error
                )
                || nested_error) {
                nested_error.clear();
                continue;
            }
            const auto id =
                compact_mod_id(nested->path().filename().string());
            if (requested_mod_id(requested, id)) {
                targets.push_back(nested->path());
            }
        }
    }
}

void collect_legacy_remediation_targets(
    const std::filesystem::path& root,
    std::string_view prefix,
    std::span<const std::string> requested,
    std::vector<std::filesystem::path>& targets
)
{
    std::error_code error;
    for (std::filesystem::directory_iterator file{
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error,
         },
         end;
         file != end;
         file.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!file->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto extension = file->path().extension().string();
        if (extension != ".pak" && extension != ".utoc"
            && extension != ".ucas") {
            continue;
        }
        const auto id =
            compact_mod_id(std::string{prefix}
                           + file->path().filename().string());
        if (requested_mod_id(requested, id)) {
            targets.push_back(file->path());
        }
    }
}

[[nodiscard]] auto path_depth(const std::filesystem::path& path)
    -> std::size_t
{
    return static_cast<std::size_t>(
        std::distance(path.begin(), path.end())
    );
}

[[nodiscard]] auto path_is_within(
    const std::filesystem::path& candidate,
    const std::filesystem::path& parent
) -> bool
{
    const auto candidate_normalized = candidate.lexically_normal();
    const auto parent_normalized = parent.lexically_normal();
    auto candidate_part = candidate_normalized.begin();
    for (auto parent_part = parent_normalized.begin();
         parent_part != parent_normalized.end();
         ++parent_part, ++candidate_part) {
        if (candidate_part == candidate_normalized.end()
            || !ascii_equals_ignore_case(
                candidate_part->string(),
                parent_part->string()
            )) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto unused_quarantine_destination(
    const std::filesystem::path& destination
) -> std::filesystem::path
{
    if (!std::filesystem::exists(destination)) {
        return destination;
    }
    for (unsigned int suffix = 1; suffix < 10'000; ++suffix) {
        const auto candidate = std::filesystem::path{
            destination.wstring() + L"." + std::to_wstring(suffix)
        };
        if (!std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::filesystem::filesystem_error{
        "quarantine destination exhausted",
        destination,
        std::make_error_code(std::errc::file_exists),
    };
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

auto quarantine_unapproved_mods(
    const std::filesystem::path& game_root,
    std::span<const std::string> mod_ids
) -> ModRemediationResult
{
    const auto game_executable =
        game_root / "Pal" / "Binaries" / "Win64"
        / "Palworld-Win64-Shipping.exe";
    if (!std::filesystem::is_regular_file(game_executable)) {
        return {
            .success = false,
            .quarantined = 0,
            .detail = "invalid-game-root",
        };
    }

    std::vector<std::string> requested;
    for (const auto& id : mod_ids) {
        if (!safe_mod_id(id)) {
            return {
                .success = false,
                .quarantined = 0,
                .detail = "invalid-unapproved-mod-id",
            };
        }
        if (protected_mod_id(id)
            || std::ranges::any_of(
                requested,
                [&id](const std::string& existing) {
                    return ascii_equals_ignore_case(existing, id);
                }
            )) {
            continue;
        }
        requested.push_back(id);
    }
    if (requested.empty()) {
        return {
            .success = true,
            .quarantined = 0,
            .detail = "no-remediation-required",
        };
    }

    try {
        std::vector<std::filesystem::path> targets;
        collect_workshop_remediation_targets(
            game_root / "Mods" / "Workshop",
            requested,
            targets
        );
        collect_legacy_remediation_targets(
            game_root / "Pal" / "Content" / "Paks" / "~mods",
            "legacy-pak:",
            requested,
            targets
        );
        collect_legacy_remediation_targets(
            game_root / "Pal" / "Content" / "Paks" / "LogicMods",
            "logic-mod:",
            requested,
            targets
        );
        std::ranges::sort(
            targets,
            [](const auto& left, const auto& right) {
                return std::tuple{
                           path_depth(left),
                           left.generic_string(),
                       }
                    < std::tuple{
                           path_depth(right),
                           right.generic_string(),
                       };
            }
        );

        std::vector<std::filesystem::path> selected;
        for (const auto& target : targets) {
            if (std::ranges::any_of(
                    selected,
                    [&target](const auto& parent) {
                        return path_is_within(target, parent);
                    }
                )) {
                continue;
            }
            selected.push_back(target);
        }

        const auto quarantine_root =
            game_root / "Mods" / ".pal3mien-quarantine";
        std::size_t quarantined = 0;
        for (const auto& target : selected) {
            std::error_code relative_error;
            const auto relative =
                std::filesystem::relative(
                    target,
                    game_root,
                    relative_error
                );
            if (relative_error || !safe_payload_path(relative)) {
                return {
                    .success = false,
                    .quarantined = quarantined,
                    .detail = "unsafe-remediation-target",
                };
            }
            const auto destination = unused_quarantine_destination(
                quarantine_root / relative
            );
            std::filesystem::create_directories(
                destination.parent_path()
            );
            std::filesystem::rename(target, destination);
            ++quarantined;
        }
        return {
            .success = true,
            .quarantined = quarantined,
            .detail = quarantined == 0
                ? "unapproved-mod-not-found"
                : "unapproved-mod-quarantined",
        };
    } catch (const std::filesystem::filesystem_error& error) {
        return {
            .success = false,
            .quarantined = 0,
            .detail = error.code().message(),
        };
    }
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
            if (managed_ue4ss_relative_path(
                    file.relative_path.lexically_normal()
                ).has_value()) {
                continue;
            }
            const auto target =
                package_target / file.relative_path.lexically_normal();
            if (!write_payload_file(target, file)) {
                return {
                    .success = false,
                    .detail = "payload-write-failed",
                };
            }
        }
        std::optional<std::filesystem::path> workshop_root_override;
        const auto managed = install_managed_statue_markers(
            mods_root,
            files,
            workshop_root_override
        );
        if (!managed.success) {
            return managed;
        }
        const auto settings = update_palmod_settings(
            mods_root,
            managed.detail == "managed-mod-installed",
            workshop_root_override
        );
        if (!settings.success) {
            return settings;
        }
    } catch (const std::filesystem::filesystem_error& error) {
        return {.success = false, .detail = error.code().message()};
    }
    return {.success = true, .detail = "installed"};
}

}  // namespace palverify
