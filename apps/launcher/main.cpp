#include "palverify/installer_settings.hpp"
#include "palverify/client_report.hpp"
#include "palverify/launcher_state.hpp"
#include "palverify/payload_archive.hpp"
#include "../../resources/launcher/resource.h"

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t window_class[] = L"Pal3MienLauncherWindow";
constexpr wchar_t window_title[] = L"Palworld 3 Miền";
constexpr std::string_view launcher_version = "1.0.37";
constexpr std::string_view launcher_display_version = "1.0";
constexpr std::string_view palverify_version = "1.0.16";
constexpr std::string_view default_manifest_url =
    "https://ae3mien.net/api/palverify/v1/launcher/manifest";
constexpr std::string_view release_manifest_asset_name =
    "palverify-launcher-manifest.json";
constexpr wchar_t default_website_url[] =
    L"https://ae3mien.net/";
constexpr float design_width = 1672.0F;
constexpr float design_height = 941.0F;
constexpr float heading_font_size = 24.0F;
constexpr float body_font_size = 18.0F;
constexpr float supporting_font_size = 17.0F;
constexpr float status_font_size = 20.0F;
constexpr wchar_t failure_support_hint[] =
    L"Bấm để xem và sao chép log gửi admin.";
constexpr UINT message_snapshot = WM_APP + 1;
constexpr UINT message_progress = WM_APP + 2;
constexpr UINT message_update_failed = WM_APP + 3;
constexpr UINT timer_recheck = 1;
constexpr UINT timer_client_watchdog = 2;
constexpr unsigned int game_launch_attempts = 3;

struct Arguments {
    std::optional<std::filesystem::path> game_root;
    std::string manifest_url{default_manifest_url};
    bool install_only{false};
    bool check_manifest{false};
    bool silent{false};
    bool disable_auto_update{false};
};

struct Snapshot {
    palverify::LauncherStatus status{
        palverify::LauncherStatus::ServerUnavailable
    };
    std::optional<palverify::LauncherManifest> manifest;
    std::optional<std::filesystem::path> game_root;
    palverify::SteamAppState steam{
        .installed = false,
        .build_id = {},
        .update_pending = false,
    };
    std::wstring headline{L"Đang kiểm tra phiên bản..."};
    std::wstring detail{L"Vui lòng chờ trong giây lát."};
    int progress{10};
    bool checking{true};
    bool updating{false};
    bool waiting_for_game_exit{false};
    bool payload_installed{false};
    bool preflight_succeeded{false};
    std::wstring support_log;
};

enum class StatusIcon {
    Success,
    Warning,
    Error,
};

[[nodiscard]] auto status_icon_for(const Snapshot& snapshot) -> StatusIcon
{
    if (!snapshot.support_log.empty()
        || snapshot.status == palverify::LauncherStatus::GameMissing) {
        return StatusIcon::Error;
    }
    if (palverify::launcher_can_start(
            snapshot.status,
            snapshot.payload_installed,
            snapshot.preflight_succeeded
        )) {
        return StatusIcon::Success;
    }
    return StatusIcon::Warning;
}

struct HttpResponse {
    DWORD status{};
    std::string body;
};

struct OpenHttpResponse {
    HINTERNET connection{};
    HINTERNET request{};
    DWORD status{};

    OpenHttpResponse() = default;
    OpenHttpResponse(const OpenHttpResponse&) = delete;
    auto operator=(const OpenHttpResponse&) -> OpenHttpResponse& = delete;

    OpenHttpResponse(OpenHttpResponse&& other) noexcept
        : connection{std::exchange(other.connection, nullptr)},
          request{std::exchange(other.request, nullptr)},
          status{other.status}
    {
    }

    auto operator=(OpenHttpResponse&& other) noexcept -> OpenHttpResponse&
    {
        if (this != &other) {
            reset();
            connection = std::exchange(other.connection, nullptr);
            request = std::exchange(other.request, nullptr);
            status = other.status;
        }
        return *this;
    }

    ~OpenHttpResponse()
    {
        reset();
    }

    void reset()
    {
        if (request != nullptr) {
            WinHttpCloseHandle(request);
            request = nullptr;
        }
        if (connection != nullptr) {
            WinHttpCloseHandle(connection);
            connection = nullptr;
        }
    }
};

thread_local std::string last_http_failure{"transport-error"};
thread_local DWORD last_http_error = ERROR_SUCCESS;

class ResourceImage {
public:
    ResourceImage() = default;
    ResourceImage(const ResourceImage&) = delete;
    auto operator=(const ResourceImage&) -> ResourceImage& = delete;

    ResourceImage(ResourceImage&& other) noexcept
        : stream_{std::exchange(other.stream_, nullptr)},
          image_{std::exchange(other.image_, nullptr)}
    {
    }

    auto operator=(ResourceImage&& other) noexcept -> ResourceImage&
    {
        if (this != &other) {
            reset();
            stream_ = std::exchange(other.stream_, nullptr);
            image_ = std::exchange(other.image_, nullptr);
        }
        return *this;
    }

    ~ResourceImage()
    {
        reset();
    }

    [[nodiscard]] auto get() const -> Gdiplus::Image*
    {
        return image_;
    }

    [[nodiscard]] static auto load(HINSTANCE instance, int id)
        -> ResourceImage
    {
        ResourceImage result;
        const auto resource =
            FindResourceW(
                instance,
                MAKEINTRESOURCEW(id),
                MAKEINTRESOURCEW(10)
            );
        if (resource == nullptr) {
            return result;
        }
        const auto loaded = LoadResource(instance, resource);
        const auto size = SizeofResource(instance, resource);
        const auto data = loaded != nullptr ? LockResource(loaded) : nullptr;
        if (data == nullptr || size == 0) {
            return result;
        }
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
        if (memory == nullptr) {
            return result;
        }
        const auto destination = GlobalLock(memory);
        if (destination == nullptr) {
            GlobalFree(memory);
            return result;
        }
        std::memcpy(destination, data, size);
        GlobalUnlock(memory);
        if (CreateStreamOnHGlobal(memory, TRUE, &result.stream_) != S_OK) {
            GlobalFree(memory);
            return {};
        }
        result.image_ = Gdiplus::Image::FromStream(result.stream_);
        if (result.image_ == nullptr
            || result.image_->GetLastStatus() != Gdiplus::Ok) {
            result.reset();
        }
        return result;
    }

private:
    void reset()
    {
        delete image_;
        image_ = nullptr;
        if (stream_ != nullptr) {
            stream_->Release();
            stream_ = nullptr;
        }
    }

    IStream* stream_{};
    Gdiplus::Image* image_{};
};

struct Assets {
    ResourceImage background;
    ResourceImage logo;
    ResourceImage button_start_ready;
    ResourceImage button_news_reference;
    ResourceImage progress;
    ResourceImage icon_check;
    ResourceImage icon_close;
    ResourceImage icon_globe;
    ResourceImage icon_minimize;
    ResourceImage icon_refresh;
    ResourceImage small_logo;

    explicit Assets(HINSTANCE instance)
        : background{ResourceImage::load(instance, IDR_BACKGROUND)},
          logo{ResourceImage::load(instance, IDR_LOGO)},
          button_start_ready{ResourceImage::load(instance, IDR_BUTTON_START)},
          button_news_reference{ResourceImage::load(instance, IDR_BUTTON_NEWS)},
          progress{ResourceImage::load(instance, IDR_PROGRESS)},
          icon_check{ResourceImage::load(instance, IDR_ICON_CHECK)},
          icon_close{ResourceImage::load(instance, IDR_ICON_CLOSE)},
          icon_globe{ResourceImage::load(instance, IDR_ICON_GLOBE)},
          icon_minimize{ResourceImage::load(instance, IDR_ICON_MINIMIZE)},
          icon_refresh{ResourceImage::load(instance, IDR_ICON_REFRESH)},
          small_logo{ResourceImage::load(instance, IDR_SMALL_LOGO)}
    {
    }
};

enum class InteractiveButton {
    None,
    Start,
    News,
};

[[nodiscard]] auto normalized_path_for_compare(
    const std::filesystem::path& value
) -> std::wstring
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(value, error);
    const auto normalized = error ? value.lexically_normal() : absolute;
    auto result = normalized.lexically_normal().wstring();
    std::ranges::transform(result, result.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return result;
}

[[nodiscard]] auto process_image_path(DWORD process_id)
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
    std::array<wchar_t, 32768> buffer{};
    DWORD length = static_cast<DWORD>(buffer.size());
    const auto queried = QueryFullProcessImageNameW(
        process,
        0,
        buffer.data(),
        &length
    );
    CloseHandle(process);
    if (queried == FALSE || length == 0) {
        return std::nullopt;
    }
    return std::filesystem::path{
        std::wstring{buffer.data(), length}
    };
}

