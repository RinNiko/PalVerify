#include "palverify/client_report.hpp"
#include "palverify/process_rules.hpp"
#include "palverify/windows_process_scan.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <winhttp.h>

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] auto palworld_is_running() -> bool
{
    const auto snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return true;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(
                    entry.szExeFile,
                    L"Palworld-Win64-Shipping.exe"
                )
                == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return running;
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

[[nodiscard]] auto find_game_root()
    -> std::optional<std::filesystem::path>
{
    auto candidate = module_directory();
    for (int depth = 0; depth < 12 && !candidate.empty(); ++depth) {
        const auto executable =
            candidate / "Pal" / "Binaries" / "Win64"
            / "Palworld-Win64-Shipping.exe";
        if (std::filesystem::is_regular_file(executable)) {
            return candidate;
        }
        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return std::nullopt;
}

[[nodiscard]] auto read_client_config()
    -> std::optional<palverify::ClientConfig>
{
    std::ifstream input{
        module_directory() / "config.json",
        std::ios::binary,
    };
    if (!input) {
        return std::nullopt;
    }
    const std::string json{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    return palverify::parse_client_config(json);
}

[[nodiscard]] auto active_steam_user_id() -> std::optional<std::string>
{
    DWORD account_id = 0;
    DWORD size = sizeof(account_id);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Valve\\Steam\\ActiveProcess",
            L"ActiveUser",
            RRF_RT_REG_DWORD,
            nullptr,
            &account_id,
            &size
        )
        != ERROR_SUCCESS
        || account_id == 0) {
        return std::nullopt;
    }
    return palverify::steam_user_id_from_account_id(account_id);
}

[[nodiscard]] auto utc_timestamp() -> std::string
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-' << std::setw(2)
           << time.wDay << 'T' << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':' << std::setw(2)
           << time.wSecond << 'Z';
    return output.str();
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
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            size
        )
        != size) {
        return std::nullopt;
    }
    return output;
}

struct HttpResponse {
    DWORD status;
    std::string body;
};

[[nodiscard]] auto post_json(
    std::string_view endpoint_value,
    std::string_view json
) -> std::optional<HttpResponse>
{
    const auto endpoint = utf8_to_wide(endpoint_value);
    if (!endpoint.has_value()) {
        return std::nullopt;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(
            endpoint->c_str(),
            static_cast<DWORD>(endpoint->size()),
            0,
            &parts
        )
        == FALSE) {
        return std::nullopt;
    }

    const std::wstring host{parts.lpszHostName, parts.dwHostNameLength};
    std::wstring path{parts.lpszUrlPath, parts.dwUrlPathLength};
    if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength != 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    const auto session = WinHttpOpen(
        L"PalVerify/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (session == nullptr) {
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    const auto connection =
        WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    const auto request = WinHttpOpenRequest(
        connection,
        L"POST",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
    );
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    constexpr wchar_t headers[] =
        L"Content-Type: application/json; charset=utf-8\r\n";
    const auto sent = WinHttpSendRequest(
        request,
        headers,
        static_cast<DWORD>(std::size(headers) - 1),
        const_cast<char*>(json.data()),
        static_cast<DWORD>(json.size()),
        static_cast<DWORD>(json.size()),
        0
    );
    const auto received =
        sent != FALSE ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const auto queried =
        received != FALSE
        && WinHttpQueryHeaders(
               request,
               WINHTTP_QUERY_STATUS_CODE
                   | WINHTTP_QUERY_FLAG_NUMBER,
               WINHTTP_HEADER_NAME_BY_INDEX,
               &status,
               &status_size,
               WINHTTP_NO_HEADER_INDEX
           )
            != FALSE;

    if (!queried) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > 16 * 1024) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(
                request,
                body.data() + offset,
                available,
                &read
            )
            == FALSE) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        body.resize(offset + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return HttpResponse{.status = status, .body = std::move(body)};
}

[[nodiscard]] auto log_path() -> std::filesystem::path
{
    PWSTR local_app_data = nullptr;
    if (SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT,
            nullptr,
            &local_app_data
        )
        != S_OK) {
        return "PalVerifyClient.log";
    }

    const std::filesystem::path root{local_app_data};
    CoTaskMemFree(local_app_data);
    return root / "PalVerify" / "PalVerifyClient.log";
}

void write_event(std::ofstream& log, std::string_view event)
{
    SYSTEMTIME time{};
    GetLocalTime(&time);
    log << time.wYear << '-' << time.wMonth << '-' << time.wDay << 'T'
        << time.wHour << ':' << time.wMinute << ':' << time.wSecond << ' '
        << event << '\n';
    log.flush();
}

}  // namespace

