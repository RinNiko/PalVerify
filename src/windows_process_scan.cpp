#include "palverify/windows_process_scan.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace palverify {
namespace {

struct LanguageCodePage {
    WORD language;
    WORD code_page;
};

struct FileStamp {
    std::uint64_t size;
    std::uint64_t write_time;

    [[nodiscard]] auto operator==(const FileStamp&) const -> bool = default;
};

struct CachedEvidence {
    FileStamp stamp;
    ProcessFileEvidence evidence;
};

struct ObservedProcess {
    std::string image_name;
    ProcessFileEvidence file;
};

struct ObservedModule {
    std::string image_name;
    ProcessFileEvidence file;
    bool game_location;
    bool system_location;
};

[[nodiscard]] constexpr auto ascii_lower(char value) -> char
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

[[nodiscard]] constexpr auto ascii_equals_ignore_case(
    std::string_view left,
    std::string_view right
) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto ascii_image_name(const wchar_t* wide_name) -> std::string
{
    std::string image_name;
    while (*wide_name != L'\0') {
        const auto value = static_cast<unsigned int>(*wide_name);
        if (value > 0x7F) {
            return {};
        }
        image_name.push_back(static_cast<char>(value));
        ++wide_name;
    }
    return image_name;
}

[[nodiscard]] auto utf8(std::wstring_view value) -> std::string
{
    if (value.empty()) {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (required <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        required,
        nullptr,
        nullptr
    );
    if (written != required) {
        return {};
    }
    return output;
}

[[nodiscard]] auto process_executable_path(DWORD process_id)
    -> std::optional<std::filesystem::path>
{
    const auto process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        process_id
    );
    if (process == nullptr) {
        return std::nullopt;
    }

    std::wstring buffer(32768, L'\0');
    auto length = static_cast<DWORD>(buffer.size());
    const auto queried = QueryFullProcessImageNameW(
        process,
        0,
        buffer.data(),
        &length
    );
    CloseHandle(process);
    if (queried == FALSE) {
        return std::nullopt;
    }
    buffer.resize(length);
    return std::filesystem::path{buffer};
}

[[nodiscard]] auto file_stamp(const std::filesystem::path& path)
    -> std::optional<FileStamp>
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &attributes
        )
        == FALSE) {
        return std::nullopt;
    }

    ULARGE_INTEGER size{};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    ULARGE_INTEGER write_time{};
    write_time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    write_time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    return FileStamp{
        .size = size.QuadPart,
        .write_time = write_time.QuadPart,
    };
}

[[nodiscard]] auto query_version_string(
    const std::vector<std::byte>& version,
    WORD language,
    WORD code_page,
    std::wstring_view name
) -> std::string
{
    wchar_t sub_block[128]{};
    const auto formatted = swprintf_s(
        sub_block,
        L"\\StringFileInfo\\%04x%04x\\%.*s",
        language,
        code_page,
        static_cast<int>(name.size()),
        name.data()
    );
    if (formatted <= 0) {
        return {};
    }

    void* value = nullptr;
    UINT length = 0;
    if (VerQueryValueW(
            version.data(),
            sub_block,
            &value,
            &length
        )
        == FALSE
        || value == nullptr
        || length == 0) {
        return {};
    }
    const auto* text = static_cast<const wchar_t*>(value);
    const auto text_length =
        text[length - 1] == L'\0' ? length - 1 : length;
    return utf8(std::wstring_view{text, text_length});
}

[[nodiscard]] auto version_strings(
    const std::filesystem::path& path
) -> std::pair<std::string, std::string>
{
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<std::byte> version(size);
    if (GetFileVersionInfoW(
            path.c_str(),
            0,
            size,
            version.data()
        )
        == FALSE) {
        return {};
    }

    void* translations = nullptr;
    UINT translation_bytes = 0;
    if (VerQueryValueW(
            version.data(),
            L"\\VarFileInfo\\Translation",
            &translations,
            &translation_bytes
        )
        == FALSE
        || translations == nullptr
        || translation_bytes < sizeof(LanguageCodePage)) {
        return {};
    }

    const auto* entries =
        static_cast<const LanguageCodePage*>(translations);
    const auto count = translation_bytes / sizeof(LanguageCodePage);
    for (UINT index = 0; index < count; ++index) {
        auto description = query_version_string(
            version,
            entries[index].language,
            entries[index].code_page,
            L"FileDescription"
        );
        auto company = query_version_string(
            version,
            entries[index].language,
            entries[index].code_page,
            L"CompanyName"
        );
        if (!description.empty() || !company.empty()) {
            return {std::move(description), std::move(company)};
        }
    }
    return {};
}