[[nodiscard]] auto palworld_is_running() -> bool
{
    const auto snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );
    if (snapshot == INVALID_HANDLE_VALUE) {
        return true;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    auto success = Process32FirstW(snapshot, &entry) != FALSE;
    while (success) {
        if (_wcsicmp(entry.szExeFile, L"Palworld.exe") == 0
            || _wcsicmp(
                   entry.szExeFile,
                   L"Palworld-Win64-Shipping.exe"
               )
                == 0) {
            CloseHandle(snapshot);
            return true;
        }
        success = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    return false;
}

[[nodiscard]] auto stop_running_client(
    const std::filesystem::path& executable
) -> bool
{
    const auto target = normalized_path_for_compare(executable);
    const auto snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    auto success = Process32FirstW(snapshot, &entry) != FALSE;
    while (success) {
        const auto image = process_image_path(entry.th32ProcessID);
        if (image.has_value()
            && normalized_path_for_compare(*image) == target) {
            const auto process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION
                    | PROCESS_TERMINATE
                    | SYNCHRONIZE,
                FALSE,
                entry.th32ProcessID
            );
            if (process == nullptr) {
                CloseHandle(snapshot);
                return false;
            }
            const auto terminated = TerminateProcess(process, 0) != FALSE;
            const auto wait = terminated
                ? WaitForSingleObject(process, 5000)
                : WAIT_FAILED;
            CloseHandle(process);
            if (!terminated || wait != WAIT_OBJECT_0) {
                CloseHandle(snapshot);
                return false;
            }
        }
        success = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    return true;
}

[[nodiscard]] auto client_agent_is_running(
    const std::filesystem::path& executable
) -> bool
{
    const auto target = normalized_path_for_compare(executable);
    const auto snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );
    if (snapshot == INVALID_HANDLE_VALUE) {
        return true;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    auto success = Process32FirstW(snapshot, &entry) != FALSE;
    while (success) {
        const auto image = process_image_path(entry.th32ProcessID);
        if (image.has_value()
            && normalized_path_for_compare(*image) == target) {
            CloseHandle(snapshot);
            return true;
        }
        success = Process32NextW(snapshot, &entry) != FALSE;
    }
    CloseHandle(snapshot);
    return false;
}

[[nodiscard]] auto module_directory() -> std::filesystem::path
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );
    if (length == 0 || length == buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path{buffer}.parent_path();
}

[[nodiscard]] auto install_embedded_payload(
    const std::filesystem::path& game_root
) -> palverify::InstallResult
{
    const auto instance = GetModuleHandleW(nullptr);
    const auto resource = instance != nullptr
        ? FindResourceW(
              instance,
              MAKEINTRESOURCEW(IDR_PALVERIFY_PAYLOAD),
              MAKEINTRESOURCEW(10)
          )
        : nullptr;
    if (resource == nullptr) {
        return {
            .success = false,
            .detail = "embedded-payload-missing",
        };
    }
    const auto loaded = LoadResource(instance, resource);
    const auto size = SizeofResource(instance, resource);
    const auto* data = loaded != nullptr
        ? static_cast<const std::byte*>(LockResource(loaded))
        : nullptr;
    if (data == nullptr || size == 0) {
        return {
            .success = false,
            .detail = "embedded-payload-unreadable",
        };
    }

    const auto payload = palverify::unpack_payload_archive(
        std::span<const std::byte>{data, size}
    );
    if (!payload.success) {
        return {
            .success = false,
            .detail = payload.detail,
        };
    }
    const auto client_executable =
        game_root / "Mods" / "Workshop" / "PalVerify" / "client"
        / "Scripts" / "PalVerifyClient.exe";
    if (!stop_running_client(client_executable)) {
        return {
            .success = false,
            .detail = "CLIENT_STOP_BEFORE_INSTALL_FAILED",
        };
    }
    return palverify::install_palverify_payload(
        game_root,
        std::span<const palverify::PayloadFile>{payload.files}
    );
}

[[nodiscard]] auto wide_to_utf8(std::wstring_view value) -> std::string
{
    if (value.empty()) {
        return {};
    }
    const auto size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );
    return result;
}

[[nodiscard]] auto utf8_to_wide(std::string_view value) -> std::wstring
{
    if (value.empty()) {
        return {};
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
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size
    );
    return result;
}

void assign_support_log(
    Snapshot& snapshot,
    std::string_view code,
    std::string_view detail
)
{
    snapshot.support_log = utf8_to_wide(
        palverify::build_launcher_support_log({
            .code = std::string{code},
            .detail = std::string{detail},
            .launcher_version = std::string{launcher_version},
            .palverify_version = std::string{palverify_version},
            .local_palworld_build = snapshot.steam.build_id,
            .required_palworld_build =
                snapshot.manifest.has_value()
                ? snapshot.manifest->required_palworld_build_id
                : std::string{},
        })
    );
}

[[nodiscard]] auto copy_text_to_clipboard(
    HWND owner,
    std::wstring_view text
) -> bool
{
    if (text.empty() || OpenClipboard(owner) == FALSE) {
        return false;
    }
    if (EmptyClipboard() == FALSE) {
        CloseClipboard();
        return false;
    }
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    const auto destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

[[nodiscard]] auto parse_arguments() -> Arguments
{
    int count = 0;
    auto** values = CommandLineToArgvW(GetCommandLineW(), &count);
    Arguments arguments;
    if (values == nullptr) {
        return arguments;
    }
    for (int index = 1; index < count; ++index) {
        const std::wstring_view value{values[index]};
        if (value == L"--install-only") {
            arguments.install_only = true;
        } else if (value == L"--check-manifest") {
            arguments.check_manifest = true;
        } else if (value == L"--silent") {
            arguments.silent = true;
        } else if (value == L"--no-auto-update") {
            arguments.disable_auto_update = true;
        } else if (value == L"--game-root" && index + 1 < count) {
            arguments.game_root = values[++index];
        } else if (value == L"--manifest-url" && index + 1 < count) {
            arguments.manifest_url = wide_to_utf8(values[++index]);
        }
    }
    LocalFree(values);
    return arguments;
}

[[nodiscard]] auto steam_root_from_registry()
    -> std::optional<std::filesystem::path>
{
    DWORD bytes = 0;
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam",
            L"SteamPath",
            RRF_RT_REG_SZ,
            nullptr,
            nullptr,
            &bytes
        )
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam",
            L"SteamPath",
            RRF_RT_REG_SZ,
            nullptr,
            value.data(),
            &bytes
        )
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen_s(value.data(), value.size()));
    return std::filesystem::path{value};
}

[[nodiscard]] auto discover_palworld_install()
    -> std::optional<std::filesystem::path>
{
    const auto steam_root = steam_root_from_registry();
    if (!steam_root.has_value()) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> libraries{*steam_root};
    std::ifstream input{
        *steam_root / "steamapps" / "libraryfolders.vdf",
        std::ios::binary,
    };
    if (input) {
        const std::string vdf{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{},
        };
        auto parsed = palverify::extract_steam_library_paths(vdf);
        libraries.insert(libraries.end(), parsed.begin(), parsed.end());
    }
    std::ranges::sort(libraries);
    const auto unique_end = std::ranges::unique(libraries).begin();
    libraries.erase(unique_end, libraries.end());
    return palverify::find_palworld_install(libraries);
}

[[nodiscard]] auto steam_state(
    const std::filesystem::path& game_root
) -> palverify::SteamAppState
{
    const auto manifest =
        game_root.parent_path().parent_path() / "appmanifest_1623730.acf";
    std::ifstream input{manifest, std::ios::binary};
    if (!input) {
        return {.installed = false, .build_id = {}, .update_pending = false};
    }
    const std::string vdf{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    return palverify::parse_steam_app_state(vdf).value_or(
        palverify::SteamAppState{
            .installed = false,
            .build_id = {},
            .update_pending = false,
        }
    );
}

[[nodiscard]] auto crack_url(
    std::string_view value,
    std::wstring& host,
    std::wstring& path,
    INTERNET_PORT& port,
    bool& secure
) -> bool
{
    const auto url = utf8_to_wide(value);
    if (url.empty()) {
        return false;
    }
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(
            url.c_str(),
            static_cast<DWORD>(url.size()),
            0,
            &parts
        )
        == FALSE) {
        return false;
    }
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength != 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }
    port = parts.nPort;
    secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return secure
        || ((host == L"127.0.0.1" || host == L"localhost")
            && parts.nScheme == INTERNET_SCHEME_HTTP);
}

void record_http_failure(std::string_view operation, DWORD error)
{
    last_http_error = error;
    last_http_failure.assign(operation);
    last_http_failure.append("-win32-");
    last_http_failure.append(std::to_string(error));
}

[[nodiscard]] auto query_header(
    HINTERNET request,
    DWORD query
) -> std::optional<std::wstring>
{
    DWORD bytes = 0;
    SetLastError(ERROR_SUCCESS);
    if (WinHttpQueryHeaders(
            request,
            query,
            WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr,
            &bytes,
            WINHTTP_NO_HEADER_INDEX
        )
        != FALSE
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(
            request,
            query,
            WINHTTP_HEADER_NAME_BY_INDEX,
            value.data(),
            &bytes,
            WINHTTP_NO_HEADER_INDEX
        )
        == FALSE) {
        return std::nullopt;
    }
    value.resize(wcsnlen_s(value.data(), value.size()));
    return value;
}

[[nodiscard]] auto open_http_get(
    HINTERNET session,
    std::string_view initial_url
) -> std::optional<OpenHttpResponse>
{
    constexpr std::size_t maximum_redirects = 5;
    auto current_url = std::string{initial_url};
    for (std::size_t redirect = 0; redirect <= maximum_redirects; ++redirect) {
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port = 0;
        bool secure = false;
        if (!crack_url(current_url, host, path, port, secure)) {
            last_http_failure = "invalid-or-insecure-url";
            return std::nullopt;
        }

        OpenHttpResponse response;
        response.connection =
            WinHttpConnect(session, host.c_str(), port, 0);
        if (response.connection == nullptr) {
            record_http_failure("connect", GetLastError());
            return std::nullopt;
        }
        response.request = WinHttpOpenRequest(
            response.connection,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0
        );
        if (response.request == nullptr) {
            record_http_failure("open-request", GetLastError());
            return std::nullopt;
        }
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (WinHttpSetOption(
                response.request,
                WINHTTP_OPTION_REDIRECT_POLICY,
                &redirect_policy,
                sizeof(redirect_policy)
            )
            == FALSE) {
            record_http_failure("redirect-policy", GetLastError());
            return std::nullopt;
        }
        if (WinHttpSendRequest(
                response.request,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0
            )
            == FALSE) {
            record_http_failure("send", GetLastError());
            return std::nullopt;
        }
        if (WinHttpReceiveResponse(response.request, nullptr) == FALSE) {
            record_http_failure("receive", GetLastError());
            return std::nullopt;
        }

        DWORD status_size = sizeof(response.status);
        if (WinHttpQueryHeaders(
                response.request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &response.status,
                &status_size,
                WINHTTP_NO_HEADER_INDEX
            )
            == FALSE) {
            record_http_failure("status", GetLastError());
            return std::nullopt;
        }

        const auto is_redirect =
            response.status == 301 || response.status == 302
            || response.status == 303 || response.status == 307
            || response.status == 308;
        if (!is_redirect) {
            return response;
        }
        if (redirect == maximum_redirects) {
            last_http_failure = "too-many-redirects";
            return std::nullopt;
        }
        const auto location = query_header(
            response.request,
            WINHTTP_QUERY_LOCATION
        );
        if (!location.has_value()) {
            record_http_failure("redirect-location", GetLastError());
            return std::nullopt;
        }
        const auto redirect_url = palverify::validated_https_redirect(
            wide_to_utf8(*location)
        );
        if (!redirect_url.has_value()) {
            last_http_failure = "invalid-or-insecure-redirect";
            return std::nullopt;
        }
        current_url = *redirect_url;
    }
    last_http_failure = "too-many-redirects";
    return std::nullopt;
}

