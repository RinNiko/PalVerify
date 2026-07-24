#include "palverify/process_rules.hpp"
#include "palverify/windows_process_scan.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

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
        CreateMutexW(nullptr, FALSE, L"Local\\PalVerifyClient-v0.2");
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

    write_event(log, "CLIENT_STARTED protocol=observation-only version=0.2.0");
    while (palworld_is_running()) {
        const auto scan = palverify::scan_running_processes();
        if (!scan.available) {
            write_event(log, "PROCESS_SCAN_UNAVAILABLE");
        } else {
            for (const auto rule : scan.rules) {
                write_event(log, palverify::to_string(rule));
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }
    write_event(log, "CLIENT_STOPPED reason=game-exited");
    CloseHandle(instance);
    return 0;
}
