#include "palverify/client_report.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace palverify {
namespace {

class Sha256 final {
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(
                &algorithm_,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0
            )
            < 0) {
            throw std::runtime_error{"BCryptOpenAlgorithmProvider failed"};
        }

        DWORD bytes = 0;
        DWORD result_bytes = 0;
        if (BCryptGetProperty(
                algorithm_,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&bytes),
                sizeof(bytes),
                &result_bytes,
                0
            )
            < 0) {
            throw std::runtime_error{"BCryptGetProperty failed"};
        }
        object_.resize(bytes);
        if (BCryptCreateHash(
                algorithm_,
                &hash_,
                object_.data(),
                static_cast<ULONG>(object_.size()),
                nullptr,
                0,
                0
            )
            < 0) {
            throw std::runtime_error{"BCryptCreateHash failed"};
        }
    }

    Sha256(const Sha256&) = delete;
    auto operator=(const Sha256&) -> Sha256& = delete;

    ~Sha256()
    {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    void update(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size != 0) {
            const auto chunk = static_cast<ULONG>(
                std::min<std::size_t>(size, MAXDWORD)
            );
            if (BCryptHashData(
                    hash_,
                    const_cast<PUCHAR>(bytes),
                    chunk,
                    0
                )
                < 0) {
                throw std::runtime_error{"BCryptHashData failed"};
            }
            bytes += chunk;
            size -= chunk;
        }
    }

    [[nodiscard]] auto finish() -> std::array<unsigned char, 32>
    {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(
                hash_,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0
            )
            < 0) {
            throw std::runtime_error{"BCryptFinishHash failed"};
        }
        BCryptDestroyHash(hash_);
        hash_ = nullptr;
        return digest;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_HASH_HANDLE hash_{nullptr};
    std::vector<unsigned char> object_;
};

[[nodiscard]] auto hex_digest(
    const std::array<unsigned char, 32>& digest
) -> std::string
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] auto file_digest(const std::filesystem::path& path)
    -> std::string
{
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"cannot read mod file"};
    }
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        throw std::runtime_error{"cannot hash mod file"};
    }
    return hex_digest(hash.finish());
}

[[nodiscard]] auto package_digest(const std::filesystem::path& root)
    -> std::string
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator{
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error,
         },
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (iterator->is_regular_file(error) && !error) {
            files.push_back(iterator->path());
        }
        error.clear();
    }
    std::ranges::sort(files, [&](const auto& left, const auto& right) {
        return std::filesystem::relative(left, root).generic_string()
            < std::filesystem::relative(right, root).generic_string();
    });

    Sha256 aggregate;
    for (const auto& file : files) {
        const auto relative =
            std::filesystem::relative(file, root).generic_string();
        const auto digest = file_digest(file);
        aggregate.update(relative.data(), relative.size());
        const char separator = '\0';
        aggregate.update(&separator, 1);
        aggregate.update(digest.data(), digest.size());
        aggregate.update(&separator, 1);
    }
    return hex_digest(aggregate.finish());
}

[[nodiscard]] auto read_text(const std::filesystem::path& path)
    -> std::string
{
    std::ifstream input{path, std::ios::binary};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

[[nodiscard]] auto json_field(
    std::string_view json,
    std::string_view field
) -> std::string
{
    const std::regex pattern{
        "\"" + std::string{field}
            + R"regex("\s*:\s*"([^"]*)")regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(json.begin(), json.end(), match, pattern)) {
        return {};
    }
    return match[1].str();
}

[[nodiscard]] auto compact_id(std::string_view value) -> std::string
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

void scan_workshop(
    const std::filesystem::path& game_root,
    std::vector<ReportedMod>& inventory
)
{
    const auto root = game_root / "Mods" / "Workshop";
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error,
         },
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_directory(error) || error) {
            error.clear();
            continue;
        }
        const auto info_path = iterator->path() / "Info.json";
        if (!std::filesystem::is_regular_file(info_path, error) || error) {
            error.clear();
            continue;
        }
        const auto info = read_text(info_path);
        auto id = json_field(info, "PackageName");
        if (id.empty()) {
            id = iterator->path().filename().string();
        }
        inventory.push_back({
            .id = compact_id(id),
            .version = compact_id(json_field(info, "Version")),
            .digest = package_digest(iterator->path()),
        });
    }
}

void scan_legacy_files(
    const std::filesystem::path& root,
    std::string_view prefix,
    std::vector<ReportedMod>& inventory
)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error,
         },
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto extension = iterator->path().extension().string();
        if (extension != ".pak" && extension != ".utoc"
            && extension != ".ucas") {
            continue;
        }
        inventory.push_back({
            .id =
                compact_id(std::string{prefix}
                           + iterator->path().filename().string()),
            .version = "legacy",
            .digest = file_digest(iterator->path()),
        });
    }
}