[[nodiscard]] auto http_get_once(
    std::string_view url
) -> std::optional<HttpResponse>
{
    last_http_failure = "transport-error";
    last_http_error = ERROR_SUCCESS;
    const auto session = WinHttpOpen(
        L"Pal3Mien/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (session == nullptr) {
        record_http_failure("session", GetLastError());
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
    auto response = open_http_get(session, url);
    if (!response.has_value()) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    std::string body;
    {
        for (;;) {
            DWORD available = 0;
            if (WinHttpQueryDataAvailable(
                    response->request,
                    &available
                )
                == FALSE) {
                record_http_failure("query-data", GetLastError());
                body.clear();
                break;
            }
            if (available == 0) {
                break;
            }
            if (body.size() + available > 256 * 1024) {
                last_http_failure = "response-too-large";
                body.clear();
                break;
            }
            const auto offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;
            if (WinHttpReadData(
                    response->request,
                    body.data() + offset,
                    available,
                    &read
                )
                == FALSE) {
                record_http_failure("read", GetLastError());
                body.clear();
                break;
            }
            body.resize(offset + read);
        }
    }
    const auto status = response->status;
    response.reset();
    WinHttpCloseHandle(session);
    if (body.empty()) {
        last_http_failure = "empty-response";
        return std::nullopt;
    }
    return HttpResponse{.status = status, .body = std::move(body)};
}

[[nodiscard]] auto http_get(std::string_view url) -> std::optional<HttpResponse>
{
    constexpr unsigned int maximum_attempts = 3;
    for (unsigned int attempt = 1; attempt <= maximum_attempts; ++attempt) {
        auto response = http_get_once(url);
        const auto status = response.has_value()
            ? std::optional<unsigned long>{response->status}
            : std::nullopt;
        if (!palverify::should_retry_http(
                status,
                last_http_error,
                attempt,
                maximum_attempts
            )) {
            return response;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{500 * attempt}
        );
    }
    return std::nullopt;
}

[[nodiscard]] auto sha256_file(const std::filesystem::path& path)
    -> std::string
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0
        )
        != 0
        || BCryptGetProperty(
               algorithm,
               BCRYPT_OBJECT_LENGTH,
               reinterpret_cast<PUCHAR>(&object_size),
               sizeof(object_size),
               &result_size,
               0
           )
            != 0) {
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return {};
    }
    std::vector<UCHAR> object(object_size);
    std::array<UCHAR, 32> digest{};
    if (BCryptCreateHash(
            algorithm,
            &hash,
            object.data(),
            static_cast<ULONG>(object.size()),
            nullptr,
            0,
            0
        )
        != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::ifstream input{path, std::ios::binary};
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0
            && BCryptHashData(
                   hash,
                   reinterpret_cast<PUCHAR>(buffer.data()),
                   static_cast<ULONG>(count),
                   0
               )
                != 0) {
            input.setstate(std::ios::badbit);
        }
    }
    const auto finished = input.eof()
        && BCryptFinishHash(
               hash,
               digest.data(),
               static_cast<ULONG>(digest.size()),
               0
           )
            == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!finished) {
        return {};
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const auto value : digest) {
        output.push_back(hex[value >> 4U]);
        output.push_back(hex[value & 0x0fU]);
    }
    return output;
}

[[nodiscard]] auto download_update(
    const palverify::LauncherManifest& manifest,
    HWND window
) -> std::optional<std::filesystem::path>
{
    last_http_failure = "transport-error";
    const auto session = WinHttpOpen(
        L"Pal3Mien-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (session == nullptr) {
        record_http_failure("session", GetLastError());
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
    std::optional<OpenHttpResponse> response;
    constexpr unsigned int maximum_attempts = 3;
    for (unsigned int attempt = 1; attempt <= maximum_attempts; ++attempt) {
        response = open_http_get(session, manifest.launcher_download_url);
        const auto status = response.has_value()
            ? std::optional<unsigned long>{response->status}
            : std::nullopt;
        if (!palverify::should_retry_http(
                status,
                last_http_error,
                attempt,
                maximum_attempts
            )) {
            break;
        }
        response.reset();
        std::this_thread::sleep_for(
            std::chrono::milliseconds{500 * attempt}
        );
    }
    if (!response.has_value()
        || response->status < 200 || response->status >= 300) {
        if (response.has_value()) {
            last_http_failure =
                "http-status-" + std::to_string(response->status);
        }
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    const auto target =
        std::filesystem::temp_directory_path() / "Pal3Mien-update.exe";
    std::ofstream output{target, std::ios::binary | std::ios::trunc};
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t received_total = 0;
    bool downloaded = static_cast<bool>(output);
    while (downloaded) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(
                response->request,
                &available
            )
            == FALSE) {
            record_http_failure("query-data", GetLastError());
            downloaded = false;
            break;
        }
        if (available == 0) {
            break;
        }
        while (available != 0) {
            const auto requested = (std::min)(
                available,
                static_cast<DWORD>(buffer.size())
            );
            DWORD read = 0;
            if (WinHttpReadData(
                    response->request,
                    buffer.data(),
                    requested,
                    &read
                )
                == FALSE
                || read == 0) {
                record_http_failure("read", GetLastError());
                downloaded = false;
                break;
            }
            output.write(buffer.data(), read);
            received_total += read;
            available -= read;
            if (!output || received_total > 200ULL * 1024ULL * 1024ULL) {
                downloaded = false;
                break;
            }
            const auto progress = static_cast<WPARAM>(
                (std::min<std::uint64_t>)(
                    89,
                    25 + received_total / (256 * 1024)
                )
            );
            PostMessageW(window, message_progress, progress, 0);
        }
    }
    output.close();
    response.reset();
    WinHttpCloseHandle(session);
    if (!downloaded || received_total == 0) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        return std::nullopt;
    }

    PostMessageW(window, message_progress, 92, 0);
    auto actual = sha256_file(target);
    std::ranges::transform(actual, actual.begin(), [](char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
    });
    auto expected = manifest.launcher_sha256;
    std::ranges::transform(expected, expected.begin(), [](char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
    });
    if (actual != expected) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        return std::nullopt;
    }
    PostMessageW(window, message_progress, 100, 0);
    return target;
}

struct ClientStartProcess {
    bool started{false};
    DWORD exit_code{STILL_ACTIVE};
    DWORD win32_error{ERROR_SUCCESS};
};

[[nodiscard]] auto start_client_agent(
    const std::filesystem::path& game_root
) -> ClientStartProcess
{
    ClientStartProcess result;
    const auto executable =
        game_root / "Mods" / "Workshop" / "PalVerify" / "client"
        / "Scripts" / "PalVerifyClient.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        result.win32_error = ERROR_FILE_NOT_FOUND;
        return result;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto working_directory = executable.parent_path().wstring();
    const auto created = CreateProcessW(
        executable.c_str(),
        nullptr,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process
    );
    if (created == FALSE) {
        result.win32_error = GetLastError();
        return result;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 750);
    if (wait == WAIT_OBJECT_0) {
        static_cast<void>(GetExitCodeProcess(
            process.hProcess,
            &result.exit_code
        ));
        result.started = result.exit_code == 0;
    } else if (wait == WAIT_TIMEOUT) {
        result.started = true;
        result.exit_code = STILL_ACTIVE;
    } else {
        result.win32_error = GetLastError();
    }
    CloseHandle(process.hProcess);
    return result;
}

struct ClientPreflightProcess {
    bool launched{false};
    bool timed_out{false};
    DWORD exit_code{MAXDWORD};
    DWORD win32_error{ERROR_SUCCESS};
    std::string output;
};

