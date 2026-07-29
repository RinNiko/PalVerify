#include "palverify/client_report.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
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

[[nodiscard]] auto ascii_equal_ignore_case(
    std::string_view left,
    std::string_view right
) -> bool
{
    return left.size() == right.size()
        && std::ranges::equal(left, right, [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs))
                   == std::tolower(static_cast<unsigned char>(rhs));
           });
}

[[nodiscard]] auto is_managed_palverify_cache(
    const std::filesystem::path& relative
) -> bool
{
    auto component = relative.begin();
    if (component == relative.end()
        || !ascii_equal_ignore_case(component->string(), "Mods")) {
        return false;
    }
    ++component;
    return component != relative.end()
        && ascii_equal_ignore_case(component->string(), "PalVerify");
}

[[nodiscard]] auto package_digest(
    const std::filesystem::path& root,
    bool exclude_managed_palverify_cache = false
)
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
            const auto relative =
                std::filesystem::relative(iterator->path(), root);
            if (exclude_managed_palverify_cache
                && is_managed_palverify_cache(relative)) {
                continue;
            }
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

[[nodiscard]] auto json_bool_field(
    std::string_view json,
    std::string_view field
) -> std::optional<bool>
{
    const std::regex pattern{
        "\"" + std::string{field} + R"regex("\s*:\s*(true|false))regex",
        std::regex::ECMAScript,
    };
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_search(json.begin(), json.end(), match, pattern)) {
        return std::nullopt;
    }
    return match[1].str() == "true";
}