[[nodiscard]] auto json_escape(std::string_view value) -> std::string
{
    std::string escaped;
    escaped.reserve(value.size());
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (byte < 0x20) {
                escaped += "\\u00";
                escaped.push_back(hex[(byte >> 4U) & 0x0fU]);
                escaped.push_back(hex[byte & 0x0fU]);
            } else {
                escaped.push_back(character);
            }
        }
    }
    return escaped;
}

void append_json_string(std::string& output, std::string_view value)
{
    output.push_back('"');
    output += json_escape(value);
    output.push_back('"');
}

[[nodiscard]] auto utf8_to_wide(std::string_view value)
    -> std::optional<std::wstring>
{
    if (value.empty()) {
        return std::nullopt;
    }
    const auto size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            wide.data(),
            size
        )
        != size) {
        return std::nullopt;
    }
    return wide;
}

[[nodiscard]] auto secure_endpoint(std::string_view endpoint) -> bool
{
    const auto wide = utf8_to_wide(endpoint);
    if (!wide.has_value()) {
        return false;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(
            wide->c_str(),
            static_cast<DWORD>(wide->size()),
            0,
            &components
        )
        == FALSE) {
        return false;
    }
    if (components.nScheme == INTERNET_SCHEME_HTTPS) {
        return true;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP) {
        return false;
    }
    const std::wstring_view host{
        components.lpszHostName,
        components.dwHostNameLength,
    };
    return host == L"127.0.0.1" || host == L"localhost"
        || host == L"::1";
}

}  // namespace

auto steam_user_id_from_account_id(std::uint32_t account_id) -> std::string
{
    constexpr std::uint64_t steam_id_base = 76561197960265728ULL;
    return "steam_"
        + std::to_string(steam_id_base + static_cast<std::uint64_t>(account_id));
}

auto next_report_sequence(
    std::uint64_t previous,
    std::uint64_t wall_clock_milliseconds
) -> std::uint64_t
{
    if (previous >= wall_clock_milliseconds) {
        return previous + 1;
    }
    return wall_clock_milliseconds;
}

auto scan_mod_inventory(const std::filesystem::path& game_root)
    -> std::vector<ReportedMod>
{
    std::vector<ReportedMod> inventory;
    scan_workshop(game_root, inventory);
    scan_legacy_files(
        game_root / "Pal" / "Content" / "Paks" / "~mods",
        "legacy-pak:",
        inventory
    );
    scan_legacy_files(
        game_root / "Pal" / "Content" / "Paks" / "LogicMods",
        "logic-mod:",
        inventory
    );
    std::ranges::sort(inventory, [](const auto& left, const auto& right) {
        return std::tie(left.id, left.version, left.digest)
            < std::tie(right.id, right.version, right.digest);
    });
    return inventory;
}

auto build_client_report_json(const ClientReport& report) -> std::string
{
    std::string output{"{\"serverId\":"};
    append_json_string(output, report.server_id);
    output += ",\"userId\":";
    append_json_string(output, report.user_id);
    output += ",\"protocolVersion\":";
    append_json_string(output, report.protocol_version);
    output += ",\"challenge\":";
    append_json_string(output, report.challenge);
    output += ",\"sequence\":" + std::to_string(report.sequence);
    output += ",\"sentAt\":";
    append_json_string(output, report.sent_at);
    output += ",\"mods\":[";
    for (std::size_t index = 0; index < report.mods.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"id\":";
        append_json_string(output, report.mods[index].id);
        output += ",\"version\":";
        append_json_string(output, report.mods[index].version);
        output += ",\"digest\":";
        append_json_string(output, report.mods[index].digest);
        output.push_back('}');
    }
    output += "],\"violations\":[";
    for (std::size_t index = 0; index < report.violations.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_json_string(output, report.violations[index]);
    }
    output += "]}";
    return output;
}

auto build_challenge_request_json(
    std::string_view server_id,
    std::string_view user_id
) -> std::string
{
    std::string output{"{\"serverId\":"};
    append_json_string(output, server_id);
    output += ",\"userId\":";
    append_json_string(output, user_id);
    output.push_back('}');
    return output;
}

auto parse_client_config(std::string_view json)
    -> std::optional<ClientConfig>
{
    ClientConfig config{
        .coordinator = json_field(json, "coordinator"),
        .server_id = json_field(json, "serverId"),
    };
    if (!secure_endpoint(config.coordinator) || config.server_id.empty()
        || config.server_id.size() > 64) {
        return std::nullopt;
    }
    while (!config.coordinator.empty()
           && config.coordinator.back() == '/') {
        config.coordinator.pop_back();
    }
    for (const auto character : config.server_id) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) == 0 && character != '-'
            && character != '_') {
            return std::nullopt;
        }
    }
    return config;
}

auto parse_challenge_json(std::string_view json)
    -> std::optional<std::string>
{
    auto challenge = json_field(json, "challenge");
    if (challenge.size() != 64) {
        return std::nullopt;
    }
    for (const auto character : challenge) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isxdigit(byte) == 0) {
            return std::nullopt;
        }
    }
    return challenge;
}

}  // namespace palverify