auto wmain() -> int
{
    const auto instance =
        CreateMutexW(nullptr, FALSE, L"Local\\PalVerifyClient");
    if (instance == nullptr) {
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance);
        return 0;
    }

    const auto path = log_path();
    std::error_code directory_error;
    std::filesystem::create_directories(
        path.parent_path(),
        directory_error
    );
    std::ofstream log{path, std::ios::app};
    if (!log) {
        CloseHandle(instance);
        return 3;
    }

    const auto config = read_client_config();
    const auto game_root = find_game_root();
    const auto user_id = active_steam_user_id();
    if (!config.has_value()) {
        write_event(log, "CLIENT_START_FAILED reason=invalid-config");
        CloseHandle(instance);
        return 4;
    }
    if (!game_root.has_value()) {
        write_event(log, "CLIENT_START_FAILED reason=game-root-unavailable");
        CloseHandle(instance);
        return 5;
    }
    if (!user_id.has_value()) {
        write_event(log, "CLIENT_START_FAILED reason=steam-user-unavailable");
        CloseHandle(instance);
        return 6;
    }

    if (!palworld_is_running()) {
        write_event(log, "CLIENT_WAITING_FOR_GAME");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::minutes{2};
        while (!palworld_is_running()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
        if (!palworld_is_running()) {
            write_event(log, "CLIENT_START_FAILED reason=game-start-timeout");
            CloseHandle(instance);
            return 7;
        }
    }

    write_event(log, "CLIENT_STARTED protocol=3 version=1.0");
    auto inventory = palverify::scan_mod_inventory(*game_root);
    {
        std::string ids;
        for (const auto& mod : inventory) {
            if (!ids.empty()) {
                ids.push_back(',');
            }
            ids += mod.id;
        }
        write_event(log, "MOD_INVENTORY ids=" + ids);
    }

    std::uint64_t sequence = 0;
    auto next_inventory_refresh = std::chrono::steady_clock::now()
        + std::chrono::minutes{1};
    while (palworld_is_running()) {
        const auto scan = palverify::scan_running_processes();
        std::vector<std::string> violations;
        if (!scan.available) {
            write_event(log, "PROCESS_SCAN_UNAVAILABLE");
        } else {
            for (const auto rule : scan.rules) {
                violations.emplace_back(palverify::to_string(rule));
            }
        }
        const auto module_scan =
            palverify::scan_palworld_modules(*game_root);
        if (!module_scan.available) {
            write_event(log, "MODULE_SCAN_UNAVAILABLE");
        } else {
            for (const auto rule : module_scan.rules) {
                violations.emplace_back(palverify::to_string(rule));
            }
        }
        if (!violations.empty()) {
            write_event(log, "INTEGRITY_VIOLATION");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_inventory_refresh) {
            inventory = palverify::scan_mod_inventory(*game_root);
            next_inventory_refresh = now + std::chrono::minutes{1};
        }
        const auto wall_clock_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
        sequence = palverify::next_report_sequence(
            sequence,
            static_cast<std::uint64_t>(wall_clock_milliseconds)
        );
        const auto challenge_response = post_json(
            config->coordinator + "/v1/client/challenge",
            palverify::build_challenge_request_json(
                config->server_id,
                *user_id
            )
        );
        if (!challenge_response.has_value()) {
            write_event(log, "CHALLENGE_FAILED reason=transport");
            std::this_thread::sleep_for(std::chrono::seconds{5});
            continue;
        }
        if (challenge_response->status < 200
            || challenge_response->status >= 300) {
            write_event(
                log,
                "CHALLENGE_WAITING status="
                    + std::to_string(challenge_response->status)
            );
            std::this_thread::sleep_for(std::chrono::seconds{5});
            continue;
        }
        const auto challenge =
            palverify::parse_challenge_json(challenge_response->body);
        if (!challenge.has_value()) {
            write_event(log, "CHALLENGE_FAILED reason=invalid-response");
            std::this_thread::sleep_for(std::chrono::seconds{5});
            continue;
        }
        const auto report = palverify::build_client_report_json({
            .server_id = config->server_id,
            .user_id = *user_id,
            .protocol_version = "3",
            .challenge = *challenge,
            .sequence = sequence,
            .sent_at = utc_timestamp(),
            .mods = inventory,
            .violations = std::move(violations),
        });
        const auto response = post_json(
            config->coordinator + "/v1/client/report",
            report
        );
        if (!response.has_value()) {
            write_event(log, "REPORT_FAILED reason=transport");
        } else if (response->status < 200 || response->status >= 300) {
            write_event(
                log,
                "REPORT_FAILED status="
                    + std::to_string(response->status)
            );
        } else {
            write_event(
                log,
                "REPORT_ACCEPTED sequence=" + std::to_string(sequence)
            );
        }
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }
    write_event(log, "CLIENT_STOPPED reason=game-exited");
    CloseHandle(instance);
    return 0;
}