[[nodiscard]] auto clean_preflight_output(std::string_view value)
    -> std::string
{
    std::string clean;
    clean.reserve(std::min<std::size_t>(value.size(), 2048));
    bool previous_space = false;
    for (const auto character : value) {
        if (clean.size() == 2048) {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        const auto safe = byte >= 0x20 && byte <= 0x7e;
        const auto output = safe ? character : ' ';
        if (output == ' ') {
            if (clean.empty() || previous_space) {
                continue;
            }
            previous_space = true;
        } else {
            previous_space = false;
        }
        clean.push_back(output);
    }
    while (!clean.empty() && clean.back() == ' ') {
        clean.pop_back();
    }
    return clean;
}

[[nodiscard]] auto run_client_preflight(
    const std::filesystem::path& game_root
) -> ClientPreflightProcess
{
    ClientPreflightProcess result;
    const auto executable =
        game_root / "Mods" / "Workshop" / "PalVerify" / "client"
        / "Scripts" / "PalVerifyClient.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        result.win32_error = ERROR_FILE_NOT_FOUND;
        return result;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &security, 0) == FALSE) {
        result.win32_error = GetLastError();
        return result;
    }
    if (SetHandleInformation(
            read_pipe,
            HANDLE_FLAG_INHERIT,
            0
        )
        == FALSE) {
        result.win32_error = GetLastError();
        CloseHandle(write_pipe);
        CloseHandle(read_pipe);
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    auto command_line =
        L"\"" + executable.wstring() + L"\" --preflight";
    std::vector<wchar_t> command{
        command_line.begin(),
        command_line.end(),
    };
    command.push_back(L'\0');
    const auto working_directory = executable.parent_path().wstring();
    const auto created = CreateProcessW(
        executable.c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process
    );
    CloseHandle(write_pipe);
    if (created == FALSE) {
        result.win32_error = GetLastError();
        CloseHandle(read_pipe);
        return result;
    }
    result.launched = true;
    CloseHandle(process.hThread);

    constexpr DWORD preflight_timeout_milliseconds = 35'000;
    const auto wait =
        WaitForSingleObject(process.hProcess, preflight_timeout_milliseconds);
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
    } else if (wait == WAIT_FAILED) {
        result.win32_error = GetLastError();
    }
    static_cast<void>(GetExitCodeProcess(
        process.hProcess,
        &result.exit_code
    ));
    CloseHandle(process.hProcess);

    std::string output;
    std::array<char, 512> buffer{};
    for (;;) {
        DWORD read = 0;
        if (ReadFile(
                read_pipe,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr
            )
            == FALSE
            || read == 0) {
            break;
        }
        output.append(buffer.data(), read);
        if (output.size() >= 2048) {
            break;
        }
    }
    CloseHandle(read_pipe);
    result.output = clean_preflight_output(output);
    return result;
}

[[nodiscard]] auto preflight_copy(
    const ClientPreflightProcess& preflight
) -> std::pair<std::wstring, std::wstring>
{
    if (!preflight.launched) {
        return {
            L"Không thể chạy kiểm tra PalVerify",
            L"Client chưa khởi động được. Bấm để xem chi tiết lỗi.",
        };
    }
    if (preflight.timed_out) {
        return {
            L"Máy chủ xác minh không phản hồi",
            L"Kiểm tra Internet rồi bấm làm mới để thử lại.",
        };
    }
    switch (static_cast<palverify::ClientPreflightExit>(
        preflight.exit_code
    )) {
    case palverify::ClientPreflightExit::integrity_violation:
        return {
            L"Phát hiện phần mềm can thiệp",
            L"Tắt phần mềm liên quan, thoát hẳn game rồi kiểm tra lại.",
        };
    case palverify::ClientPreflightExit::unapproved_mod:
        return {
            L"PalVerify chưa được máy chủ chấp nhận",
            L"Phiên bản hoặc hash client không khớp chính sách server.",
        };
    case palverify::ClientPreflightExit::invalid_config:
    case palverify::ClientPreflightExit::game_root_unavailable:
    case palverify::ClientPreflightExit::steam_user_unavailable:
        return {
            L"Cấu hình PalVerify chưa hợp lệ",
            L"Mở Steam đúng tài khoản rồi bấm làm mới để tự sửa.",
        };
    case palverify::ClientPreflightExit::scan_unavailable:
        return {
            L"Không thể quét trạng thái máy",
            L"Thử mở launcher bằng quyền thường và tắt game trước khi kiểm tra.",
        };
    case palverify::ClientPreflightExit::transport_failed:
    case palverify::ClientPreflightExit::http_rejected:
    case palverify::ClientPreflightExit::invalid_response:
        return {
            L"Không thể xác minh với máy chủ",
            L"Kiểm tra Internet rồi bấm làm mới để thử lại.",
        };
    case palverify::ClientPreflightExit::accepted:
        break;
    case palverify::ClientPreflightExit::rejected:
        break;
    }
    return {
        L"PalVerify bị máy chủ từ chối",
        L"Bấm để xem mã lỗi và gửi cho admin.",
    };
}

[[nodiscard]] auto shell_open_steam_uri(
    HWND owner,
    std::wstring_view uri
) -> INT_PTR
{
    const auto result = ShellExecuteW(
        owner,
        L"open",
        std::wstring{uri}.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
    return reinterpret_cast<INT_PTR>(result);
}

[[nodiscard]] auto launch_palworld_through_steam(HWND owner) -> INT_PTR
{
    const auto uri = utf8_to_wide(palverify::steam_launch_uri());
    INT_PTR last_error = 0;
    for (unsigned int attempt = 1; attempt <= game_launch_attempts;
         ++attempt) {
        const auto result = shell_open_steam_uri(owner, uri);
        if (result > 32) {
            return result;
        }
        last_error = result;
        if (attempt < game_launch_attempts) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{350 * attempt}
            );
        }
    }
    return last_error;
}

[[nodiscard]] auto status_copy(
    palverify::LauncherStatus status,
    const palverify::SteamAppState& steam,
    const palverify::LauncherManifest& manifest
) -> std::pair<std::wstring, std::wstring>
{
    switch (status) {
    case palverify::LauncherStatus::Ready:
        return {
            L"Phiên bản của bạn đã mới nhất!",
            L"PalVerify và Palworld đã sẵn sàng.",
        };
    case palverify::LauncherStatus::LauncherUpdateRequired:
        return {
            L"Có bản cập nhật bắt buộc",
            L"Launcher đang tải và xác minh bản mới.",
        };
    case palverify::LauncherStatus::GameUpdateRequired:
        if (!steam.build_id.empty()
            && steam.build_id > manifest.required_palworld_build_id) {
            return {
                L"Server chưa cập nhật Palworld",
                L"Vui lòng chờ server hoàn tất cập nhật.",
            };
        }
        return {
            L"Palworld cần được cập nhật",
            L"Mở Steam và hoàn tất update trước khi chơi.",
        };
    case palverify::LauncherStatus::GameMissing:
        return {
            L"Không tìm thấy Palworld",
            L"Hãy cài Palworld Steam trên máy này.",
        };
    case palverify::LauncherStatus::ServerUnavailable:
        return {
            L"Máy chủ đang ngoại tuyến",
            L"Launcher sẽ tự kiểm tra lại sau 8 giây.",
        };
    }
    return {L"Không thể kiểm tra", L"Hãy thử lại."};
}

class LauncherApp {
public:
    LauncherApp(HINSTANCE instance, Arguments arguments)
        : instance_{instance},
          arguments_{std::move(arguments)},
          assets_{instance}
    {
    }

    void attach(HWND window)
    {
        window_ = window;
    }

