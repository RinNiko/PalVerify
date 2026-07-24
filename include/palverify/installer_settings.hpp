#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace palverify {

[[nodiscard]] auto enable_palverify_mod(std::string_view existing_settings)
    -> std::string;

[[nodiscard]] auto extract_steam_library_paths(std::string_view vdf)
    -> std::vector<std::filesystem::path>;

[[nodiscard]] auto find_palworld_install(
    const std::vector<std::filesystem::path>& steam_library_roots
) -> std::optional<std::filesystem::path>;

struct InstallResult {
    bool success;
    std::string detail;
};

[[nodiscard]] auto install_palverify_payload(
    const std::filesystem::path& game_root,
    const std::filesystem::path& payload_root
) -> InstallResult;

}  // namespace palverify
