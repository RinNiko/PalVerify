#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace palverify {

struct PayloadFile {
    std::filesystem::path relative_path;
    std::vector<std::byte> content;
    std::string sha256;
};

struct PackedPayloadResult {
    bool success;
    std::string detail;
    std::vector<std::byte> archive;
};

struct PayloadArchiveResult {
    bool success;
    std::string detail;
    std::vector<PayloadFile> files;
};

[[nodiscard]] auto sha256_hex(std::span<const std::byte> content)
    -> std::string;

[[nodiscard]] auto pack_payload_archive(
    std::span<const PayloadFile> files
) -> PackedPayloadResult;

[[nodiscard]] auto unpack_payload_archive(
    std::span<const std::byte> archive
) -> PayloadArchiveResult;

}  // namespace palverify