struct SignatureEvidence {
    std::string signer_name;
    bool valid;
};

[[nodiscard]] auto signature_evidence(
    const std::filesystem::path& path
) -> SignatureEvidence
{
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file_info;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags =
        WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto no_window = reinterpret_cast<HWND>(INVALID_HANDLE_VALUE);
    const auto status = WinVerifyTrust(
        no_window,
        &policy,
        &trust
    );

    SignatureEvidence result{.signer_name = {}, .valid = status == 0};
    if (result.valid) {
        const auto* provider =
            WTHelperProvDataFromStateData(trust.hWVTStateData);
        const auto* signer = provider == nullptr
            ? nullptr
            : WTHelperGetProvSignerFromChain(
                const_cast<CRYPT_PROVIDER_DATA*>(provider),
                0,
                FALSE,
                0
            );
        if (signer != nullptr && signer->csCertChain != 0) {
            const auto* certificate = signer->pasCertChain[0].pCert;
            const auto length = CertGetNameStringW(
                certificate,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0,
                nullptr,
                nullptr,
                0
            );
            if (length > 1) {
                std::wstring name(length, L'\0');
                const auto written = CertGetNameStringW(
                    certificate,
                    CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0,
                    nullptr,
                    name.data(),
                    length
                );
                if (written == length) {
                    name.resize(length - 1);
                    result.signer_name = utf8(name);
                }
            }
        }
    }

    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(
        WinVerifyTrust(no_window, &policy, &trust)
    );
    return result;
}

[[nodiscard]] auto sha256_file(
    const std::filesystem::path& path
) -> std::string
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0
        )
        != 0) {
        return {};
    }

    DWORD object_size = 0;
    DWORD digest_size = 0;
    DWORD bytes = 0;
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &bytes,
            0
        )
        != 0
        || BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digest_size),
            sizeof(digest_size),
            &bytes,
            0
        )
            != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> digest(digest_size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            object_size,
            nullptr,
            0,
            0
        )
        != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    const auto file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    bool success = file != INVALID_HANDLE_VALUE;
    std::array<UCHAR, 64 * 1024> buffer{};
    while (success) {
        DWORD read = 0;
        if (ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr
            )
            == FALSE) {
            success = false;
            break;
        }
        if (read == 0) {
            break;
        }
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0) {
            success = false;
            break;
        }
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (success && BCryptFinishHash(
            hash,
            digest.data(),
            digest_size,
            0
        )
            != 0) {
        success = false;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) {
        return {};
    }

    constexpr std::string_view hexadecimal{"0123456789abcdef"};
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto value : digest) {
        output.push_back(hexadecimal[value >> 4]);
        output.push_back(hexadecimal[value & 0x0F]);
    }
    return output;
}