    void refresh()
    {
        if (busy_.exchange(true)) {
            return;
        }
        snapshot_.checking = true;
        snapshot_.updating = false;
        snapshot_.progress = 12;
        snapshot_.headline = L"Đang kiểm tra phiên bản...";
        snapshot_.detail = L"Đang kết nối máy chủ Palworld 3 Miền.";
        snapshot_.support_log.clear();
        InvalidateRect(window_, nullptr, FALSE);

        worker_ = std::jthread([this](std::stop_token) {
            auto result = std::make_unique<Snapshot>();
            const auto response = http_get(arguments_.manifest_url);
            if (!response.has_value() || response->status != 200) {
                result->headline = L"Không thể kiểm tra cập nhật";
                result->detail =
                    L"Kiểm tra Internet rồi bấm nút làm mới.";
                result->progress = 0;
                result->checking = false;
                assign_support_log(
                    *result,
                    "MANIFEST_REQUEST_FAILED",
                    response.has_value()
                        ? "http-status-" + std::to_string(response->status)
                        : last_http_failure
                );
                post_snapshot(std::move(result));
                return;
            }
            auto manifest_body = response->body;
            const auto release_asset =
                palverify::github_release_asset_url(
                    manifest_body,
                    release_manifest_asset_name
                );
            if (release_asset.has_value()) {
                const auto asset_response = http_get(*release_asset);
                if (!asset_response.has_value()
                    || asset_response->status != 200) {
                    result->headline =
                        L"Không thể tải manifest từ GitHub";
                    result->detail =
                        L"Kiểm tra Internet rồi bấm nút làm mới.";
                    result->progress = 0;
                    result->checking = false;
                    assign_support_log(
                        *result,
                        "GITHUB_RELEASE_ASSET_FAILED",
                        asset_response.has_value()
                            ? "http-status-"
                                + std::to_string(asset_response->status)
                            : last_http_failure
                    );
                    post_snapshot(std::move(result));
                    return;
                }
                manifest_body = asset_response->body;
            }
            result->manifest =
                palverify::parse_launcher_manifest(manifest_body);
            if (!result->manifest.has_value()) {
                result->headline = L"Manifest cập nhật không hợp lệ";
                result->detail =
                    L"Launcher đã khóa để bảo vệ phiên chơi.";
                result->progress = 0;
                result->checking = false;
                assign_support_log(
                    *result,
                    "MANIFEST_INVALID",
                    "manifest-parse-or-validation-failed"
                );
                post_snapshot(std::move(result));
                return;
            }
            result->game_root = arguments_.game_root.has_value()
                ? arguments_.game_root
                : discover_palworld_install();
            if (result->game_root.has_value()) {
                result->steam = steam_state(*result->game_root);
            }
            result->status = palverify::evaluate_launcher(
                launcher_version,
                palverify_version,
                result->steam,
                *result->manifest
            );
            const auto [headline, detail] = status_copy(
                result->status,
                result->steam,
                *result->manifest
            );
            result->headline = headline;
            result->detail = detail;
            result->progress =
                result->status == palverify::LauncherStatus::Ready ? 70 : 55;
            result->checking = false;
            if (result->status == palverify::LauncherStatus::Ready) {
                const auto game_running = palworld_is_running();
                if (!palverify::launcher_can_prepare_payload(
                        result->status,
                        game_running
                    )) {
                    result->waiting_for_game_exit = true;
                    result->headline = L"Hãy thoát hoàn toàn Palworld";
                    result->detail =
                        L"Launcher sẽ tự cài UE4SS sau khi game đã đóng.";
                } else {
                    const auto install =
                        install_embedded_payload(*result->game_root);
                    if (!install.success) {
                        result->headline = L"Không thể cài PalVerify";
                        result->detail =
                            L"Kiểm tra quyền ghi thư mục game rồi thử lại.";
                        result->progress = 0;
                        assign_support_log(
                            *result,
                            "INSTALL_PAYLOAD_FAILED",
                            install.detail
                        );
                    } else {
                        result->payload_installed = true;
                        auto preflight =
                            run_client_preflight(*result->game_root);
                        const auto preflight_accepted =
                            [](const ClientPreflightProcess& value) {
                                return value.launched
                                    && !value.timed_out
                                    && value.exit_code
                                        == static_cast<DWORD>(
                                            palverify::ClientPreflightExit::
                                                accepted
                                        );
                            };
                        bool remediation_failed = false;
                        bool remediated_unapproved_mod = false;
                        if (!preflight_accepted(preflight)) {
                            const auto unapproved_mod_ids =
                                palverify::
                                    extract_not_whitelisted_mod_ids(
                                        preflight.output
                                    );
                            if (!unapproved_mod_ids.empty()) {
                                const auto remediation =
                                    palverify::quarantine_unapproved_mods(
                                        *result->game_root,
                                        std::span<const std::string>{
                                            unapproved_mod_ids
                                        }
                                    );
                                if (!remediation.success) {
                                    remediation_failed = true;
                                    result->headline =
                                        L"Không thể cách ly mod không hợp lệ";
                                    result->detail =
                                        L"Kiểm tra quyền ghi thư mục game rồi thử lại.";
                                    result->progress = 85;
                                    assign_support_log(
                                        *result,
                                        "MOD_REMEDIATION_FAILED",
                                        remediation.detail
                                    );
                                } else if (remediation.quarantined > 0) {
                                    const auto reinstall =
                                        install_embedded_payload(
                                            *result->game_root
                                        );
                                    if (!reinstall.success) {
                                        remediation_failed = true;
                                        result->headline =
                                            L"Không thể khôi phục mod bắt buộc";
                                        result->detail =
                                            L"Kiểm tra quyền ghi thư mục game rồi thử lại.";
                                        result->progress = 85;
                                        assign_support_log(
                                            *result,
                                            "MOD_REINSTALL_FAILED",
                                            reinstall.detail
                                        );
                                    } else {
                                        remediated_unapproved_mod = true;
                                        preflight = run_client_preflight(
                                            *result->game_root
                                        );
                                    }
                                }
                            }
                        }
                        result->preflight_succeeded =
                            !remediation_failed
                            && preflight_accepted(preflight);
                        if (remediation_failed) {
                            post_snapshot(std::move(result));
                            return;
                        }
                        if (result->preflight_succeeded) {
                            const auto client_start =
                                start_client_agent(*result->game_root);
                            if (!client_start.started) {
                                result->preflight_succeeded = false;
                                result->headline =
                                    L"Không thể duy trì PalVerify";
                                result->detail =
                                    L"Client đã thoát sớm. Bấm để xem mã lỗi.";
                                result->progress = 85;
                                const auto start_detail =
                                    client_start.exit_code != STILL_ACTIVE
                                    ? "early-exit-"
                                        + std::to_string(
                                            client_start.exit_code
                                        )
                                    : "create-process-failed-"
                                        + std::to_string(
                                            client_start.win32_error
                                        );
                                assign_support_log(
                                    *result,
                                    "CLIENT_AUTOSTART_FAILED",
                                    start_detail
                                );
                            } else if (remediated_unapproved_mod) {
                                result->headline =
                                    L"Đã tự cách ly mod không hợp lệ";
                                result->detail =
                                    L"Mod ngoài whitelist đã được gỡ khỏi phiên chơi.";
                            } else {
                                result->headline =
                                    L"PalVerify đã được máy chủ xác nhận!";
                                result->detail =
                                    L"Client, phiên bản và hash đều hợp lệ.";
                            }
                            result->progress = 100;
                        } else {
                            const auto [
                                preflight_headline,
                                preflight_detail
                            ] = preflight_copy(preflight);
                            result->headline = preflight_headline;
                            result->detail = preflight_detail;
                            result->progress = 85;
                            std::string failure_detail =
                                preflight.output.empty()
                                ? "exit-code-"
                                    + std::to_string(preflight.exit_code)
                                : preflight.output;
                            if (!preflight.launched) {
                                failure_detail =
                                    "create-process-failed-"
                                    + std::to_string(
                                        preflight.win32_error
                                    );
                            } else if (preflight.timed_out) {
                                failure_detail = "preflight-timeout";
                            }
                            assign_support_log(
                                *result,
                                "CLIENT_PREFLIGHT_FAILED",
                                failure_detail
                            );
                        }
                    }
                }
            }
            post_snapshot(std::move(result));
        });
    }

    void begin_mandatory_update()
    {
        if (arguments_.disable_auto_update || !snapshot_.manifest.has_value()
            || busy_.exchange(true)) {
            return;
        }
        snapshot_.updating = true;
        snapshot_.progress = 25;
        snapshot_.headline = L"Đang tải bản launcher mới";
        snapshot_.detail =
            L"Launcher sẽ tự đóng để cài bản mới.";
        InvalidateRect(window_, nullptr, FALSE);
        const auto manifest = *snapshot_.manifest;
        worker_ = std::jthread([this, manifest](std::stop_token) {
            const auto update = download_update(manifest, window_);
            if (!update.has_value()) {
                PostMessageW(window_, message_update_failed, 0, 0);
                return;
            }
            const auto result = ShellExecuteW(
                window_,
                L"open",
                update->c_str(),
                L"/S /UPDATE=1",
                update->parent_path().c_str(),
                SW_HIDE
            );
            if (reinterpret_cast<INT_PTR>(result) <= 32) {
                PostMessageW(window_, message_update_failed, 0, 0);
                return;
            }
            PostMessageW(window_, WM_CLOSE, 0, 0);
        });
    }

    void apply_snapshot(std::unique_ptr<Snapshot> next)
    {
        snapshot_ = std::move(*next);
        busy_ = false;
        InvalidateRect(window_, nullptr, FALSE);
        if (snapshot_.status
                == palverify::LauncherStatus::LauncherUpdateRequired
            && !snapshot_.checking) {
            begin_mandatory_update();
        }
        if (snapshot_.status
                == palverify::LauncherStatus::GameUpdateRequired
            || snapshot_.status
                == palverify::LauncherStatus::ServerUnavailable
            || snapshot_.waiting_for_game_exit) {
            SetTimer(
                window_,
                timer_recheck,
                snapshot_.waiting_for_game_exit ? 2000 : 8000,
                nullptr
            );
        } else {
            KillTimer(window_, timer_recheck);
        }
    }