[[nodiscard]] auto safe_policy_code(
    std::string_view value,
    std::size_t maximum
) -> bool
{
    return !value.empty() && value.size() <= maximum
        && std::ranges::all_of(value, [](const char character) {
               const auto byte = static_cast<unsigned char>(character);
               return std::isalnum(byte) != 0 || character == '.'
                   || character == '_' || character == '-'
                   || character == ':' || character == ',';
           });
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

[[nodiscard]] auto trim_ascii(std::string_view value) -> std::string_view
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

[[nodiscard]] auto configured_workshop_root(
    const std::filesystem::path& game_root
) -> std::optional<std::filesystem::path>
{
    const auto settings =
        read_text(game_root / "Mods" / "PalModSettings.ini");
    std::istringstream lines{settings};
    bool mods_enabled = false;
    std::string workshop_root;
    for (std::string line; std::getline(lines, line);) {
        const auto cleaned = trim_ascii(line);
        if (cleaned.empty() || cleaned.front() == ';'
            || cleaned.front() == '#' || cleaned.front() == '[') {
            continue;
        }
        const auto equals = cleaned.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto key = trim_ascii(cleaned.substr(0, equals));
        const auto value = trim_ascii(cleaned.substr(equals + 1));
        if (key == "bGlobalEnableMod") {
            mods_enabled =
                value == "True" || value == "true" || value == "1";
        } else if (key == "WorkshopRootDir") {
            workshop_root = value;
        }
    }
    if (!mods_enabled || workshop_root.empty()) {
        return std::nullopt;
    }

    auto root = std::filesystem::path{workshop_root}.lexically_normal();
    if (!root.is_absolute() || root == root.root_path()) {
        throw std::runtime_error{"unsafe WorkshopRootDir"};
    }
    return root;
}

void scan_enabled_ue4ss_mods(
    const std::filesystem::path& package_root,
    std::vector<ReportedMod>& inventory
)
{
    const auto mods_root = package_root / "Mods";
    std::error_code error;
    for (std::filesystem::directory_iterator iterator{
             mods_root,
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
        const auto enabled = iterator->path() / "enabled.txt";
        if (!std::filesystem::is_regular_file(enabled, error) || error) {
            error.clear();
            continue;
        }
        inventory.push_back({
            .id = compact_id(iterator->path().filename().string()),
            .version = "ue4ss",
            .digest = package_digest(iterator->path()),
        });
    }
}

void scan_workshop_root(
    const std::filesystem::path& root,
    std::vector<ReportedMod>& inventory,
    std::set<std::string>& reported_packages
)
{
    std::vector<std::filesystem::path> package_roots;
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
        package_roots.push_back(iterator->path());
    }
    std::ranges::sort(package_roots);

    for (const auto& package_root : package_roots) {
        const auto info_path = package_root / "Info.json";
        const auto info = read_text(info_path);
        auto id = json_field(info, "PackageName");
        if (id.empty()) {
            id = package_root.filename().string();
        }
        id = compact_id(id);
        if (!reported_packages.insert(id).second) {
            continue;
        }
        inventory.push_back({
            .id = id,
            .version = compact_id(json_field(info, "Version")),
            .digest = package_digest(
                package_root,
                id == "UE4SSExperimentalPW"
            ),
        });
        scan_enabled_ue4ss_mods(package_root, inventory);
    }
}

void scan_workshop(
    const std::filesystem::path& game_root,
    std::vector<ReportedMod>& inventory
)
{
    const auto local_root = game_root / "Mods" / "Workshop";
    std::set<std::string> reported_packages;
    scan_workshop_root(local_root, inventory, reported_packages);
    const auto external_root = configured_workshop_root(game_root);
    if (external_root.has_value()) {
        std::error_code local_error;
        std::error_code external_error;
        auto normalized_local =
            std::filesystem::weakly_canonical(local_root, local_error);
        auto normalized_external =
            std::filesystem::weakly_canonical(*external_root, external_error);
        if (local_error) {
            normalized_local = std::filesystem::absolute(
                local_root,
                local_error
            ).lexically_normal();
        }
        if (external_error) {
            normalized_external = std::filesystem::absolute(
                *external_root,
                external_error
            ).lexically_normal();
        }
        auto local_text = normalized_local.wstring();
        auto external_text = normalized_external.wstring();
        std::ranges::transform(
            local_text,
            local_text.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            }
        );
        std::ranges::transform(
            external_text,
            external_text.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            }
        );
        if (local_text == external_text) {
            return;
        }
        scan_workshop_root(*external_root, inventory, reported_packages);
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

auto should_retry_client_http(
    std::optional<unsigned long> status,
    unsigned long win32_error,
    unsigned int attempt,
    unsigned int maximum_attempts
) -> bool
{
    if (attempt >= maximum_attempts) {
        return false;
    }
    if (status.has_value()) {
        return *status == 408 || *status == 429 || *status >= 500;
    }
    constexpr std::array<unsigned long, 6> transient_errors{
        12002,
        12007,
        12029,
        12030,
        12031,
        12152,
    };
    return std::ranges::find(transient_errors, win32_error)
        != transient_errors.end();
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

struct AsyncModInventory::State {
    State(
        std::filesystem::path root,
        std::vector<ReportedMod> initial,
        ModInventoryScan scan_function
    )
        : game_root{std::move(root)},
          inventory{std::move(initial)},
          scan{std::move(scan_function)}
    {
        if (!scan) {
            scan = scan_mod_inventory;
        }
        worker = std::thread{[this] {
            run();
        }};
    }

    ~State()
    {
        {
            const std::scoped_lock lock{mutex};
            stopping = true;
        }
        condition.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void run()
    {
        while (true) {
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] {
                    return stopping || refresh_requested;
                });
                if (stopping) {
                    return;
                }
                refresh_requested = false;
                refresh_running = true;
            }

            const auto background_mode = SetThreadPriority(
                GetCurrentThread(),
                THREAD_MODE_BACKGROUND_BEGIN
            ) != FALSE;
            std::optional<std::vector<ReportedMod>> refreshed;
            try {
                refreshed = scan(game_root);
            } catch (const std::exception&) {
            }
            if (background_mode) {
                SetThreadPriority(
                    GetCurrentThread(),
                    THREAD_MODE_BACKGROUND_END
                );
            }

            {
                const std::scoped_lock lock{mutex};
                if (refreshed.has_value()) {
                    inventory = std::move(*refreshed);
                    last_refresh_failed = false;
                } else {
                    last_refresh_failed = true;
                }
                refresh_running = false;
            }
        }
    }

    std::filesystem::path game_root;
    std::vector<ReportedMod> inventory;
    ModInventoryScan scan;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::thread worker;
    bool stopping{};
    bool refresh_requested{};
    bool refresh_running{};
    bool last_refresh_failed{};
};

AsyncModInventory::AsyncModInventory(
    std::filesystem::path game_root,
    std::vector<ReportedMod> initial,
    ModInventoryScan scan
)
    : state_{std::make_unique<State>(
          std::move(game_root),
          std::move(initial),
          std::move(scan)
      )}
{
}

AsyncModInventory::~AsyncModInventory() = default;

void AsyncModInventory::request_refresh()
{
    {
        const std::scoped_lock lock{state_->mutex};
        if (state_->refresh_running || state_->refresh_requested) {
            return;
        }
        state_->refresh_requested = true;
    }
    state_->condition.notify_one();
}

auto AsyncModInventory::snapshot() const -> std::vector<ReportedMod>
{
    const std::scoped_lock lock{state_->mutex};
    return state_->inventory;
}

auto AsyncModInventory::refresh_in_progress() const -> bool
{
    const std::scoped_lock lock{state_->mutex};
    return state_->refresh_running || state_->refresh_requested;
}

