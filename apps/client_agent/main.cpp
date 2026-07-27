#include "palverify/client_report.hpp"
#include "palverify/process_rules.hpp"
#include "palverify/windows_process_scan.hpp"
#include "client_ui_windows.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <winhttp.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

thread_local DWORD last_post_error = ERROR_SUCCESS;
thread_local std::string_view last_post_stage = "none";

[[nodiscard]] auto post_json(
    std::string_view endpoint_value,
    std::string_view json
) -> std::optional<HttpResponse>
{
    last_post_error = ERROR_SUCCESS;
    last_post_stage = "start";
    const auto endpoint = utf8_to_wide(endpoint_value);
    if (!endpoint.has_value()) {
        last_post_error = ERROR_INVALID_PARAMETER;
        last_post_stage = "url-encoding";
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
        last_post_error = GetLastError();
        last_post_stage = "url-parse";
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
        last_post_error = GetLastError();
        last_post_stage = "session";
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 8000, 8000, 12000, 12000);
    const auto connection =
        WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (connection == nullptr) {
        last_post_error = GetLastError();
        last_post_stage = "connect";
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
        last_post_error = GetLastError();
        last_post_stage = "request";
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
    if (sent == FALSE) {
        last_post_error = GetLastError();
        last_post_stage = "send";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    const auto received = WinHttpReceiveResponse(request, nullptr);
    if (received == FALSE) {
        last_post_error = GetLastError();
        last_post_stage = "receive";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const auto queried = WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &status_size,
        WINHTTP_NO_HEADER_INDEX
    );

    if (!queried) {
        last_post_error = GetLastError();
        last_post_stage = "status";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            last_post_error = GetLastError();
            last_post_stage = "body-size";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > 16 * 1024) {
            last_post_error = ERROR_INSUFFICIENT_BUFFER;
            last_post_stage = "body-limit";
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
            last_post_error = GetLastError();
            last_post_stage = "body-read";
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

[[nodiscard]] auto post_json_with_retry(
    std::string_view endpoint,
    std::string_view json
) -> std::optional<HttpResponse>
{
    constexpr unsigned int maximum_attempts = 2;
    for (unsigned int attempt = 1; attempt <= maximum_attempts; ++attempt) {
        auto response = post_json(endpoint, json);
        const auto status = response.has_value()
            ? std::optional<unsigned long>{response->status}
            : std::nullopt;
        if (!palverify::should_retry_client_http(
                status,
                last_post_error,
                attempt,
                maximum_attempts
            )) {
            return response;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{400 * attempt}
        );
    }
    return std::nullopt;
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

[[nodiscard]] auto take_next_ui_command(
    const std::filesystem::path& queue_directory
) -> std::optional<palverify::ClientUiCommand>
{
    std::error_code error;
    std::filesystem::create_directories(queue_directory, error);
    error.clear();

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator{
             queue_directory,
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
        if (iterator->is_regular_file(error)
            && !error
            && iterator->path().extension() == ".cmd") {
            candidates.push_back(iterator->path());
        }
        error.clear();
    }
    std::ranges::sort(candidates);
    for (const auto& candidate : candidates) {
        auto processing = candidate;
        processing.replace_extension(".processing");
        std::filesystem::rename(candidate, processing, error);
        if (error) {
            error.clear();
            continue;
        }
        const auto size = std::filesystem::file_size(processing, error);
        if (error || size > 128) {
            error.clear();
            std::filesystem::remove(processing, error);
            error.clear();
            continue;
        }
        std::ifstream input{processing, std::ios::binary};
        const std::string value{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{},
        };
        input.close();
        std::filesystem::remove(processing, error);
        error.clear();
        if (const auto command =
                palverify::parse_client_ui_command(value);
            command.has_value()) {
            return command;
        }
    }
    return std::nullopt;
}

void start_client_ui_worker(
    std::filesystem::path queue_directory,
    std::string website
)
{
    std::thread(
        [
            queue_directory = std::move(queue_directory),
            website = std::move(website)
        ] {
            while (palworld_is_running()) {
                const auto command =
                    take_next_ui_command(queue_directory);
                if (!command.has_value()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{250}
                    );
                    continue;
                }
                if (command->kind
                    == palverify::ClientUiCommandKind::verify) {
                    if (palverify::client_ui::confirm_verification(
                            command->value
                        )) {
                        static_cast<void>(
                            palverify::client_ui::open_default_browser(
                                palverify::build_client_ui_url(
                                    website,
                                    *command
                                )
                            )
                        );
                    }
                    continue;
                }
                const auto giftcode =
                    palverify::client_ui::prompt_giftcode(command->value);
                if (!giftcode.has_value()) {
                    continue;
                }
                const palverify::ClientUiCommand submitted{
                    .kind =
                        palverify::ClientUiCommandKind::giftcode,
                    .value = *giftcode,
                };
                static_cast<void>(
                    palverify::client_ui::open_default_browser(
                        palverify::build_client_ui_url(
                            website,
                            submitted
                        )
                    )
                );
            }
        }
    ).detach();
}

[[nodiscard]] auto join_codes(
    const std::vector<std::string>& values
) -> std::string
{
    std::string joined;
    for (const auto& value : values) {
        if (!joined.empty()) {
            joined.push_back(',');
        }
        joined += value;
    }
    return joined;
}

void append_module_evidence(
    const palverify::ModuleScanResult& scan,
    std::vector<std::string>& violations,
    std::vector<palverify::IntegrityEvidence>& evidence
)
{
    for (const auto rule : scan.rules) {
        violations.emplace_back(palverify::to_string(rule));
    }
    for (const auto& match : scan.matches) {
        evidence.push_back({
            .rule = std::string{palverify::to_string(match.rule)},
            .source = match.image_name.empty() ? "memory" : "module",
            .file_name = match.image_name,
            .sha256 = match.sha256,
            .signer_name = match.signer_name,
            .file_description = match.file_description,
            .company_name = match.company_name,
            .match_reason = match.match_reason,
            .signature_valid = match.signature_valid,
        });
    }
}

void show_runtime_integrity_alert(
    const std::vector<std::string>& violations,
    const std::vector<palverify::IntegrityEvidence>& evidence
)
{
    const auto message = utf8_to_wide(
        palverify::format_runtime_integrity_message(violations, evidence)
    );
    if (!message.has_value()) {
        return;
    }
    std::thread([message = *message] {
        MessageBoxW(
            nullptr,
            message.c_str(),
            L"PalVerify - Không thể xác minh",
            MB_OK | MB_ICONERROR | MB_TOPMOST
        );
    }).detach();
}

[[nodiscard]] auto finish_preflight(
    std::ofstream& log,
    palverify::ClientPreflightExit exit_code,
    std::string_view reason,
    std::string_view detail = {}
) -> int
{
    std::string event =
        exit_code == palverify::ClientPreflightExit::accepted
        ? "PREFLIGHT_ACCEPTED"
        : "PREFLIGHT_REJECTED";
    event += " reason=";
    event += reason;
    if (!detail.empty()) {
        event += " detail=";
        event += detail;
    }
    write_event(log, event);
    std::cout << event << '\n';
    return static_cast<int>(exit_code);
}

[[nodiscard]] auto run_preflight(
    std::ofstream& log,
    const palverify::ClientConfig& config,
    const std::filesystem::path& game_root
) -> int
{
    if (!active_steam_user_id().has_value()) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::steam_user_unavailable,
            "STEAM_USER_UNAVAILABLE"
        );
    }

    const auto process_scan = palverify::scan_running_processes();
    if (!process_scan.available) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::scan_unavailable,
            "PROCESS_SCAN_UNAVAILABLE"
        );
    }
    std::vector<std::string> violations;
    std::vector<palverify::IntegrityEvidence> violation_evidence;
    for (const auto rule : process_scan.rules) {
        violations.emplace_back(palverify::to_string(rule));
    }

    if (palworld_is_running()) {
        const auto module_scan = palverify::scan_palworld_modules(game_root);
        if (!module_scan.available) {
            return finish_preflight(
                log,
                palverify::ClientPreflightExit::scan_unavailable,
                "MODULE_SCAN_UNAVAILABLE"
            );
        }
        append_module_evidence(
            module_scan,
            violations,
            violation_evidence
        );
    }

    std::vector<palverify::ReportedMod> inventory;
    try {
        inventory = palverify::scan_mod_inventory(game_root);
    } catch (const std::exception&) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::scan_unavailable,
            "MOD_SCAN_UNAVAILABLE"
        );
    }
    const auto response = post_json_with_retry(
        config.coordinator + "/v1/client/preflight",
        palverify::build_client_preflight_json({
            .server_id = config.server_id,
            .protocol_version = "3",
            .mods = std::move(inventory),
            .violations = std::move(violations),
            .violation_evidence = std::move(violation_evidence),
        })
    );
    if (!response.has_value()) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::transport_failed,
            "COORDINATOR_UNREACHABLE",
            std::string{last_post_stage}
                + "-WINHTTP_" + std::to_string(last_post_error)
        );
    }
    if (response->status != 200) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::http_rejected,
            "COORDINATOR_HTTP_ERROR",
            "HTTP_" + std::to_string(response->status)
        );
    }
    const auto result =
        palverify::parse_client_preflight_response(response->body);
    if (!result.has_value()) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::invalid_response,
            "COORDINATOR_RESPONSE_INVALID"
        );
    }
    if (result->accepted) {
        return finish_preflight(
            log,
            palverify::ClientPreflightExit::accepted,
            result->reason
        );
    }
    auto exit_code = palverify::ClientPreflightExit::rejected;
    if (result->reason == "INTEGRITY_VIOLATION") {
        exit_code = palverify::ClientPreflightExit::integrity_violation;
    } else if (result->reason == "UNAPPROVED_MOD") {
        exit_code = palverify::ClientPreflightExit::unapproved_mod;
    }
    return finish_preflight(
        log,
        exit_code,
        result->reason,
        result->detail
    );
}

}  // namespace