    void update_failed()
    {
        busy_ = false;
        snapshot_.updating = false;
        snapshot_.progress = 0;
        snapshot_.headline = L"Cập nhật launcher thất bại";
        snapshot_.detail =
            L"Bấm làm mới để tải lại; không thể chơi bản cũ.";
        assign_support_log(
            snapshot_,
            "LAUNCHER_UPDATE_FAILED",
            "download-verify-or-start-failed"
        );
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_progress(int progress)
    {
        snapshot_.progress = std::clamp(progress, 0, 100);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void supervise_client()
    {
        if (!snapshot_.preflight_succeeded
            || !snapshot_.game_root.has_value()
            || !palworld_is_running()) {
            client_recovery_attempted_ = false;
            return;
        }
        const auto executable =
            *snapshot_.game_root / "Mods" / "Workshop" / "PalVerify"
            / "client" / "Scripts" / "PalVerifyClient.exe";
        if (client_agent_is_running(executable)) {
            client_recovery_attempted_ = false;
            return;
        }
        if (client_recovery_attempted_) {
            return;
        }
        client_recovery_attempted_ = true;
        const auto client_start =
            start_client_agent(*snapshot_.game_root);
        if (client_start.started) {
            return;
        }
        snapshot_.headline = L"PalVerify đã dừng giữa phiên";
        snapshot_.detail =
            L"Không thể tự khởi động lại client. Bấm để xem mã lỗi.";
        const auto start_detail =
            client_start.exit_code != STILL_ACTIVE
            ? "early-exit-" + std::to_string(client_start.exit_code)
            : "create-process-failed-"
                + std::to_string(client_start.win32_error);
        assign_support_log(
            snapshot_,
            "CLIENT_RECOVERY_FAILED",
            start_detail
        );
        InvalidateRect(window_, nullptr, FALSE);
    }

    void start_game()
    {
        if (snapshot_.status == palverify::LauncherStatus::GameUpdateRequired) {
            ShellExecuteW(
                window_,
                L"open",
                L"steam://install/1623730",
                nullptr,
                nullptr,
                SW_SHOWNORMAL
            );
            return;
        }
        if (!palverify::launcher_can_start(
                snapshot_.status,
                snapshot_.payload_installed,
                snapshot_.preflight_succeeded
            )
            || !snapshot_.game_root.has_value()
            || !snapshot_.manifest.has_value()) {
            return;
        }
        const auto executable =
            *snapshot_.game_root / "Mods" / "Workshop" / "PalVerify"
            / "client" / "Scripts" / "PalVerifyClient.exe";
        if (!client_agent_is_running(executable)) {
            const auto client_start =
                start_client_agent(*snapshot_.game_root);
            if (!client_start.started) {
                snapshot_.headline = L"Không thể mở PalVerify";
                snapshot_.detail =
                    L"Client đã thoát sớm. Bấm để xem mã lỗi.";
                const auto start_detail =
                    client_start.exit_code != STILL_ACTIVE
                    ? "early-exit-"
                        + std::to_string(client_start.exit_code)
                    : "create-process-failed-"
                        + std::to_string(client_start.win32_error);
                assign_support_log(
                    snapshot_,
                    "CLIENT_START_FAILED",
                    start_detail
                );
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
        client_recovery_attempted_ = false;
        snapshot_.headline = L"PalVerify đã sẵn sàng";
        snapshot_.detail = L"Đang mở Palworld qua Steam.";
        InvalidateRect(window_, nullptr, FALSE);

        const auto steam_result =
            launch_palworld_through_steam(window_);
        if (steam_result <= 32) {
            snapshot_.headline = L"Không thể mở Palworld qua Steam";
            snapshot_.detail =
                L"Kiểm tra Steam đang chạy rồi bấm BẮT ĐẦU lại.";
            assign_support_log(
                snapshot_,
                "GAME_START_FAILED",
                "steam-shell-error-"
                    + std::to_string(steam_result)
            );
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        ShowWindow(window_, SW_MINIMIZE);
    }

    [[nodiscard]] auto has_error_details() const -> bool
    {
        return !snapshot_.support_log.empty();
    }

    void show_error_details() const
    {
        if (snapshot_.support_log.empty()) {
            return;
        }
        const auto copied =
            copy_text_to_clipboard(window_, snapshot_.support_log);
        auto message = snapshot_.support_log;
        message += copied
            ? L"\r\nLog đã được sao chép. Hãy dán và gửi cho admin."
            : L"\r\nKhông thể tự sao chép. Nhấn Ctrl+C để chép hộp thoại.";
        MessageBoxW(
            window_,
            message.c_str(),
            L"Chi tiết lỗi PalVerify",
            MB_OK | MB_ICONERROR
        );
    }

    void open_website() const
    {
        const auto url = snapshot_.manifest.has_value()
            ? utf8_to_wide(snapshot_.manifest->website_url)
            : default_website_url;
        ShellExecuteW(
            window_,
            L"open",
            url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
    }

    void open_news() const
    {
        const auto url = snapshot_.manifest.has_value()
            ? utf8_to_wide(snapshot_.manifest->news_url)
            : default_website_url;
        ShellExecuteW(
            window_,
            L"open",
            url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
    }

    [[nodiscard]] auto snapshot() const -> const Snapshot&
    {
        return snapshot_;
    }

    [[nodiscard]] auto assets() const -> const Assets&
    {
        return assets_;
    }

    [[nodiscard]] auto hovered_button() const -> InteractiveButton
    {
        return hovered_button_;
    }

    [[nodiscard]] auto pressed_button() const -> InteractiveButton
    {
        return pressed_button_;
    }

    void set_hovered_button(InteractiveButton button)
    {
        if (hovered_button_ == button) {
            return;
        }
        hovered_button_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_pressed_button(InteractiveButton button)
    {
        if (pressed_button_ == button) {
            return;
        }
        pressed_button_ = button;
        InvalidateRect(window_, nullptr, FALSE);
    }

private:
    void post_snapshot(std::unique_ptr<Snapshot> snapshot)
    {
        if (IsWindow(window_) != FALSE) {
            PostMessageW(
                window_,
                message_snapshot,
                0,
                reinterpret_cast<LPARAM>(snapshot.release())
            );
        }
    }

    HINSTANCE instance_{};
    Arguments arguments_;
    Assets assets_;
    HWND window_{};
    Snapshot snapshot_;
    InteractiveButton hovered_button_{InteractiveButton::None};
    InteractiveButton pressed_button_{InteractiveButton::None};
    std::atomic_bool busy_{false};
    bool client_recovery_attempted_{false};
    std::jthread worker_;
};

[[nodiscard]] auto scaled_rect(
    const RECT& client,
    float left,
    float top,
    float width,
    float height
) -> Gdiplus::RectF
{
    const auto sx =
        static_cast<float>(client.right - client.left) / design_width;
    const auto sy =
        static_cast<float>(client.bottom - client.top) / design_height;
    return {left * sx, top * sy, width * sx, height * sy};
}

[[nodiscard]] auto hit(
    const RECT& client,
    POINT point,
    float left,
    float top,
    float width,
    float height
) -> bool
{
    const auto rectangle =
        scaled_rect(client, left, top, width, height);
    return rectangle.Contains(
        static_cast<Gdiplus::REAL>(point.x),
        static_cast<Gdiplus::REAL>(point.y)
    );
}

void draw_image(
    Gdiplus::Graphics& graphics,
    Gdiplus::Image* image,
    const Gdiplus::RectF& rectangle
)
{
    if (image != nullptr) {
        graphics.DrawImage(image, rectangle);
    }
}

void draw_status_icon(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rectangle,
    StatusIcon icon
)
{
    const auto scale = rectangle.Width / 62.0F;
    const auto green = Gdiplus::Color{255, 36, 238, 116};
    const auto yellow = Gdiplus::Color{255, 255, 191, 62};
    const auto red = Gdiplus::Color{255, 255, 82, 82};
    const auto color = icon == StatusIcon::Success
        ? green
        : (icon == StatusIcon::Warning ? yellow : red);
    Gdiplus::Pen pen{color, 4.0F * scale};
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    const auto inset = 5.0F * scale;
    graphics.DrawEllipse(
        &pen,
        rectangle.X + inset,
        rectangle.Y + inset,
        rectangle.Width - 2.0F * inset,
        rectangle.Height - 2.0F * inset
    );

    if (icon == StatusIcon::Success) {
        const Gdiplus::PointF points[]{
            {rectangle.X + 17.0F * scale, rectangle.Y + 32.0F * scale},
            {rectangle.X + 27.0F * scale, rectangle.Y + 42.0F * scale},
            {rectangle.X + 46.0F * scale, rectangle.Y + 21.0F * scale},
        };
        graphics.DrawLines(&pen, points, 3);
        return;
    }
    if (icon == StatusIcon::Warning) {
        graphics.DrawLine(
            &pen,
            rectangle.X + 31.0F * scale,
            rectangle.Y + 18.0F * scale,
            rectangle.X + 31.0F * scale,
            rectangle.Y + 36.0F * scale
        );
        Gdiplus::SolidBrush dot{color};
        graphics.FillEllipse(
            &dot,
            rectangle.X + 28.0F * scale,
            rectangle.Y + 43.0F * scale,
            6.0F * scale,
            6.0F * scale
        );
        return;
    }

    graphics.DrawLine(
        &pen,
        rectangle.X + 21.0F * scale,
        rectangle.Y + 21.0F * scale,
        rectangle.X + 41.0F * scale,
        rectangle.Y + 41.0F * scale
    );
    graphics.DrawLine(
        &pen,
        rectangle.X + 41.0F * scale,
        rectangle.Y + 21.0F * scale,
        rectangle.X + 21.0F * scale,
        rectangle.Y + 41.0F * scale
    );
}

void draw_text(
    Gdiplus::Graphics& graphics,
    std::wstring_view text,
    const Gdiplus::RectF& rectangle,
    float size,
    Gdiplus::Color color,
    Gdiplus::FontStyle style = Gdiplus::FontStyleBold,
    Gdiplus::StringAlignment alignment = Gdiplus::StringAlignmentNear
)
{
    Gdiplus::FontFamily family{L"Segoe UI"};
    Gdiplus::Font font{&family, size, style, Gdiplus::UnitPixel};
    Gdiplus::SolidBrush brush{color};
    Gdiplus::StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    graphics.DrawString(
        text.data(),
        static_cast<INT>(text.size()),
        &font,
        rectangle,
        &format,
        &brush
    );
}

void add_rounded_rectangle(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rectangle,
    float radius
)
{
    const auto diameter = std::min(
        radius * 2.0F,
        std::min(rectangle.Width, rectangle.Height)
    );
    const auto right = rectangle.X + rectangle.Width;
    const auto bottom = rectangle.Y + rectangle.Height;
    path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180, 90);
    path.AddArc(
        right - diameter,
        rectangle.Y,
        diameter,
        diameter,
        270,
        90
    );
    path.AddArc(
        right - diameter,
        bottom - diameter,
        diameter,
        diameter,
        0,
        90
    );
    path.AddArc(
        rectangle.X,
        bottom - diameter,
        diameter,
        diameter,
        90,
        90
    );
    path.CloseFigure();
}

void draw_glass_panel(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rectangle,
    float radius,
    bool prominent = false
)
{
    Gdiplus::GraphicsPath path;
    add_rounded_rectangle(path, rectangle, radius);
    const auto top = prominent
        ? Gdiplus::Color{238, 7, 53, 80}
        : Gdiplus::Color{232, 4, 34, 53};
    const auto bottom = prominent
        ? Gdiplus::Color{245, 2, 28, 47}
        : Gdiplus::Color{242, 1, 25, 42};
    Gdiplus::LinearGradientBrush fill{
        rectangle,
        top,
        bottom,
        Gdiplus::LinearGradientModeVertical,
    };
    graphics.FillPath(&fill, &path);
    Gdiplus::Pen outer{Gdiplus::Color{220, 24, 187, 226}, 1.5F};
    graphics.DrawPath(&outer, &path);
    Gdiplus::Pen inner{Gdiplus::Color{75, 166, 235, 255}, 1.0F};
    auto inner_rectangle = rectangle;
    inner_rectangle.Inflate(-3.0F, -3.0F);
    Gdiplus::GraphicsPath inner_path;
    add_rounded_rectangle(inner_path, inner_rectangle, radius - 2.0F);
    graphics.DrawPath(&inner, &inner_path);
}

void draw_action_button(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rectangle,
    float radius
)
{
    Gdiplus::GraphicsPath path;
    add_rounded_rectangle(path, rectangle, radius);
    Gdiplus::LinearGradientBrush fill{
        rectangle,
        Gdiplus::Color{255, 7, 58, 87},
        Gdiplus::Color{255, 2, 31, 54},
        Gdiplus::LinearGradientModeVertical,
    };
    graphics.FillPath(&fill, &path);

    Gdiplus::Pen border{Gdiplus::Color{255, 35, 211, 255}, 2.0F};
    graphics.DrawPath(&border, &path);

}

[[nodiscard]] auto button_render_rect(
    Gdiplus::RectF rectangle,
    bool pressed
) -> Gdiplus::RectF
{
    if (pressed) {
        rectangle.Inflate(-2.0F, -2.0F);
        rectangle.Y += 2.0F;
    }
    return rectangle;
}

void draw_button_interaction(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rectangle,
    float radius,
    bool hovered,
    bool pressed
)
{
    if (!hovered && !pressed) {
        return;
    }

    Gdiplus::GraphicsPath path;
    add_rounded_rectangle(path, rectangle, radius);
    if (pressed) {
        Gdiplus::SolidBrush shade{Gdiplus::Color{72, 0, 9, 18}};
        graphics.FillPath(&shade, &path);
        Gdiplus::Pen edge{Gdiplus::Color{245, 116, 231, 255}, 1.5F};
        graphics.DrawPath(&edge, &path);
        return;
    }

    auto glow_rectangle = rectangle;
    glow_rectangle.Inflate(3.0F, 3.0F);
    Gdiplus::GraphicsPath glow_path;
    add_rounded_rectangle(glow_path, glow_rectangle, radius + 3.0F);
    Gdiplus::Pen glow{Gdiplus::Color{100, 29, 207, 255}, 5.0F};
    graphics.DrawPath(&glow, &glow_path);

    Gdiplus::SolidBrush sheen{Gdiplus::Color{24, 151, 235, 255}};
    graphics.FillPath(&sheen, &path);
    Gdiplus::Pen edge{Gdiplus::Color{245, 119, 232, 255}, 1.5F};
    graphics.DrawPath(&edge, &path);
}

void draw_horizontal_rule(
    Gdiplus::Graphics& graphics,
    const Gdiplus::RectF& rectangle
)
{
    Gdiplus::LinearGradientBrush rule{
        rectangle,
        Gdiplus::Color{225, 36, 218, 255},
        Gdiplus::Color{0, 36, 218, 255},
        Gdiplus::LinearGradientModeHorizontal,
    };
    graphics.FillRectangle(&rule, rectangle);
}

void paint_launcher(HWND window, LauncherApp& app)
{
    PAINTSTRUCT paint{};
    const auto device = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    HDC memory = CreateCompatibleDC(device);
    HBITMAP bitmap = CreateCompatibleBitmap(device, width, height);
    const auto old_bitmap =
        static_cast<HBITMAP>(SelectObject(memory, bitmap));

    Gdiplus::Graphics graphics{memory};
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(
        Gdiplus::InterpolationModeHighQualityBicubic
    );
    graphics.SetTextRenderingHint(
        Gdiplus::TextRenderingHintAntiAliasGridFit
    );
    const auto& assets = app.assets();
    const auto& snapshot = app.snapshot();
    const auto ready = palverify::launcher_can_start(
        snapshot.status,
        snapshot.payload_installed,
        snapshot.preflight_succeeded
    );
    const auto start_hovered =
        app.hovered_button() == InteractiveButton::Start;
    const auto start_pressed =
        app.pressed_button() == InteractiveButton::Start;
    const auto news_hovered =
        app.hovered_button() == InteractiveButton::News;
    const auto news_pressed =
        app.pressed_button() == InteractiveButton::News;
    const auto start_button = button_render_rect(
        scaled_rect(client, 604, 744, 442, 108),
        start_pressed
    );
    const auto news_button = button_render_rect(
        scaled_rect(client, 1425, 858, 201, 62),
        news_pressed
    );

    draw_image(
        graphics,
        assets.background.get(),
        Gdiplus::RectF{
            0,
            0,
            static_cast<float>(width),
            static_cast<float>(height),
        }
    );
    Gdiplus::SolidBrush shade{Gdiplus::Color{25, 0, 12, 26}};
    graphics.FillRectangle(
        &shade,
        Gdiplus::RectF{
            0,
            0,
            static_cast<float>(width),
            static_cast<float>(height),
        }
    );

    draw_image(
        graphics,
        assets.logo.get(),
        scaled_rect(client, 456, 8, 760, 430)
    );
    draw_glass_panel(
        graphics,
        scaled_rect(client, 45, 478, 416, 337),
        8.0F,
        false
    );
    draw_glass_panel(
        graphics,
        scaled_rect(client, 1175, 478, 418, 337),
        8.0F,
        false
    );
    if (ready) {
        draw_image(graphics, assets.button_start_ready.get(), start_button);
    } else {
        draw_action_button(graphics, start_button, 8.0F);
    }
    draw_button_interaction(
        graphics,
        start_button,
        8.0F,
        start_hovered,
        start_pressed
    );
    draw_image(graphics, assets.button_news_reference.get(), news_button);
    draw_button_interaction(
        graphics,
        news_button,
        7.0F,
        news_hovered,
        news_pressed
    );
    draw_horizontal_rule(
        graphics,
        scaled_rect(client, 71, 541, 360, 2)
    );
    draw_horizontal_rule(
        graphics,
        scaled_rect(client, 1205, 541, 350, 2)
    );
    draw_image(
        graphics,
        assets.small_logo.get(),
        scaled_rect(client, 42, 847, 160, 64)
    );

    draw_image(
        graphics,
        assets.icon_globe.get(),
        scaled_rect(client, 25, 21, 30, 30)
    );
    draw_image(
        graphics,
        assets.icon_minimize.get(),
        scaled_rect(client, 1545, 26, 28, 28)
    );
    draw_image(
        graphics,
        assets.icon_close.get(),
        scaled_rect(client, 1597, 20, 32, 32)
    );
    draw_image(
        graphics,
        assets.icon_refresh.get(),
        scaled_rect(client, 1519, 494, 30, 30)
    );
    const auto sx = static_cast<float>(width) / design_width;
    const auto white = Gdiplus::Color{255, 241, 248, 255};
    const auto muted = Gdiplus::Color{255, 185, 204, 220};
    const auto cyan = Gdiplus::Color{255, 68, 222, 255};
    const auto green = Gdiplus::Color{255, 36, 238, 116};
    const auto warning = Gdiplus::Color{255, 255, 191, 62};
    const auto error = Gdiplus::Color{255, 255, 82, 82};
    const auto status_icon = status_icon_for(snapshot);
    const auto status_color = status_icon == StatusIcon::Success
        ? green
        : (status_icon == StatusIcon::Warning ? warning : error);

    draw_text(
        graphics,
        L"TRẠNG THÁI",
        scaled_rect(client, 70, 489, 330, 50),
        heading_font_size * sx,
        white,
        Gdiplus::FontStyleBold
    );
    draw_text(
        graphics,
        L"KIỂM TRA PHIÊN BẢN",
        scaled_rect(client, 1187, 489, 280, 50),
        heading_font_size * sx,
        white,
        Gdiplus::FontStyleBold
    );

    draw_status_icon(
        graphics,
        scaled_rect(client, 70, 568, 62, 62),
        status_icon
    );
    draw_text(
        graphics,
        snapshot.headline,
        scaled_rect(client, 151, 566, 284, 44),
        status_font_size * sx,
        status_color,
        Gdiplus::FontStyleBold
    );
    draw_text(
        graphics,
        snapshot.detail,
        scaled_rect(client, 151, 607, 284, 56),
        supporting_font_size * sx,
        muted
    );
    draw_text(
        graphics,
        L"PHIÊN BẢN HIỆN TẠI",
        scaled_rect(client, 70, 684, 330, 34),
        status_font_size * sx,
        white,
        Gdiplus::FontStyleBold
    );
    draw_text(
        graphics,
        L"Launcher:  v" + utf8_to_wide(launcher_display_version),
        scaled_rect(client, 70, 726, 330, 28),
        body_font_size * sx,
        white
    );
    draw_text(
        graphics,
        L"PalVerify:  v" + utf8_to_wide(palverify_version),
        scaled_rect(client, 70, 759, 330, 28),
        body_font_size * sx,
        green
    );

    std::wstring local_version = L"Đang kiểm tra";
    std::wstring server_version = L"Đang kiểm tra";
    if (snapshot.manifest.has_value()) {
        server_version =
            utf8_to_wide(snapshot.manifest->palworld_version);
        if (snapshot.steam.build_id
            == snapshot.manifest->required_palworld_build_id) {
            local_version = server_version;
        } else if (!snapshot.steam.build_id.empty()) {
            local_version =
                L"Build " + utf8_to_wide(snapshot.steam.build_id);
        } else {
            local_version = L"Không tìm thấy";
        }
    }
    draw_text(
        graphics,
        L"Phiên bản của bạn",
        scaled_rect(client, 1187, 560, 185, 36),
        body_font_size * sx,
        white
    );
    draw_text(
        graphics,
        local_version,
        scaled_rect(client, 1370, 560, 175, 36),
        body_font_size * sx,
        ready ? green : warning,
        Gdiplus::FontStyleBold,
        Gdiplus::StringAlignmentFar
    );
    draw_text(
        graphics,
        L"Phiên bản trên server",
        scaled_rect(client, 1187, 613, 185, 36),
        body_font_size * sx,
        white
    );
    draw_text(
        graphics,
        server_version,
        scaled_rect(client, 1370, 613, 175, 36),
        body_font_size * sx,
        ready ? green : warning,
        Gdiplus::FontStyleBold,
        Gdiplus::StringAlignmentFar
    );
    draw_text(
        graphics,
        ready ? L"Phiên bản trùng khớp" : L"Chưa sẵn sàng",
        scaled_rect(client, 1235, 690, 320, 40),
        status_font_size * sx,
        ready ? green : warning,
        Gdiplus::FontStyleBold
    );
    draw_text(
        graphics,
        ready
            ? L"Bạn đang dùng phiên bản mới nhất."
            : L"Nút chơi sẽ mở khi kiểm tra hoàn tất.",
        scaled_rect(client, 1235, 733, 320, 54),
        supporting_font_size * sx,
        muted
    );

    std::wstring button = L"ĐANG KIỂM TRA";
    if (ready) {
        button = L"BẮT ĐẦU";
    } else if (snapshot.status == palverify::LauncherStatus::Ready) {
        button = snapshot.waiting_for_game_exit
            ? L"HÃY THOÁT PALWORLD"
            : snapshot.payload_installed
            ? L"PALVERIFY KHÔNG HỢP LỆ"
            : L"CÀI PALVERIFY THẤT BẠI";
    } else if (
        snapshot.status == palverify::LauncherStatus::GameUpdateRequired) {
        button = L"MỞ STEAM CẬP NHẬT";
    } else if (
        snapshot.status
        == palverify::LauncherStatus::LauncherUpdateRequired) {
        button = snapshot.updating ? L"ĐANG CẬP NHẬT" : L"CẬP NHẬT BẮT BUỘC";
    }
    if (!ready) {
        const auto pressed_offset = start_pressed ? 2.0F : 0.0F;
        draw_text(
            graphics,
            button,
            scaled_rect(client, 620, 742 + pressed_offset, 382, 55),
            button.size() > 12 ? 26 * sx : 34 * sx,
            white,
            Gdiplus::FontStyleBold,
            Gdiplus::StringAlignmentCenter
        );
        draw_text(
            graphics,
            snapshot.status == palverify::LauncherStatus::Ready
                ? failure_support_hint
                : L"Launcher đang bảo vệ phiên chơi của bạn.",
            scaled_rect(client, 620, 796 + pressed_offset, 382, 29),
            supporting_font_size * sx,
            muted,
            Gdiplus::FontStyleBold,
            Gdiplus::StringAlignmentCenter
        );
    }

    const auto track = scaled_rect(client, 517, 905, 610, 11);
    Gdiplus::SolidBrush track_brush{Gdiplus::Color{190, 3, 24, 38}};
    graphics.FillRectangle(&track_brush, track);
    const auto progress_width =
        track.Width * static_cast<float>(snapshot.progress) / 100.0F;
    if (progress_width > 0) {
        const auto clip_state = graphics.Save();
        graphics.SetClip(
            Gdiplus::RectF{track.X, track.Y, progress_width, track.Height}
        );
        draw_image(graphics, assets.progress.get(), track);
        graphics.Restore(clip_state);
    }
    draw_text(
        graphics,
        ready ? L"Trò chơi đã được cập nhật đầy đủ."
              : snapshot.headline,
        scaled_rect(client, 517, 865, 520, 35),
        supporting_font_size * sx,
        white
    );
    draw_text(
        graphics,
        std::to_wstring(snapshot.progress) + L"%",
        scaled_rect(client, 1135, 892, 80, 38),
        body_font_size * sx,
        white,
        Gdiplus::FontStyleBold
    );

    BitBlt(device, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

[[nodiscard]] auto app_from_window(HWND window) -> LauncherApp*
{
    return reinterpret_cast<LauncherApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)
    );
}

auto CALLBACK window_proc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) -> LRESULT
{
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams)
        );
        app_from_window(window)->attach(window);
    }
    auto* app = app_from_window(window);
    switch (message) {
    case WM_CREATE:
        app->refresh();
        SetTimer(window, timer_client_watchdog, 5000, nullptr);
        return 0;
    case WM_PAINT:
        paint_launcher(window, *app);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wparam == timer_recheck) {
            app->refresh();
        } else if (wparam == timer_client_watchdog) {
            app->supervise_client();
        }
        return 0;
    case message_snapshot:
        app->apply_snapshot(
            std::unique_ptr<Snapshot>{
                reinterpret_cast<Snapshot*>(lparam),
            }
        );
        return 0;
    case message_progress:
        app->set_progress(static_cast<int>(wparam));
        return 0;
    case message_update_failed:
        app->update_failed();
        return 0;
    case WM_MOUSEMOVE: {
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam),
        };
        RECT client{};
        GetClientRect(window, &client);
        auto hovered = InteractiveButton::None;
        if (hit(client, point, 604, 744, 442, 108)) {
            hovered = InteractiveButton::Start;
        } else if (hit(client, point, 1425, 858, 201, 62)) {
            hovered = InteractiveButton::News;
        }
        app->set_hovered_button(hovered);
        TRACKMOUSEEVENT tracking{
            .cbSize = sizeof(TRACKMOUSEEVENT),
            .dwFlags = TME_LEAVE,
            .hwndTrack = window,
            .dwHoverTime = 0,
        };
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        app->set_hovered_button(InteractiveButton::None);
        return 0;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        RECT client{};
        GetClientRect(window, &client);
        if (hit(client, point, 18, 10, 48, 55)
            || hit(client, point, 604, 744, 442, 108)
            || hit(client, point, 1425, 858, 201, 62)
            || hit(client, point, 1500, 15, 140, 50)
            || hit(client, point, 1500, 470, 60, 70)) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam),
        };
        RECT client{};
        GetClientRect(window, &client);
        if (hit(client, point, 604, 744, 442, 108)) {
            SetCapture(window);
            app->set_pressed_button(InteractiveButton::Start);
        } else if (hit(client, point, 1425, 858, 201, 62)) {
            SetCapture(window);
            app->set_pressed_button(InteractiveButton::News);
        } else if (hit(client, point, 1590, 10, 55, 55)) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (hit(client, point, 1535, 10, 55, 55)) {
            ShowWindow(window, SW_MINIMIZE);
        } else if (hit(client, point, 18, 10, 48, 55)) {
            app->open_website();
        } else if (hit(client, point, 1495, 470, 70, 70)) {
            app->refresh();
        } else if (point.y < static_cast<LONG>(80.0F * client.bottom
                                              / design_height)) {
            ReleaseCapture();
            SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const auto pressed = app->pressed_button();
        if (pressed == InteractiveButton::None) {
            break;
        }
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam),
        };
        RECT client{};
        GetClientRect(window, &client);
        ReleaseCapture();
        app->set_pressed_button(InteractiveButton::None);
        if (pressed == InteractiveButton::News
            && hit(client, point, 1425, 858, 201, 62)) {
            app->open_news();
        } else if (pressed == InteractiveButton::Start
                   && hit(client, point, 604, 744, 442, 108)) {
            if (app->has_error_details()) {
                app->show_error_details();
            } else if (app->snapshot().status
                    == palverify::LauncherStatus::LauncherUpdateRequired
                && !app->snapshot().updating) {
                app->begin_mandatory_update();
            } else {
                app->start_game();
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        app->set_pressed_button(InteractiveButton::None);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F5) {
            app->refresh();
            return 0;
        }
        if (wparam == VK_RETURN) {
            if (app->has_error_details()) {
                app->show_error_details();
            } else {
                app->start_game();
            }
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(window, timer_recheck);
        KillTimer(window, timer_client_watchdog);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] auto run_install_only(const Arguments& arguments) -> int
{
    const auto game_root = arguments.game_root.has_value()
        ? arguments.game_root
        : discover_palworld_install();
    if (!game_root.has_value()) {
        if (!arguments.silent) {
            MessageBoxW(
                nullptr,
                L"Không tìm thấy Palworld Steam trên máy này.",
                window_title,
                MB_OK | MB_ICONERROR
            );
        }
        return 2;
    }
    const auto result = install_embedded_payload(*game_root);
    if (!result.success) {
        if (!arguments.silent) {
            MessageBoxW(
                nullptr,
                L"Không thể cài PalVerify vào thư mục game.",
                window_title,
                MB_OK | MB_ICONERROR
            );
        }
        return 3;
    }
    return 0;
}

[[nodiscard]] auto run_manifest_check(const Arguments& arguments) -> int
{
    const auto response = http_get(arguments.manifest_url);
    if (!response.has_value()) {
        return 10;
    }
    if (response->status != 200) {
        return 11;
    }
    auto body = response->body;
    const auto release_asset = palverify::github_release_asset_url(
        body,
        release_manifest_asset_name
    );
    if (release_asset.has_value()) {
        const auto asset_response = http_get(*release_asset);
        if (!asset_response.has_value()) {
            return 12;
        }
        if (asset_response->status != 200) {
            return 13;
        }
        body = asset_response->body;
    }
    return palverify::parse_launcher_manifest(body).has_value() ? 0 : 14;
}

}  // namespace

