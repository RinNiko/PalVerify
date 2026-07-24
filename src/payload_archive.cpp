#include "palverify/payload_archive.hpp"

#define NOMINMAX
#include <Windows.h>
#include <compressapi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace palverify {
namespace {

constexpr std::array<std::byte, 8> archive_magic{
    std::byte{'P'},
    std::byte{'V'},
    std::byte{'P'},
    std::byte{'A'},
    std::byte{'Y'},
    std::byte{'0'},
    std::byte{'1'},
    std::byte{0},
};
constexpr std::uint32_t archive_version = 1;
constexpr std::uint32_t compression_algorithm = 1;
constexpr std::size_t digest_size = 32;
constexpr std::uint64_t maximum_file_size = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_payload_size = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t maximum_file_count = 64;
constexpr std::uint32_t maximum_path_size = 1024;

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr auto rotate_right(
    std::uint32_t value,
    unsigned int count
) -> std::uint32_t
{
    return std::rotr(value, static_cast<int>(count));
}

void transform_sha256(
    std::array<std::uint32_t, 8>& state,
    const std::byte* block
)
{
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const auto offset = index * 4;
        words[index] =
            (std::to_integer<std::uint32_t>(block[offset]) << 24U)
            | (std::to_integer<std::uint32_t>(block[offset + 1]) << 16U)
            | (std::to_integer<std::uint32_t>(block[offset + 2]) << 8U)
            | std::to_integer<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto first = words[index - 15];
        const auto second = words[index - 2];
        const auto sigma_zero =
            rotate_right(first, 7U) ^ rotate_right(first, 18U)
            ^ (first >> 3U);
        const auto sigma_one =
            rotate_right(second, 17U) ^ rotate_right(second, 19U)
            ^ (second >> 10U);
        words[index] = words[index - 16] + sigma_zero
            + words[index - 7] + sigma_one;
    }

    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum_one =
            rotate_right(e, 6U) ^ rotate_right(e, 11U)
            ^ rotate_right(e, 25U);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary_one =
            h + sum_one + choose + round_constants[index] + words[index];
        const auto sum_zero =
            rotate_right(a, 2U) ^ rotate_right(a, 13U)
            ^ rotate_right(a, 22U);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary_two = sum_zero + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary_one;
        d = c;
        c = b;
        b = a;
        a = temporary_one + temporary_two;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

[[nodiscard]] auto sha256_digest(std::span<const std::byte> content)
    -> std::array<std::byte, digest_size>
{
    std::array<std::uint32_t, 8> state{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };

    const auto full_blocks = content.size() / 64;
    for (std::size_t index = 0; index < full_blocks; ++index) {
        transform_sha256(state, content.data() + index * 64);
    }

    std::array<std::byte, 128> tail{};
    const auto remainder = content.size() % 64;
    std::ranges::copy(
        content.subspan(full_blocks * 64),
        tail.begin()
    );
    tail[remainder] = std::byte{0x80};
    const auto tail_size = remainder < 56 ? std::size_t{64}
                                         : std::size_t{128};
    const auto bit_length =
        static_cast<std::uint64_t>(content.size()) * 8ULL;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[tail_size - 1 - index] = static_cast<std::byte>(
            (bit_length >> (index * 8U)) & 0xffU
        );
    }
    transform_sha256(state, tail.data());
    if (tail_size == 128) {
        transform_sha256(state, tail.data() + 64);
    }

    std::array<std::byte, digest_size> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4] =
            static_cast<std::byte>((state[index] >> 24U) & 0xffU);
        digest[index * 4 + 1] =
            static_cast<std::byte>((state[index] >> 16U) & 0xffU);
        digest[index * 4 + 2] =
            static_cast<std::byte>((state[index] >> 8U) & 0xffU);
        digest[index * 4 + 3] =
            static_cast<std::byte>(state[index] & 0xffU);
    }
    return digest;
}

template <typename Integer>
void append_integer(std::vector<std::byte>& output, Integer value)
{
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU)
        );
    }
}

template <typename Integer>
[[nodiscard]] auto read_integer(
    std::span<const std::byte> input,
    std::size_t& offset,
    Integer& value
) -> bool
{
    static_assert(std::is_unsigned_v<Integer>);
    if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(
                     std::to_integer<unsigned int>(input[offset + index])
                 )
            << (index * 8U);
    }
    offset += sizeof(Integer);
    return true;
}