auto wmain(int argc, wchar_t** argv) -> int
{
    const auto preflight =
        argc == 2 && _wcsicmp(argv[1], L"--preflight") == 0;
    if (argc != 1 && !preflight) {
        return 10;
    }

    HANDLE instance = nullptr;
    if (!preflight) {
        instance = CreateMutexW(nullptr, FALSE, L"Local\\PalVerifyClient");
        if (instance == nullptr) {
            return 2;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(instance);
            return 0;
        }
    }
    const auto path = log_path();
    std::error_code directory_error;
    std::filesystem::create_directories(
        path.parent_path(),
        directory_error
    );
    std::ofstream log{path, std::ios::app};
    if (!log) {
        if (instance != nullptr) {
            CloseHandle(instance);
        }
        return 3;
    }

    const auto config = read_client_config();
    const auto game_root = find_game_root();
    if (!config.has_value()) {
        write_event(log, "CLIENT_START_FAILED reason=invalid-config");
        if (instance != nullptr) {
            CloseHandle(instance);
        }
        if (preflight) {
            return finish_preflight(
                log,
                palverify::ClientPreflightExit::invalid_config,
                "INVALID_CONFIG"
            );
        }
        return 4;
    }
    if (!game_root.has_value()) {
        write_event(log, "CLIENT_START_FAILED reason=game-root-unavailable");
        if (instance != nullptr) {
            CloseHandle(instance);
        }
        if (preflight) {
            return finish_preflight(
                log,
                palverify::ClientPreflightExit::game_root_unavailable,
                "GAME_ROOT_UNAVAILABLE"
            );
        }
        return 5;
    }
    if (preflight) {
        return run_preflight(log, *config, *game_root);
    }

    const auto user_id = active_steam_user_id();
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

    write_event(log, "CLIENT_STARTED protocol=3 version=1.0.13");
    start_client_ui_worker(
        module_directory() / "ui-queue",
        config->website
    );
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
    bool integrity_alert_shown = false;
    while (true) {
        if (!palworld_is_running()) {
            write_event(log, "CLIENT_STOPPED reason=game-exited");
            break;
        }

        const auto scan = palverify::scan_running_processes();
        std::vector<std::string> violations;
        std::vector<palverify::IntegrityEvidence> violation_evidence;
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
            append_module_evidence(
                module_scan,
                violations,
                violation_evidence
            );
        }
        if (!violations.empty()) {
            write_event(
                log,
                "INTEGRITY_VIOLATION rules=" + join_codes(violations)
            );
            if (!integrity_alert_shown) {
                integrity_alert_shown = true;
                show_runtime_integrity_alert(
                    violations,
                    violation_evidence
                );
            }
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
        const auto challenge_response = post_json_with_retry(
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
            .violation_evidence = std::move(violation_evidence),
        });
        const auto response = post_json_with_retry(
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
    write_event(log, "CLIENT_STOPPED reason=game-restart-timeout");
    CloseHandle(instance);
    return 0;
}