[[nodiscard]] auto cached_process_evidence(
    const std::filesystem::path& path
) -> std::optional<ProcessFileEvidence>
{
    static std::unordered_map<std::wstring, CachedEvidence> cache;
    const auto stamp = file_stamp(path);
    if (!stamp.has_value()) {
        return std::nullopt;
    }
    const auto key = path.native();
    const auto found = cache.find(key);
    if (found != cache.end() && found->second.stamp == *stamp) {
        return found->second.evidence;
    }

    auto [description, company] = version_strings(path);
    ProcessFileEvidence evidence{
        .file_description = std::move(description),
        .company_name = std::move(company),
        .signer_name = {},
        .sha256 = {},
        .signature_valid = false,
    };
    const auto known_cheat_engine_size =
        std::uint64_t{18'611'176};
    const auto known_wemod_bootstrap_size = std::uint64_t{552'184};
    const auto hash_candidate =
        (
            evidence.file_description == "Cheat Engine"
            && evidence.company_name == "Cheat Engine"
        )
        || (
            evidence.file_description == "WeMod - Cheats and Mods"
            && evidence.company_name == "WeMod"
        )
        || stamp->size == known_cheat_engine_size
        || stamp->size == known_wemod_bootstrap_size;
    auto signature = signature_evidence(path);
    evidence.signer_name = std::move(signature.signer_name);
    evidence.signature_valid = signature.valid;
    if (hash_candidate) {
        evidence.sha256 = sha256_file(path);
    }
    if (cache.size() >= 1024) {
        cache.clear();
    }
    cache.insert_or_assign(
        key,
        CachedEvidence{.stamp = *stamp, .evidence = evidence}
    );
    return evidence;
}

[[nodiscard]] auto cached_module_evidence(
    const std::filesystem::path& path
) -> std::optional<ProcessFileEvidence>
{
    static std::unordered_map<std::wstring, CachedEvidence> cache;
    const auto stamp = file_stamp(path);
    if (!stamp.has_value()) {
        return std::nullopt;
    }
    const auto key = path.native();
    const auto found = cache.find(key);
    if (found != cache.end() && found->second.stamp == *stamp) {
        return found->second.evidence;
    }
    const auto evidence = inspect_process_executable(path);
    if (!evidence.has_value()) {
        return std::nullopt;
    }
    if (cache.size() >= 1024) {
        cache.clear();
    }
    cache.insert_or_assign(
        key,
        CachedEvidence{.stamp = *stamp, .evidence = *evidence}
    );
    return evidence;
}

[[nodiscard]] auto normalized_lower_path(
    const std::filesystem::path& path
) -> std::wstring
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        normalized = std::filesystem::absolute(path, error);
    }
    if (error) {
        normalized = path.lexically_normal();
    }
    auto value = normalized.native();
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    while (value.size() > 3
           && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] auto path_is_within(
    const std::filesystem::path& child,
    const std::filesystem::path& root
) -> bool
{
    const auto child_value = normalized_lower_path(child);
    auto root_value = normalized_lower_path(root);
    if (child_value == root_value) {
        return true;
    }
    root_value.push_back(L'\\');
    return child_value.starts_with(root_value);
}

[[nodiscard]] auto windows_directory() -> std::filesystem::path
{
    std::wstring buffer(MAX_PATH, L'\0');
    auto length = GetWindowsDirectoryW(
        buffer.data(),
        static_cast<UINT>(buffer.size())
    );
    if (length == 0) {
        return {};
    }
    if (length >= buffer.size()) {
        buffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetWindowsDirectoryW(
            buffer.data(),
            static_cast<UINT>(buffer.size())
        );
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
    }
    buffer.resize(length);
    return std::filesystem::path{buffer};
}

[[nodiscard]] auto palworld_process_id() -> std::optional<DWORD>
{
    const auto snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::optional<DWORD> process_id;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(
                    entry.szExeFile,
                    L"Palworld-Win64-Shipping.exe"
                )
                == 0) {
                process_id = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return process_id;
}

[[nodiscard]] auto executable_page(DWORD protection) -> bool
{
    if ((protection & PAGE_GUARD) != 0
        || (protection & PAGE_NOACCESS) != 0) {
        return false;
    }
    const auto access = protection & 0xffU;
    return access == PAGE_EXECUTE
        || access == PAGE_EXECUTE_READ
        || access == PAGE_EXECUTE_READWRITE
        || access == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] auto manual_map_detected(DWORD process_id) -> bool
{
    const auto process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE,
        process_id
    );
    if (process == nullptr) {
        return false;
    }

    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    auto address = reinterpret_cast<std::uintptr_t>(
        system.lpMinimumApplicationAddress
    );
    const auto maximum = reinterpret_cast<std::uintptr_t>(
        system.lpMaximumApplicationAddress
    );
    bool detected = false;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQueryEx(
                process,
                reinterpret_cast<const void*>(address),
                &region,
                sizeof(region)
            )
            == 0) {
            break;
        }
        const auto base =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto next = base + region.RegionSize;
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE
            && region.RegionSize >= 64 * 1024
            && executable_page(region.Protect)) {
            std::array<std::byte, 4096> header{};
            SIZE_T read = 0;
            if (ReadProcessMemory(
                    process,
                    region.BaseAddress,
                    header.data(),
                    header.size(),
                    &read
                )
                    != FALSE
                && looks_like_manual_map_candidate(
                    std::span{header}.first(read)
                )) {
                detected = true;
                break;
            }
        }
        if (next <= address) {
            break;
        }
        address = next;
    }
    CloseHandle(process);
    return detected;
}

}  // namespace