[[nodiscard]] auto make_compressor() -> COMPRESSOR_HANDLE
{
    COMPRESSOR_HANDLE compressor = nullptr;
    if (CreateCompressor(
            COMPRESS_ALGORITHM_XPRESS_HUFF,
            nullptr,
            &compressor
        )
        == FALSE) {
        return nullptr;
    }
    return compressor;
}

[[nodiscard]] auto make_decompressor() -> DECOMPRESSOR_HANDLE
{
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (CreateDecompressor(
            COMPRESS_ALGORITHM_XPRESS_HUFF,
            nullptr,
            &decompressor
        )
        == FALSE) {
        return nullptr;
    }
    return decompressor;
}

[[nodiscard]] auto compress_content(
    COMPRESSOR_HANDLE compressor,
    std::span<const std::byte> content
) -> std::vector<std::byte>
{
    if (content.empty()) {
        return {};
    }
    SIZE_T required = 0;
    const auto first = Compress(
        compressor,
        content.data(),
        content.size(),
        nullptr,
        0,
        &required
    );
    if (first != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || required == 0) {
        return {};
    }
    std::vector<std::byte> compressed(required);
    SIZE_T written = 0;
    if (Compress(
            compressor,
            content.data(),
            content.size(),
            compressed.data(),
            compressed.size(),
            &written
        )
        == FALSE) {
        return {};
    }
    compressed.resize(written);
    return compressed;
}

[[nodiscard]] auto decompress_content(
    DECOMPRESSOR_HANDLE decompressor,
    std::span<const std::byte> compressed,
    std::size_t expected_size
) -> std::vector<std::byte>
{
    if (expected_size == 0) {
        return compressed.empty() ? std::vector<std::byte>{}
                                  : std::vector<std::byte>{};
    }
    std::vector<std::byte> content(expected_size);
    SIZE_T written = 0;
    if (Decompress(
            decompressor,
            compressed.data(),
            compressed.size(),
            content.data(),
            content.size(),
            &written
        )
            == FALSE
        || written != expected_size) {
        return {};
    }
    return content;
}

[[nodiscard]] auto lower_hex_digest(
    const std::array<std::byte, digest_size>& digest
) -> std::string
{
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto value : digest) {
        const auto byte = std::to_integer<unsigned int>(value);
        output.push_back(hexadecimal[byte >> 4U]);
        output.push_back(hexadecimal[byte & 0x0fU]);
    }
    return output;
}

}  // namespace

auto sha256_hex(std::span<const std::byte> content) -> std::string
{
    return lower_hex_digest(sha256_digest(content));
}

auto pack_payload_archive(std::span<const PayloadFile> files)
    -> PackedPayloadResult
{
    if (files.empty() || files.size() > maximum_file_count) {
        return {
            .success = false,
            .detail = "invalid-file-count",
            .archive = {},
        };
    }
    const auto compressor = make_compressor();
    if (compressor == nullptr) {
        return {
            .success = false,
            .detail = "compression-unavailable",
            .archive = {},
        };
    }

    std::vector<std::byte> archive;
    archive.insert(archive.end(), archive_magic.begin(), archive_magic.end());
    append_integer(archive, archive_version);
    append_integer(archive, compression_algorithm);
    append_integer(archive, static_cast<std::uint32_t>(files.size()));

    std::uint64_t total_size = 0;
    for (const auto& file : files) {
        const auto path = file.relative_path.generic_string();
        if (path.empty() || path.size() > maximum_path_size
            || file.content.size() > maximum_file_size) {
            CloseCompressor(compressor);
            return {
                .success = false,
                .detail = "invalid-payload-file",
                .archive = {},
            };
        }
        total_size += file.content.size();
        if (total_size > maximum_payload_size) {
            CloseCompressor(compressor);
            return {
                .success = false,
                .detail = "payload-too-large",
                .archive = {},
            };
        }
        const auto compressed = compress_content(compressor, file.content);
        if (!file.content.empty() && compressed.empty()) {
            CloseCompressor(compressor);
            return {
                .success = false,
                .detail = "compression-failed",
                .archive = {},
            };
        }
        const auto digest = sha256_digest(file.content);
        append_integer(archive, static_cast<std::uint32_t>(path.size()));
        append_integer(
            archive,
            static_cast<std::uint64_t>(file.content.size())
        );
        append_integer(
            archive,
            static_cast<std::uint64_t>(compressed.size())
        );
        archive.insert(archive.end(), digest.begin(), digest.end());
        const auto* path_bytes =
            reinterpret_cast<const std::byte*>(path.data());
        archive.insert(
            archive.end(),
            path_bytes,
            path_bytes + path.size()
        );
        archive.insert(
            archive.end(),
            compressed.begin(),
            compressed.end()
        );
    }
    CloseCompressor(compressor);
    return {
        .success = true,
        .detail = "packed",
        .archive = std::move(archive),
    };
}