auto AsyncModInventory::refresh_failed() const -> bool
{
    const std::scoped_lock lock{state_->mutex};
    return state_->last_refresh_failed;
}

void append_integrity_evidence_json(
    std::string& output,
    std::span<const IntegrityEvidence> evidence
)
{
    output += ",\"violationEvidence\":[";
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const auto& item = evidence[index];
        output += "{\"rule\":";
        append_json_string(output, item.rule);
        output += ",\"source\":";
        append_json_string(output, item.source);
        output += ",\"fileName\":";
        append_json_string(output, item.file_name);
        output += ",\"sha256\":";
        append_json_string(output, item.sha256);
        output += ",\"signerName\":";
        append_json_string(output, item.signer_name);
        output += ",\"fileDescription\":";
        append_json_string(output, item.file_description);
        output += ",\"companyName\":";
        append_json_string(output, item.company_name);
        output += ",\"matchReason\":";
        append_json_string(output, item.match_reason);
        output += ",\"signatureValid\":";
        output += item.signature_valid ? "true" : "false";
        output.push_back('}');
    }
    output.push_back(']');
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
    output.push_back(']');
    append_integrity_evidence_json(output, report.violation_evidence);
    output.push_back('}');
    return output;
}

auto format_runtime_integrity_message(
    std::span<const std::string> violations,
    std::span<const IntegrityEvidence> evidence
) -> std::string
{
    auto compact_display_text = [](std::string_view value) {
        std::string compact;
        compact.reserve(std::min<std::size_t>(value.size(), 160));
        bool previous_space = false;
        for (const auto character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20 || byte == 0x7f) {
                if (!compact.empty() && !previous_space) {
                    compact.push_back(' ');
                    previous_space = true;
                }
                continue;
            }
            if (compact.size() >= 160) {
                break;
            }
            compact.push_back(character);
            previous_space = character == ' ';
        }
        while (!compact.empty() && compact.back() == ' ') {
            compact.pop_back();
        }
        return compact;
    };
    auto safe_file_name = [&](std::string_view value) {
        const auto separator = value.find_last_of("\\/:");
        if (separator != std::string_view::npos) {
            value.remove_prefix(separator + 1);
        }
        return compact_display_text(value);
    };

    std::string message{
        "PalVerify phát hiện phần mềm hoặc module can thiệp:\r\n\r\n"
    };
    for (std::size_t index = 0; index < violations.size(); ++index) {
        if (index != 0) {
            message += "\r\n";
        }
        message += compact_display_text(violations[index]);
    }
    for (const auto& item : evidence) {
        message += "\r\n\r\n";
        const auto file_name = safe_file_name(item.file_name);
        if (!file_name.empty()) {
            message += "Tệp/DLL nghi vấn: " + file_name + "\r\n";
        }
        const auto description =
            compact_display_text(item.file_description);
        if (!description.empty()) {
            message += "Phần mềm: " + description + "\r\n";
        }
        const auto company = compact_display_text(item.company_name);
        if (!company.empty()) {
            message += "Hãng phát hành: " + company + "\r\n";
        }
        const auto signer = compact_display_text(item.signer_name);
        if (!signer.empty() && signer != company) {
            message += "Chữ ký số: " + signer + "\r\n";
        }
        const auto match_reason = compact_display_text(item.match_reason);
        if (!match_reason.empty()) {
            message += "Mã nhận diện: " + match_reason;
        } else if (message.ends_with("\r\n")) {
            message.resize(message.size() - 2);
        }
    }
    message +=
        "\r\n\r\nHãy tắt phần mềm liên quan, thoát hẳn Palworld "
        "rồi mở lại bằng launcher.";
    return message;
}