auto inspect_process_executable(const std::filesystem::path& executable)
    -> std::optional<ProcessFileEvidence>
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(executable, error) || error) {
        return std::nullopt;
    }

    auto [description, company] = version_strings(executable);
    auto signature = signature_evidence(executable);
    return ProcessFileEvidence{
        .file_description = std::move(description),
        .company_name = std::move(company),
        .signer_name = std::move(signature.signer_name),
        .sha256 = sha256_file(executable),
        .signature_valid = signature.valid,
    };
}

auto detect_module_matches(std::span<const ModuleEvidence> modules)
    -> std::vector<ModuleRuleMatch>
{
    constexpr std::string_view palforge_sha256 =
        "4d0597dc7ba65b65106743afbadd70c2045f9e07725bdf4629c0d057a4469bba";
    constexpr std::array wand_module_names{
        std::string_view{"celib_x64.dll"},
        std::string_view{"inputcaptureplugin_x64.dll"},
        std::string_view{"overlay_game_x64.dll"},
        std::string_view{"tophat_service_x64.dll"},
        std::string_view{"trainerlib_x64.dll"},
        std::string_view{"trainerlibplugin_x64.dll"},
        std::string_view{"we-graphics-hook64.dll"},
    };
    for (const auto& module : modules) {
        std::string_view match_reason;
        if (module.sha256 == palforge_sha256) {
            match_reason = "KNOWN_INJECTED_MODULE_HASH";
        }
        const auto wemod_publisher =
            module.signature_valid
            && ascii_equals_ignore_case(module.signer_name, "WeMod LLC");
        const auto wemod_metadata =
            ascii_equals_ignore_case(module.company_name, "WeMod")
            || ascii_equals_ignore_case(module.company_name, "WeMod LLC");
        bool wand_module_name = false;
        for (const auto known_name : wand_module_names) {
            if (ascii_equals_ignore_case(module.image_name, known_name)) {
                wand_module_name = true;
                break;
            }
        }
        if (match_reason.empty() && wemod_publisher) {
            match_reason = "WEMOD_MODULE_SIGNATURE";
        } else if (match_reason.empty() && wemod_metadata) {
            match_reason = "WEMOD_MODULE_METADATA";
        } else if (match_reason.empty() && wand_module_name) {
            match_reason = "WEMOD_MODULE_NAME";
        }
        if (match_reason.empty() && !module.signature_valid
            && !module.system_location
            && !module.game_location) {
            match_reason = "UNSIGNED_EXTERNAL_MODULE";
        }
        if (!match_reason.empty()) {
            return {{
                .rule = ProcessRuleId::InjectedModuleDetected,
                .image_name = std::string{module.image_name},
                .sha256 = std::string{module.sha256},
                .signer_name = std::string{module.signer_name},
                .file_description = std::string{module.file_description},
                .company_name = std::string{module.company_name},
                .match_reason = std::string{match_reason},
                .signature_valid = module.signature_valid,
            }};
        }
    }
    return {};
}

auto detect_module_rules(std::span<const ModuleEvidence> modules)
    -> std::vector<ProcessRuleId>
{
    const auto matches = detect_module_matches(modules);
    std::vector<ProcessRuleId> rules;
    rules.reserve(matches.size());
    for (const auto& match : matches) {
        rules.push_back(match.rule);
    }
    return rules;
}