auto unpack_payload_archive(std::span<const std::byte> archive)
    -> PayloadArchiveResult
{
    if (archive.size() < archive_magic.size()
        || !std::ranges::equal(
            archive.first(archive_magic.size()),
            archive_magic
        )) {
        return {
            .success = false,
            .detail = "invalid-archive-magic",
            .files = {},
        };
    }

    std::size_t offset = archive_magic.size();
    std::uint32_t version = 0;
    std::uint32_t algorithm = 0;
    std::uint32_t file_count = 0;
    if (!read_integer(archive, offset, version)
        || !read_integer(archive, offset, algorithm)
        || !read_integer(archive, offset, file_count)
        || version != archive_version
        || algorithm != compression_algorithm
        || file_count == 0 || file_count > maximum_file_count) {
        return {
            .success = false,
            .detail = "invalid-archive-header",
            .files = {},
        };
    }

    const auto decompressor = make_decompressor();
    if (decompressor == nullptr) {
        return {
            .success = false,
            .detail = "decompression-unavailable",
            .files = {},
        };
    }

    std::vector<PayloadFile> files;
    files.reserve(file_count);
    std::uint64_t total_size = 0;
    for (std::uint32_t index = 0; index < file_count; ++index) {
        std::uint32_t path_size = 0;
        std::uint64_t content_size = 0;
        std::uint64_t compressed_size = 0;
        if (!read_integer(archive, offset, path_size)
            || !read_integer(archive, offset, content_size)
            || !read_integer(archive, offset, compressed_size)
            || path_size == 0 || path_size > maximum_path_size
            || content_size > maximum_file_size
            || compressed_size > maximum_payload_size
            || offset > archive.size()
            || archive.size() - offset
                < digest_size + static_cast<std::size_t>(path_size)) {
            CloseDecompressor(decompressor);
            return {
                .success = false,
                .detail = "invalid-archive-entry",
                .files = {},
            };
        }
        total_size += content_size;
        if (total_size > maximum_payload_size) {
            CloseDecompressor(decompressor);
            return {
                .success = false,
                .detail = "payload-too-large",
                .files = {},
            };
        }

        std::array<std::byte, digest_size> expected_digest{};
        std::ranges::copy(
            archive.subspan(offset, digest_size),
            expected_digest.begin()
        );
        offset += digest_size;
        const auto* path_data =
            reinterpret_cast<const char*>(archive.data() + offset);
        const std::string path{path_data, path_size};
        offset += path_size;
        if (compressed_size
                > static_cast<std::uint64_t>(archive.size() - offset)
            || content_size
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::size_t>::max)()
                )
            || (content_size == 0 && compressed_size != 0)) {
            CloseDecompressor(decompressor);
            return {
                .success = false,
                .detail = "invalid-archive-entry",
                .files = {},
            };
        }
        const auto compressed_count =
            static_cast<std::size_t>(compressed_size);
        auto content = decompress_content(
            decompressor,
            archive.subspan(offset, compressed_count),
            static_cast<std::size_t>(content_size)
        );
        offset += compressed_count;
        if ((content_size != 0 && content.empty())
            || sha256_digest(content) != expected_digest) {
            CloseDecompressor(decompressor);
            return {
                .success = false,
                .detail = "payload-hash-mismatch",
                .files = {},
            };
        }
        files.push_back({
            .relative_path = std::filesystem::path{path},
            .content = std::move(content),
            .sha256 = lower_hex_digest(expected_digest),
        });
    }
    CloseDecompressor(decompressor);
    if (offset != archive.size()) {
        return {
            .success = false,
            .detail = "trailing-archive-data",
            .files = {},
        };
    }
    return {
        .success = true,
        .detail = "unpacked",
        .files = std::move(files),
    };
}

}  // namespace palverify
