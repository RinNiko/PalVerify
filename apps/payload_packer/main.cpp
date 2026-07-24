#include "palverify/payload_archive.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] auto read_file(const std::filesystem::path& path)
    -> std::vector<std::byte>
{
    std::ifstream input{path, std::ios::binary};
    const std::vector<char> raw{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    if (input.bad()) {
        return {};
    }
    std::vector<std::byte> content(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        content[index] = static_cast<std::byte>(
            static_cast<unsigned char>(raw[index])
        );
    }
    return content;
}

[[nodiscard]] auto write_binary(
    const std::filesystem::path& path,
    const std::vector<std::byte>& content
) -> bool
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(
        reinterpret_cast<const char*>(content.data()),
        static_cast<std::streamsize>(content.size())
    );
    return static_cast<bool>(output);
}

[[nodiscard]] auto write_resource_script(
    const std::filesystem::path& path,
    const std::filesystem::path& archive
) -> bool
{
    std::filesystem::create_directories(path.parent_path());
    auto archive_path = std::filesystem::absolute(archive).generic_string();
    std::string escaped;
    escaped.reserve(archive_path.size());
    for (const auto character : archive_path) {
        if (character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << "#include \"resource.h\"\n"
              "IDR_PALVERIFY_PAYLOAD RCDATA \""
           << escaped << "\"\n";
    return static_cast<bool>(output);
}

}  // namespace

auto main(int argument_count, char** arguments) -> int
{
    if (argument_count < 6 || (argument_count - 3) % 2 != 0) {
        std::cerr
            << "usage: palverify_payload_packer <archive> <resource-rc> "
               "<relative-path> <source-file> [...]\n";
        return 2;
    }

    const std::filesystem::path archive_path{arguments[1]};
    const std::filesystem::path resource_path{arguments[2]};
    std::vector<palverify::PayloadFile> files;
    for (int index = 3; index < argument_count; index += 2) {
        const std::filesystem::path source{arguments[index + 1]};
        if (!std::filesystem::is_regular_file(source)) {
            std::cerr << "missing payload file: " << source.string() << '\n';
            return 3;
        }
        auto content = read_file(source);
        if (content.empty() && std::filesystem::file_size(source) != 0) {
            std::cerr << "could not read payload file: " << source.string()
                      << '\n';
            return 4;
        }
        files.push_back({
            .relative_path = arguments[index],
            .content = std::move(content),
            .sha256 = {},
        });
    }

    const auto packed = palverify::pack_payload_archive(files);
    if (!packed.success) {
        std::cerr << "payload packing failed: " << packed.detail << '\n';
        return 5;
    }
    if (!write_binary(archive_path, packed.archive)
        || !write_resource_script(resource_path, archive_path)) {
        std::cerr << "could not write embedded payload resource\n";
        return 6;
    }
    std::cout << "PACKED_PAYLOAD files=" << files.size()
              << " bytes=" << packed.archive.size() << '\n';
    return 0;
}