auto format_policy_rejection_message(
    const ClientPreflightResponse& response
) -> std::string
{
    if (response.accepted) {
        return {};
    }

    std::string explanation;
    std::string recovery;
    if (response.reason == "UNAPPROVED_MOD") {
        explanation =
            "Phát hiện mod không được máy chủ chấp thuận hoặc sai phiên bản.";
        recovery =
            "Hãy cập nhật PalVerify và các mod bằng launcher, sau đó mở lại "
            "Palworld.";
    } else if (response.reason == "INTEGRITY_VIOLATION") {
        explanation =
            "Phát hiện phần mềm hoặc tệp đang can thiệp vào Palworld.";
        recovery =
            "Hãy tắt phần mềm liên quan, thoát hẳn Palworld rồi mở lại bằng "
            "launcher.";
    } else if (response.reason == "MISSING_PALVERIFY") {
        explanation =
            "PalVerifyClient.exe không gửi heartbeat đúng thời hạn.";
        recovery =
            "Hãy thoát hẳn Palworld rồi mở lại bằng launcher để khởi động "
            "PalVerifyClient.exe.";
    } else {
        explanation = "Phiên xác minh không đạt yêu cầu của máy chủ.";
        recovery =
            "Hãy thoát hẳn Palworld, cập nhật bằng launcher rồi thử lại.";
    }

    std::string message{
        "PalVerify sắp ngắt kết nối để bảo vệ máy chủ.\r\n\r\nLý do: "
    };
    message += explanation;
    message += "\r\nMã lỗi: ";
    message += response.reason;
    if (!response.detail.empty()) {
        message += "\r\nChi tiết: ";
        message += response.detail;
    }
    message += "\r\n\r\nCách xử lý: ";
    message += recovery;
    return message;
}

auto build_client_preflight_json(const ClientPreflight& preflight)
    -> std::string
{
    std::string output{"{\"serverId\":"};
    append_json_string(output, preflight.server_id);
    output += ",\"protocolVersion\":";
    append_json_string(output, preflight.protocol_version);
    output += ",\"mods\":[";
    for (std::size_t index = 0; index < preflight.mods.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += "{\"id\":";
        append_json_string(output, preflight.mods[index].id);
        output += ",\"version\":";
        append_json_string(output, preflight.mods[index].version);
        output += ",\"digest\":";
        append_json_string(output, preflight.mods[index].digest);
        output.push_back('}');
    }
    output += "],\"violations\":[";
    for (std::size_t index = 0; index < preflight.violations.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        append_json_string(output, preflight.violations[index]);
    }
    output.push_back(']');
    append_integrity_evidence_json(output, preflight.violation_evidence);
    output.push_back('}');
    return output;
}

auto parse_client_preflight_response(std::string_view json)
    -> std::optional<ClientPreflightResponse>
{
    const auto accepted = json_bool_field(json, "accepted");
    ClientPreflightResponse response{
        .accepted = accepted.value_or(false),
        .reason = json_field(json, "reason"),
        .detail = json_field(json, "detail"),
    };
    if (!accepted.has_value() || !safe_policy_code(response.reason, 64)
        || (!response.detail.empty()
            && !safe_policy_code(response.detail, 1024))) {
        return std::nullopt;
    }
    if (response.accepted && response.reason != "VERIFIED") {
        return std::nullopt;
    }
    if (!response.accepted && response.detail.empty()) {
        return std::nullopt;
    }
    return response;
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
        .website = json_field(json, "website"),
        .server_id = json_field(json, "serverId"),
    };
    if (!secure_endpoint(config.coordinator)
        || !secure_endpoint(config.website)
        || config.server_id.empty()
        || config.server_id.size() > 64) {
        return std::nullopt;
    }
    while (!config.coordinator.empty()
           && config.coordinator.back() == '/') {
        config.coordinator.pop_back();
    }
    while (!config.website.empty() && config.website.back() == '/') {
        config.website.pop_back();
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

auto parse_client_ui_command(std::string_view value)
    -> std::optional<ClientUiCommand>
{
    const auto separator = value.find('|');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto kind = value.substr(0, separator);
    const auto payload = value.substr(separator + 1);
    if (kind == "verify") {
        if (payload.size() != 6
            || !std::ranges::all_of(payload, [](const char character) {
                   return std::isdigit(
                              static_cast<unsigned char>(character)
                          )
                       != 0;
               })) {
            return std::nullopt;
        }
        return ClientUiCommand{
            .kind = ClientUiCommandKind::verify,
            .value = std::string{payload},
        };
    }
    if (kind != "giftcode" || payload.size() > 32) {
        return std::nullopt;
    }
    if (!payload.empty()
        && (payload.size() < 4
            || !std::ranges::all_of(
                payload,
                [](const char character) {
                    const auto byte =
                        static_cast<unsigned char>(character);
                    return std::isupper(byte) != 0
                        || std::isdigit(byte) != 0
                        || character == '-';
                }
            ))) {
        return std::nullopt;
    }
    return ClientUiCommand{
        .kind = ClientUiCommandKind::giftcode,
        .value = std::string{payload},
    };
}

auto build_client_ui_url(
    std::string_view website,
    const ClientUiCommand& command
) -> std::string
{
    std::string url{website};
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url.push_back('/');
    if (command.kind == ClientUiCommandKind::verify) {
        return url + "#gacha-verify?verify=" + command.value;
    }
    return url + "#giftcode?giftcode=" + command.value;
}

}  // namespace palverify