auto WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show
) -> int
{
    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    );
    const auto arguments = parse_arguments();
    if (arguments.check_manifest) {
        return run_manifest_check(arguments);
    }
    if (arguments.install_only) {
        return run_install_only(arguments);
    }

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(
            &gdiplus_token,
            &gdiplus_input,
            nullptr
        )
        != Gdiplus::Ok) {
        return 5;
    }
    auto app = std::make_unique<LauncherApp>(instance, arguments);
    WNDCLASSEXW window_class_definition{
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = window_proc,
        .hInstance = instance,
        .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PAL3MIEN)),
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground =
            reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)),
        .lpszClassName = window_class,
        .hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_PAL3MIEN)),
    };
    if (RegisterClassExW(&window_class_definition) == 0) {
        app.reset();
        Gdiplus::GdiplusShutdown(gdiplus_token);
        return 6;
    }

    const auto desktop_width = GetSystemMetrics(SM_CXSCREEN);
    const auto desktop_height = GetSystemMetrics(SM_CYSCREEN);
    int width = (std::min)(1338, desktop_width - 80);
    int height = static_cast<int>(
        static_cast<double>(width) * design_height / design_width
    );
    if (height > desktop_height - 80) {
        height = desktop_height - 80;
        width = static_cast<int>(
            static_cast<double>(height) * design_width / design_height
        );
    }
    const auto window = CreateWindowExW(
        WS_EX_APPWINDOW,
        window_class,
        window_title,
        WS_POPUP | WS_MINIMIZEBOX,
        (desktop_width - width) / 2,
        (desktop_height - height) / 2,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        app.get()
    );
    if (window == nullptr) {
        app.reset();
        Gdiplus::GdiplusShutdown(gdiplus_token);
        return 7;
    }
    constexpr DWORD corner_preference = 2;
    DwmSetWindowAttribute(
        window,
        33,
        &corner_preference,
        sizeof(corner_preference)
    );
    ShowWindow(window, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app.reset();
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return static_cast<int>(message.wParam);
}
