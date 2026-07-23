#include "palverify/windows_process_scan.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace palverify {
namespace {

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

}  // namespace

auto scan_running_processes() -> ProcessScanResult
{
    const auto snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {.available = false, .rules = {}};
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<std::string> image_names;

    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            auto image_name = ascii_image_name(entry.szExeFile);
            if (!image_name.empty()) {
                image_names.push_back(std::move(image_name));
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    } else {
        CloseHandle(snapshot);
        return {.available = false, .rules = {}};
    }
    CloseHandle(snapshot);

    std::vector<std::string_view> image_views;
    image_views.reserve(image_names.size());
    for (const auto& image_name : image_names) {
        image_views.push_back(image_name);
    }

    return {
        .available = true,
        .rules = detect_process_rules(image_views),
    };
}

}  // namespace palverify
