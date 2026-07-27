#pragma once

#include "palverify/payload_archive.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

[[nodiscard]] auto remove_palverify_game_mod_activation(
    std::string_view existing_settings
) -> std::string;

[[nodiscard]] auto extract_steam_library_paths(std::string_view vdf)
    -> std::vector<std::filesystem::path>;

[[nodiscard]] auto find_palworld_install(
    const std::vector<std::filesystem::path>& steam_library_roots
) -> std::optional<std::filesystem::path>;

struct InstallResult {
    bool success;
    std::string detail;
};

struct ModRemediationResult {
    bool success;
    std::size_t quarantined;
    std::string detail;
};

[[nodiscard]] auto install_palverify_payload(
    const std::filesystem::path& game_root,
    const std::filesystem::path& payload_root
) -> InstallResult;

[[nodiscard]] auto install_palverify_payload(
    const std::filesystem::path& game_root,
    std::span<const PayloadFile> files
) -> InstallResult;

[[nodiscard]] auto quarantine_unapproved_mods(
    const std::filesystem::path& game_root,
    std::span<const std::string> mod_ids
) -> ModRemediationResult;

}  // namespace palverify