auto looks_like_manual_map_candidate(std::span<const std::byte> header)
    -> bool
{
    if (header.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, header.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return false;
    }
    const auto offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > header.size()
        || header.size() - offset < sizeof(IMAGE_NT_HEADERS64)) {
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, header.data() + offset, sizeof(nt));
    return nt.Signature == IMAGE_NT_SIGNATURE
        && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64
        && nt.FileHeader.NumberOfSections != 0
        && nt.FileHeader.NumberOfSections <= 96
        && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC
        && nt.OptionalHeader.SizeOfImage >= 64 * 1024;
}

auto scan_running_processes() -> ProcessScanResult
{
    const auto snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {.available = false, .rules = {}};
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<ObservedProcess> observed;

    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            ObservedProcess process{
                .image_name = ascii_image_name(entry.szExeFile),
                .file = {},
            };
            const auto path = process_executable_path(entry.th32ProcessID);
            if (path.has_value()) {
                const auto file = cached_process_evidence(*path);
                if (file.has_value()) {
                    process.file = *file;
                }
            }
            observed.push_back(std::move(process));
        } while (Process32NextW(snapshot, &entry) != FALSE);
    } else {
        CloseHandle(snapshot);
        return {.available = false, .rules = {}};
    }
    CloseHandle(snapshot);

    std::vector<ProcessEvidence> evidence;
    evidence.reserve(observed.size());
    for (const auto& process : observed) {
        evidence.push_back({
            .image_name = process.image_name,
            .file_description = process.file.file_description,
            .company_name = process.file.company_name,
            .signer_name = process.file.signer_name,
            .sha256 = process.file.sha256,
            .signature_valid = process.file.signature_valid,
        });
    }

    return {
        .available = true,
        .rules = detect_process_rules(evidence),
    };
}

auto scan_palworld_modules(const std::filesystem::path& game_root)
    -> ModuleScanResult
{
    const auto process_id = palworld_process_id();
    if (!process_id.has_value()) {
        return {.available = false, .rules = {}, .matches = {}};
    }
    const auto snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        *process_id
    );
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {.available = false, .rules = {}, .matches = {}};
    }

    const auto windows_root = windows_directory();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<ObservedModule> observed;
    if (Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            const std::filesystem::path path{entry.szExePath};
            const auto file = cached_module_evidence(path);
            if (!file.has_value()) {
                continue;
            }
            observed.push_back({
                .image_name = ascii_image_name(entry.szModule),
                .file = *file,
                .game_location = path_is_within(path, game_root),
                .system_location =
                    !windows_root.empty()
                    && path_is_within(path, windows_root),
            });
        } while (Module32NextW(snapshot, &entry) != FALSE);
    } else {
        CloseHandle(snapshot);
        return {.available = false, .rules = {}, .matches = {}};
    }
    CloseHandle(snapshot);

    std::vector<ModuleEvidence> evidence;
    evidence.reserve(observed.size());
    for (const auto& module : observed) {
        evidence.push_back({
            .image_name = module.image_name,
            .sha256 = module.file.sha256,
            .signature_valid = module.file.signature_valid,
            .game_location = module.game_location,
            .system_location = module.system_location,
            .signer_name = module.file.signer_name,
            .file_description = module.file.file_description,
            .company_name = module.file.company_name,
        });
    }
    auto matches = detect_module_matches(evidence);
    std::vector<ProcessRuleId> rules;
    rules.reserve(matches.size() + 1);
    for (const auto& match : matches) {
        rules.push_back(match.rule);
    }
    if (manual_map_detected(*process_id)) {
        rules.push_back(ProcessRuleId::ManualMapDetected);
        matches.push_back({
            .rule = ProcessRuleId::ManualMapDetected,
            .image_name = {},
            .sha256 = {},
            .signer_name = {},
            .file_description = {},
            .company_name = {},
            .match_reason = "PRIVATE_EXECUTABLE_PE_HEADER",
            .signature_valid = false,
        });
    }
    return {
        .available = true,
        .rules = std::move(rules),
        .matches = std::move(matches),
    };
}

}  // namespace palverify
