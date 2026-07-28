#include "palverify/client_report.hpp"
#include "palverify/installer_settings.hpp"
#include "palverify/payload_archive.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

[[nodiscard]] auto read_archive(const std::filesystem::path& path)
    -> std::vector<std::byte>
{
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> raw{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    if (!input || raw.empty()) {
        return {};
    }
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        bytes[index] = static_cast<std::byte>(
            static_cast<unsigned char>(raw[index])
        );
    }
    return bytes;
}

}  // namespace

auto main(int argument_count, char** arguments) -> int
{
    if (argument_count == 3
        && std::string_view{arguments[1]} == "--scan") {
        for (const auto& mod :
             palverify::scan_mod_inventory(arguments[2])) {
            std::cout << mod.id << '\t' << mod.version << '\t'
                      << mod.digest << '\n';
        }
        return 0;
    }
    if (argument_count != 3) {
        std::cerr
            << "usage: palverify_release_inventory <payload-archive> "
               "<clean-game-root>\n"
               "       palverify_release_inventory --scan <game-root>\n";
        return 2;
    }

    const std::filesystem::path archive_path{arguments[1]};
    const std::filesystem::path game_root{arguments[2]};
    std::error_code error;
    if (std::filesystem::exists(game_root, error)
        && !std::filesystem::is_empty(game_root, error)) {
        std::cerr << "clean game root must be empty\n";
        return 3;
    }

    const auto archive = read_archive(archive_path);
    const auto payload = palverify::unpack_payload_archive(archive);
    if (!payload.success) {
        std::cerr << "payload unpack failed: " << payload.detail << '\n';
        return 4;
    }

    const auto executable =
        game_root / "Pal" / "Binaries" / "Win64"
        / "Palworld-Win64-Shipping.exe";
    std::filesystem::create_directories(executable.parent_path(), error);
    std::ofstream fixture{executable, std::ios::binary};
    fixture << "release-inventory-fixture";
    fixture.close();
    if (!fixture) {
        std::cerr << "clean game fixture creation failed\n";
        return 5;
    }

    const auto installed = palverify::install_palverify_payload(
        game_root,
        payload.files
    );
    if (!installed.success) {
        std::cerr << "payload install failed: " << installed.detail << '\n';
        return 6;
    }

    for (const auto& mod : palverify::scan_mod_inventory(game_root)) {
        std::cout << mod.id << '\t' << mod.version << '\t' << mod.digest
                  << '\n';
    }
    return 0;
}
