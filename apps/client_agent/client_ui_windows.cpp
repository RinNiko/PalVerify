#include "client_ui_windows.hpp"

#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace palverify::client_ui {
namespace {

constexpr int giftcode_edit_id = 1101;
constexpr int giftcode_submit_id = 1102;

struct GiftcodeWindowState {
    std::wstring initial_value;
    std::optional<std::string> result;
};

[[nodiscard]] auto utf8_to_wide(std::string_view value)
    -> std::optional<std::wstring>
{
    if (value.empty()) {
        return std::wstring{};
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

[[nodiscard]] auto wide_to_utf8(std::wstring_view value)
    -> std::optional<std::string>
{
    if (value.empty()) {
        return std::string{};
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
        return std::nullopt;
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            size,
            nullptr,
            nullptr
        )
        != size) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] auto valid_giftcode(std::string_view code) -> bool
{
    return code.size() >= 4 && code.size() <= 32
        && std::ranges::all_of(code, [](const char character) {
               const auto byte = static_cast<unsigned char>(character);
               return std::isupper(byte) != 0
                   || std::isdigit(byte) != 0
                   || character == '-';
           });
}

void set_default_font(HWND control)
{
    SendMessageW(
        control,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
        TRUE
    );
}

auto giftcode_window_proc(
    HWND window,
    UINT message,
    WPARAM word_parameter,
    LPARAM long_parameter
) -> LRESULT
{
    auto* state = reinterpret_cast<GiftcodeWindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)
    );
    if (message == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(long_parameter);
        state =
            static_cast<GiftcodeWindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state)
        );
    }
    if (message == WM_CREATE) {
        const auto title = CreateWindowExW(
            0,
            L"STATIC",
            L"Nhập Giftcode",
            WS_CHILD | WS_VISIBLE,
            24,
            22,
            372,
            28,
            window,
            nullptr,
            nullptr,
            nullptr
        );
        const auto guidance = CreateWindowExW(
            0,
            L"STATIC",
            L"Mã chỉ gồm chữ, số hoặc dấu gạch ngang.",
            WS_CHILD | WS_VISIBLE,
            24,
            54,
            372,
            22,
            window,
            nullptr,
            nullptr,
            nullptr
        );
        const auto edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            state != nullptr ? state->initial_value.c_str() : L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL
                | ES_UPPERCASE,
            24,
            82,
            372,
            34,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(giftcode_edit_id)
            ),
            nullptr,
            nullptr
        );
        const auto submit = CreateWindowExW(
            0,
            L"BUTTON",
            L"Xác nhận",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            270,
            132,
            126,
            36,
            window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(giftcode_submit_id)
            ),
            nullptr,
            nullptr
        );
        const auto cancel = CreateWindowExW(
            0,
            L"BUTTON",
            L"Đóng",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            164,
            132,
            94,
            36,
            window,
            reinterpret_cast<HMENU>(IDCANCEL),
            nullptr,
            nullptr
        );
        for (const auto control :
             std::array{title, guidance, edit, submit, cancel}) {
            set_default_font(control);
        }
        SendMessageW(edit, EM_SETLIMITTEXT, 32, 0);
        SetFocus(edit);
        return 0;
    }
    if (message == WM_COMMAND) {
        const auto identifier = LOWORD(word_parameter);
        if (identifier == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (identifier == giftcode_submit_id && state != nullptr) {
            std::array<wchar_t, 33> buffer{};
            GetDlgItemTextW(
                window,
                giftcode_edit_id,
                buffer.data(),
                static_cast<int>(buffer.size())
            );
            const auto code = wide_to_utf8(buffer.data());
            if (!code.has_value() || !valid_giftcode(*code)) {
                MessageBoxW(
                    window,
                    L"Giftcode phải có 4–32 ký tự: chữ, số hoặc dấu gạch ngang.",
                    L"Palworld 3 Miền",
                    MB_OK | MB_ICONWARNING
                );
                return 0;
            }
            state->result = *code;
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(
        window,
        message,
        word_parameter,
        long_parameter
    );
}

[[nodiscard]] auto register_giftcode_window_class() -> bool
{
    static const auto registered = [] {
        WNDCLASSEXW definition{};
        definition.cbSize = sizeof(definition);
        definition.lpfnWndProc = giftcode_window_proc;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.hCursor =
            LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        definition.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        definition.lpszClassName = L"PalVerifyGiftcodeWindow";
        return RegisterClassExW(&definition) != 0
            || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

auto CALLBACK task_dialog_callback(
    HWND window,
    UINT notification,
    WPARAM,
    LPARAM,
    LONG_PTR
) -> HRESULT
{
    if (notification == TDN_CREATED) {
        SetWindowPos(
            window,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
        );
        SetForegroundWindow(window);
    }
    return S_OK;
}

}  // namespace

auto confirm_verification(std::string_view code) -> bool
{
    const auto wide_code = utf8_to_wide(code);
    if (!wide_code.has_value()) {
        return false;
    }
    const std::wstring content =
        L"Website đã gửi mã xác nhận riêng cho nhân vật này.\n\nMã: "
        + *wide_code
        + L"\n\nNhấn Xác nhận để hoàn tất trên website.";
    TASKDIALOG_BUTTON button{
        .nButtonID = IDYES,
        .pszButtonText = L"Xác nhận",
    };
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags =
        TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"Palworld 3 Miền";
    dialog.pszMainInstruction = L"Xác minh nhân vật";
    dialog.pszContent = content.c_str();
    dialog.cButtons = 1;
    dialog.pButtons = &button;
    dialog.nDefaultButton = IDYES;
    dialog.pfCallback = task_dialog_callback;
    int selected = 0;
    return SUCCEEDED(TaskDialogIndirect(
               &dialog,
               &selected,
               nullptr,
               nullptr
           ))
        && selected == IDYES;
}

auto prompt_giftcode(std::string_view initial_value)
    -> std::optional<std::string>
{
    if (!register_giftcode_window_class()) {
        return std::nullopt;
    }
    GiftcodeWindowState state{};
    if (const auto value = utf8_to_wide(initial_value); value.has_value()) {
        state.initial_value = *value;
    }

    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    constexpr int width = 440;
    constexpr int height = 220;
    const auto left =
        work_area.left + (work_area.right - work_area.left - width) / 2;
    const auto top =
        work_area.top + (work_area.bottom - work_area.top - height) / 2;
    const auto window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"PalVerifyGiftcodeWindow",
        L"Palworld 3 Miền · Giftcode",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        left,
        top,
        width,
        height,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        &state
    );
    if (window == nullptr) {
        return std::nullopt;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message) == FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.result;
}

auto open_default_browser(std::string_view url) -> bool
{
    const auto wide_url = utf8_to_wide(url);
    if (!wide_url.has_value()) {
        return false;
    }
    const auto result = ShellExecuteW(
        nullptr,
        L"open",
        wide_url->c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );
    return reinterpret_cast<INT_PTR>(result) > 32;
}

}  // namespace palverify::client_ui
