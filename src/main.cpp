#include "multipaste/core.hpp"

#include <windows.h>
#include <commctrl.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/base.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kWindowClass[] = L"MultiPaste.ClipboardWindow.V2";
constexpr wchar_t kSettingsClass[] = L"MultiPaste.SettingsWindow.V2";
constexpr wchar_t kAppName[] = L"MultiPaste";

constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kHistoryCleanupFailed = WM_APP + 2;
constexpr UINT kHotkeySmart = 1001;
constexpr UINT kHotkeyRaw = 1002;
constexpr UINT kTransactionTimer = 1101;
constexpr UINT kMenuSettings = 2001;
constexpr UINT kMenuExit = 2003;
constexpr UINT kControlSmart = 3001;
constexpr UINT kControlRaw = 3002;
constexpr UINT kControlSave = 3003;
constexpr UINT kControlCancel = 3004;
constexpr UINT kControlSeparator = 3005;
constexpr UINT kControlStartup = 3006;

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MultiPaste";

enum class Failure {
    None,
    Busy,
    ClipboardUnavailable,
    NoTextClipboard,
    CopiedContentNotText,
    CopyInjectionFailed,
    CopyTimeout,
    WriteFailed,
    HistoryCleanupFailed,
};

struct AppConfig {
    multipaste::HotkeySpec smart{MOD_CONTROL | MOD_ALT, L'C'};
    multipaste::HotkeySpec raw{MOD_CONTROL | MOD_SHIFT, L'C'};
    std::wstring separator_expression{L"\\s"};
};

void secure_clear(std::wstring& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        text.clear();
    }
}

class ConfigStore {
public:
    ConfigStore() {
        PWSTR local_app_data = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                           nullptr, &local_app_data))) {
            const std::filesystem::path directory =
                std::filesystem::path(local_app_data) / L"MultiPaste";
            CoTaskMemFree(local_app_data);
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            path_ = directory / L"settings.ini";
        }
    }

    AppConfig load() const {
        AppConfig config;
        if (path_.empty()) return config;
        config.smart.modifiers = read_uint(L"SmartModifiers", config.smart.modifiers);
        config.smart.virtual_key = read_uint(L"SmartKey", config.smart.virtual_key);
        config.raw.modifiers = read_uint(L"RawModifiers", config.raw.modifiers);
        config.raw.virtual_key = read_uint(L"RawKey", config.raw.virtual_key);
        config.separator_expression = read_string(L"Merge", L"Separator", L"\\s");
        std::wstring decoded;
        if (config.separator_expression.size() > 32U ||
            !multipaste::decode_separator_expression(config.separator_expression, decoded)) {
            config.separator_expression = L"\\s";
        }
        if (!multipaste::is_hotkey_valid(config.smart)) {
            config.smart = {MOD_CONTROL | MOD_ALT, L'C'};
        }
        if (!multipaste::is_hotkey_valid(config.raw)) {
            config.raw = {MOD_CONTROL | MOD_SHIFT, L'C'};
        }
        return config;
    }

    bool save(const AppConfig& config) const {
        if (path_.empty()) return false;
        return write_uint(L"SmartModifiers", config.smart.modifiers) &&
               write_uint(L"SmartKey", config.smart.virtual_key) &&
               write_uint(L"RawModifiers", config.raw.modifiers) &&
               write_uint(L"RawKey", config.raw.virtual_key) &&
               write_string(L"Merge", L"Separator", config.separator_expression);
    }

private:
    unsigned int read_uint(const wchar_t* key, unsigned int fallback) const {
        return GetPrivateProfileIntW(L"Hotkeys", key, static_cast<INT>(fallback),
                                     path_.c_str());
    }

    bool write_uint(const wchar_t* key, unsigned int value) const {
        const std::wstring text = std::to_wstring(value);
        return WritePrivateProfileStringW(L"Hotkeys", key, text.c_str(),
                                          path_.c_str()) != FALSE;
    }

    std::wstring read_string(const wchar_t* section, const wchar_t* key,
                             const wchar_t* fallback) const {
        wchar_t buffer[64]{};
        GetPrivateProfileStringW(section, key, fallback, buffer,
                                 static_cast<DWORD>(std::size(buffer)), path_.c_str());
        return buffer;
    }

    bool write_string(const wchar_t* section, const wchar_t* key,
                      const std::wstring& value) const {
        return WritePrivateProfileStringW(section, key, value.c_str(),
                                          path_.c_str()) != FALSE;
    }

    std::filesystem::path path_;
};

std::wstring executable_path() {
    std::wstring path(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) return {};
    path.resize(length);
    return path;
}

bool is_startup_enabled() {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue,
                                        RRF_RT_REG_SZ, nullptr, value, &bytes);
    const std::wstring path = executable_path();
    return status == ERROR_SUCCESS && !path.empty() &&
           std::wstring(value) == L"\"" + path + L"\"";
}

bool set_startup_enabled(bool enabled) {
    if (!enabled) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ||
               status == ERROR_PATH_NOT_FOUND;
    }
    const std::wstring path = executable_path();
    if (path.empty() || path.find(L'\"') != std::wstring::npos) return false;
    const std::wstring command = L"\"" + path + L"\"";
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(key, kRunValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool open_clipboard_with_retry(HWND owner) {
    constexpr int delays_ms[] = {0, 5, 10, 20, 40, 80};
    for (const int delay : delays_ms) {
        if (delay > 0) Sleep(static_cast<DWORD>(delay));
        if (OpenClipboard(owner) != FALSE) return true;
    }
    return false;
}

Failure read_clipboard_text(HWND owner, std::wstring& output, bool newly_copied) {
    if (!open_clipboard_with_retry(owner)) return Failure::ClipboardUnavailable;
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        CloseClipboard();
        return newly_copied ? Failure::CopiedContentNotText : Failure::NoTextClipboard;
    }
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text == nullptr) {
        CloseClipboard();
        return Failure::ClipboardUnavailable;
    }
    output.assign(text);
    GlobalUnlock(data);
    CloseClipboard();
    return Failure::None;
}

Failure write_clipboard_text(HWND owner, const std::wstring& text) {
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) return Failure::WriteFailed;
    auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return Failure::WriteFailed;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);

    if (!open_clipboard_with_retry(owner)) {
        GlobalFree(memory);
        return Failure::ClipboardUnavailable;
    }
    if (EmptyClipboard() == FALSE || SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        CloseClipboard();
        GlobalFree(memory);
        return Failure::WriteFailed;
    }
    CloseClipboard();
    return Failure::None;
}

bool key_is_down(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool hotkey_is_down(const multipaste::HotkeySpec& hotkey) {
    if (key_is_down(static_cast<int>(hotkey.virtual_key))) return true;
    if ((hotkey.modifiers & MOD_CONTROL) != 0U && key_is_down(VK_CONTROL)) return true;
    if ((hotkey.modifiers & MOD_SHIFT) != 0U && key_is_down(VK_SHIFT)) return true;
    if ((hotkey.modifiers & MOD_ALT) != 0U && key_is_down(VK_MENU)) return true;
    if ((hotkey.modifiers & MOD_WIN) != 0U &&
        (key_is_down(VK_LWIN) || key_is_down(VK_RWIN))) return true;
    return false;
}

bool send_standard_copy() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = L'C';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = L'C';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) ==
           std::size(inputs);
}

unsigned char modifiers_to_hotkey_flags(unsigned int modifiers) {
    unsigned char flags = 0;
    if ((modifiers & MOD_CONTROL) != 0U) flags |= HOTKEYF_CONTROL;
    if ((modifiers & MOD_ALT) != 0U) flags |= HOTKEYF_ALT;
    if ((modifiers & MOD_SHIFT) != 0U) flags |= HOTKEYF_SHIFT;
    return flags;
}

multipaste::HotkeySpec hotkey_from_control(HWND control) {
    const LRESULT value = SendMessageW(control, HKM_GETHOTKEY, 0, 0);
    const unsigned int virtual_key = LOBYTE(LOWORD(value));
    const unsigned int flags = HIBYTE(LOWORD(value));
    unsigned int modifiers = 0;
    if ((flags & HOTKEYF_CONTROL) != 0U) modifiers |= MOD_CONTROL;
    if ((flags & HOTKEYF_ALT) != 0U) modifiers |= MOD_ALT;
    if ((flags & HOTKEYF_SHIFT) != 0U) modifiers |= MOD_SHIFT;
    return {modifiers, virtual_key};
}

void set_hotkey_control(HWND control, const multipaste::HotkeySpec& hotkey) {
    const WORD value = MAKEWORD(static_cast<BYTE>(hotkey.virtual_key),
                                modifiers_to_hotkey_flags(hotkey.modifiers));
    SendMessageW(control, HKM_SETHOTKEY, value, 0);
}

void cleanup_transient_history(HWND notify_window, std::wstring copied,
                               std::wstring final_text) {
    if (copied == final_text) {
        secure_clear(copied);
        secure_clear(final_text);
        return;
    }

    std::thread([notify_window, copied = std::move(copied),
                 final_text = std::move(final_text)]() mutable {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            using namespace winrt::Windows::ApplicationModel::DataTransfer;
            for (int attempt = 0; attempt < 4; ++attempt) {
                Sleep(attempt == 0 ? 180U : 120U);
                const auto result = Clipboard::GetHistoryItemsAsync().get();
                if (result.Status() != ClipboardHistoryItemsResultStatus::Success) break;
                const auto items = result.Items();
                if (items.Size() < 2U) continue;

                const auto newest = items.GetAt(0);
                const auto transient = items.GetAt(1);
                const auto newest_view = newest.Content();
                const auto transient_view = transient.Content();
                if (!newest_view.Contains(StandardDataFormats::Text()) ||
                    !transient_view.Contains(StandardDataFormats::Text())) {
                    continue;
                }
                const std::wstring newest_text = newest_view.GetTextAsync().get().c_str();
                const std::wstring transient_text = transient_view.GetTextAsync().get().c_str();
                const auto age = winrt::clock::now() - transient.Timestamp();
                if (newest_text == final_text && transient_text == copied &&
                    age >= std::chrono::seconds(0) && age <= std::chrono::seconds(5)) {
                    if (!Clipboard::DeleteItemFromHistory(transient)) {
                        PostMessageW(notify_window, kHistoryCleanupFailed, 0, 0);
                    }
                    break;
                }
            }
            winrt::uninit_apartment();
        } catch (...) {
            // History API availability varies. The final clipboard value is already correct.
        }
        secure_clear(copied);
        secure_clear(final_text);
    }).detach();
}

class App;
App* g_app = nullptr;
HWND g_settings_window = nullptr;

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance), config_(store_.load()) {}

    int run() {
        if (!register_window_classes()) return 1;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName,
                                  WS_OVERLAPPED, 0, 0, 0, 0,
                                  nullptr, nullptr, instance_, this);
        if (window_ == nullptr) return 1;
        if (AddClipboardFormatListener(window_) == FALSE) return 1;
        add_tray_icon();
        register_configured_hotkeys(true);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    const AppConfig& config() const { return config_; }

    bool apply_settings(const AppConfig& proposed, bool startup, std::wstring& error) {
        if (!multipaste::is_hotkey_valid(proposed.smart) ||
            !multipaste::is_hotkey_valid(proposed.raw)) {
            error = L"\u5feb\u6377\u952e\u5fc5\u987b\u5305\u542b Ctrl\u3001Alt \u6216 Shift\u3002";
            return false;
        }
        if (proposed.smart == proposed.raw) {
            error = L"\u4e24\u4e2a\u529f\u80fd\u4e0d\u80fd\u4f7f\u7528\u76f8\u540c\u5feb\u6377\u952e\u3002";
            return false;
        }
        std::wstring decoded;
        if (proposed.separator_expression.size() > 32U ||
            !multipaste::decode_separator_expression(proposed.separator_expression, decoded)) {
            error = L"\u586b\u5145\u7b26\u53f7\u65e0\u6548\uff0c\u8bf7\u4f7f\u7528\u666e\u901a\u6587\u5b57\u6216 \\s\u3001\\n\u3001\\t\u3001\\\\\u3002";
            return false;
        }
        const AppConfig previous = config_;
        const bool previous_startup = is_startup_enabled();
        unregister_hotkeys();
        config_ = proposed;
        if (!register_pair()) {
            const std::wstring conflict = !smart_registered_
                ? multipaste::hotkey_name(config_.smart)
                : multipaste::hotkey_name(config_.raw);
            unregister_hotkeys();
            config_ = previous;
            register_pair();
            error = L"\u5feb\u6377\u952e\u51b2\u7a81\uff1a" + conflict;
            return false;
        }
        if (!set_startup_enabled(startup)) {
            unregister_hotkeys();
            config_ = previous;
            register_pair();
            error = L"\u65e0\u6cd5\u66f4\u65b0\u5f00\u673a\u81ea\u542f\u8bbe\u7f6e\u3002";
            return false;
        }
        if (!store_.save(config_)) {
            set_startup_enabled(previous_startup);
            unregister_hotkeys();
            config_ = previous;
            register_pair();
            error = L"\u65e0\u6cd5\u4fdd\u5b58\u8bbe\u7f6e\u3002";
            return false;
        }
        return true;
    }

    bool startup_enabled() const { return is_startup_enabled(); }
    void open_settings();

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self != nullptr ? self->handle_message(window, message, wparam, lparam)
                               : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbar_created_message_ != 0U && message == taskbar_created_message_) {
            add_tray_icon();
            return 0;
        }
        switch (message) {
        case WM_HOTKEY:
            if (wparam == kHotkeySmart || wparam == kHotkeyRaw) {
                begin_transaction(wparam == kHotkeySmart
                    ? multipaste::MergeMode::SmartSpace : multipaste::MergeMode::Raw,
                    wparam == kHotkeySmart ? config_.smart : config_.raw);
            }
            return 0;
        case WM_TIMER:
            if (wparam == kTransactionTimer) on_transaction_timer();
            return 0;
        case WM_CLIPBOARDUPDATE:
            on_clipboard_update();
            return 0;
        case kHistoryCleanupFailed:
            notify_failure(Failure::HistoryCleanupFailed);
            return 0;
        case kTrayMessage: {
            const UINT event = LOWORD(static_cast<DWORD_PTR>(lparam));
            if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) show_tray_menu();
            if (event == WM_LBUTTONDBLCLK || event == NIN_SELECT) open_settings();
            return 0;
        }
        case WM_COMMAND:
            handle_command(LOWORD(wparam));
            return 0;
        case WM_DESTROY:
            cancel_transaction();
            RemoveClipboardFormatListener(window);
            unregister_hotkeys();
            remove_tray_icon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    bool register_window_classes();
    void add_tray_icon();
    void remove_tray_icon();
    void notify(const std::wstring& text);
    void notify_failure(Failure failure);
    void show_tray_menu();
    void handle_command(UINT command);
    bool register_pair();
    void register_configured_hotkeys(bool show_conflicts);
    void unregister_hotkeys();
    void begin_transaction(multipaste::MergeMode mode,
                           const multipaste::HotkeySpec& hotkey);
    void on_transaction_timer();
    void on_clipboard_update();
    void cancel_transaction();

    HINSTANCE instance_{};
    HWND window_{};
    NOTIFYICONDATAW tray_{};
    ConfigStore store_;
    AppConfig config_;
    multipaste::TransactionModel transaction_;
    multipaste::HotkeySpec active_hotkey_{};
    std::wstring base_text_;
    DWORD self_write_sequence_{};
    bool smart_registered_{};
    bool raw_registered_{};
    const UINT taskbar_created_message_{RegisterWindowMessageW(L"TaskbarCreated")};
};

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        CreateWindowW(L"STATIC", L"\u667a\u80fd\u586b\u5145\u8ffd\u52a0",
                      WS_CHILD | WS_VISIBLE, 20, 24, 120, 22,
                      window, nullptr, nullptr, nullptr);
        HWND smart = CreateWindowW(HOTKEY_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP, 150, 20, 220, 25,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlSmart)),
            nullptr, nullptr);
        CreateWindowW(L"STATIC", L"\u65e0\u586b\u5145\u539f\u6837\u8ffd\u52a0",
                      WS_CHILD | WS_VISIBLE, 20, 66, 120, 22,
                      window, nullptr, nullptr, nullptr);
        HWND raw = CreateWindowW(HOTKEY_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP, 150, 62, 220, 25,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlRaw)),
            nullptr, nullptr);
        CreateWindowW(L"STATIC", L"\u667a\u80fd\u586b\u5145\u7b26\u53f7",
                      WS_CHILD | WS_VISIBLE, 20, 108, 120, 22,
                      window, nullptr, nullptr, nullptr);
        HWND separator = CreateWindowW(L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
            150, 104, 220, 25, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlSeparator)),
            nullptr, nullptr);
        SendMessageW(separator, EM_SETLIMITTEXT, 32, 0);
        CreateWindowW(L"STATIC", L"\u652f\u6301 \\s \u7a7a\u683c\u3001\\n \u6362\u884c\u3001\\t \u5236\u8868\u7b26\u3001\\\\ \u53cd\u659c\u6760",
                      WS_CHILD | WS_VISIBLE, 150, 134, 240, 22,
                      window, nullptr, nullptr, nullptr);
        HWND startup = CreateWindowW(L"BUTTON", L"\u5f00\u673a\u81ea\u52a8\u542f\u52a8",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            150, 162, 160, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlStartup)),
            nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"\u4fdd\u5b58",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            204, 208, 80, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlSave)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"\u53d6\u6d88",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 294, 208, 80, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlCancel)), nullptr, nullptr);
        EnumChildWindows(window, [](HWND child, LPARAM font_param) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font_param), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));
        if (g_app != nullptr) {
            set_hotkey_control(smart, g_app->config().smart);
            set_hotkey_control(raw, g_app->config().raw);
            SetWindowTextW(separator, g_app->config().separator_expression.c_str());
            SendMessageW(startup, BM_SETCHECK,
                         g_app->startup_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        SetFocus(smart);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kControlSave && g_app != nullptr) {
            wchar_t separator[33]{};
            GetWindowTextW(GetDlgItem(window, kControlSeparator), separator,
                           static_cast<int>(std::size(separator)));
            AppConfig proposed{
                hotkey_from_control(GetDlgItem(window, kControlSmart)),
                hotkey_from_control(GetDlgItem(window, kControlRaw)),
                separator,
            };
            const bool startup = SendMessageW(GetDlgItem(window, kControlStartup),
                                               BM_GETCHECK, 0, 0) == BST_CHECKED;
            std::wstring error;
            if (!g_app->apply_settings(proposed, startup, error)) {
                MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == kControlCancel) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_settings_window = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool App::register_window_classes() {
    WNDCLASSEXW main_class{sizeof(WNDCLASSEXW)};
    main_class.lpfnWndProc = window_proc;
    main_class.hInstance = instance_;
    main_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&main_class) == 0) return false;

    WNDCLASSEXW settings_class{sizeof(WNDCLASSEXW)};
    settings_class.lpfnWndProc = settings_proc;
    settings_class.hInstance = instance_;
    settings_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settings_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    settings_class.lpszClassName = kSettingsClass;
    return RegisterClassExW(&settings_class) != 0;
}

void App::add_tray_icon() {
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = window_;
    tray_.uID = kTrayId;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = kTrayMessage;
    tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(tray_.szTip, std::size(tray_.szTip), kAppName);
    Shell_NotifyIconW(NIM_ADD, &tray_);
    tray_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &tray_);
}

void App::remove_tray_icon() {
    if (tray_.hWnd != nullptr) Shell_NotifyIconW(NIM_DELETE, &tray_);
}

void App::notify(const std::wstring& text) {
    tray_.uFlags = NIF_INFO;
    StringCchCopyW(tray_.szInfoTitle, std::size(tray_.szInfoTitle), kAppName);
    StringCchCopyW(tray_.szInfo, std::size(tray_.szInfo), text.c_str());
    tray_.dwInfoFlags = NIIF_WARNING | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::notify_failure(Failure failure) {
    switch (failure) {
    case Failure::Busy: notify(L"\u6b63\u5728\u5904\u7406\u4e0a\u4e00\u6b21\u590d\u5236"); break;
    case Failure::ClipboardUnavailable: notify(L"\u526a\u8d34\u677f\u6682\u65f6\u4e0d\u53ef\u7528"); break;
    case Failure::NoTextClipboard: notify(L"\u5f53\u524d\u526a\u8d34\u677f\u4e0d\u662f\u6587\u672c"); break;
    case Failure::CopiedContentNotText: notify(L"\u65b0\u590d\u5236\u5185\u5bb9\u4e0d\u662f\u6587\u672c"); break;
    case Failure::CopyInjectionFailed: notify(L"\u65e0\u6cd5\u53d1\u9001\u590d\u5236\u547d\u4ee4"); break;
    case Failure::CopyTimeout: notify(L"\u672a\u68c0\u6d4b\u5230\u65b0\u7684\u590d\u5236\u5185\u5bb9"); break;
    case Failure::WriteFailed: notify(L"\u5199\u5165\u526a\u8d34\u677f\u5931\u8d25"); break;
    case Failure::HistoryCleanupFailed:
        notify(L"\u5df2\u5408\u5e76\uff0c\u4e34\u65f6\u7247\u6bb5\u53ef\u80fd\u4fdd\u7559\u5728 Win+V");
        break;
    case Failure::None: break;
    }
}

void App::show_tray_menu() {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"\u9762\u677f");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"\u9000\u51fa");
    SetForegroundWindow(window_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
}

void App::handle_command(UINT command) {
    switch (command) {
    case kMenuSettings:
        open_settings();
        break;

    case kMenuExit:
        DestroyWindow(window_);
        break;
    default:
        break;
    }
}

bool App::register_pair() {
    smart_registered_ = RegisterHotKey(window_, kHotkeySmart,
        config_.smart.modifiers | MOD_NOREPEAT, config_.smart.virtual_key) != FALSE;
    raw_registered_ = RegisterHotKey(window_, kHotkeyRaw,
        config_.raw.modifiers | MOD_NOREPEAT, config_.raw.virtual_key) != FALSE;
    return smart_registered_ && raw_registered_;
}

void App::register_configured_hotkeys(bool show_conflicts) {
    smart_registered_ = RegisterHotKey(window_, kHotkeySmart,
        config_.smart.modifiers | MOD_NOREPEAT, config_.smart.virtual_key) != FALSE;
    raw_registered_ = RegisterHotKey(window_, kHotkeyRaw,
        config_.raw.modifiers | MOD_NOREPEAT, config_.raw.virtual_key) != FALSE;
    if (show_conflicts && !smart_registered_) {
        notify(L"\u5feb\u6377\u952e\u51b2\u7a81\uff1a" +
               multipaste::hotkey_name(config_.smart));
    }
    if (show_conflicts && !raw_registered_) {
        notify(L"\u5feb\u6377\u952e\u51b2\u7a81\uff1a" +
               multipaste::hotkey_name(config_.raw));
    }
}

void App::unregister_hotkeys() {
    if (smart_registered_) UnregisterHotKey(window_, kHotkeySmart);
    if (raw_registered_) UnregisterHotKey(window_, kHotkeyRaw);
    smart_registered_ = false;
    raw_registered_ = false;
}

void App::begin_transaction(multipaste::MergeMode mode,
                            const multipaste::HotkeySpec& hotkey) {
    if (transaction_.state() != multipaste::TransactionState::Idle) {
        notify_failure(Failure::Busy);
        return;
    }
    std::wstring base;
    const Failure failure = read_clipboard_text(window_, base, false);
    if (failure != Failure::None) {
        notify_failure(failure);
        return;
    }
    const DWORD sequence = GetClipboardSequenceNumber();
    if (!transaction_.begin(mode, sequence, GetTickCount64())) {
        secure_clear(base);
        notify_failure(Failure::Busy);
        return;
    }
    secure_clear(base_text_);
    base_text_ = std::move(base);
    active_hotkey_ = hotkey;
    SetTimer(window_, kTransactionTimer, 25U, nullptr);
}

void App::on_transaction_timer() {
    const auto action = transaction_.on_timer(GetTickCount64(),
                                               hotkey_is_down(active_hotkey_));
    if (action == multipaste::TransactionAction::SendCopy) {
        if (!send_standard_copy()) {
            cancel_transaction();
            notify_failure(Failure::CopyInjectionFailed);
        }
    } else if (action == multipaste::TransactionAction::Timeout) {
        KillTimer(window_, kTransactionTimer);
        secure_clear(base_text_);
        notify_failure(Failure::CopyTimeout);
    }
}

void App::on_clipboard_update() {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == self_write_sequence_) return;
    if (transaction_.on_clipboard_changed(sequence) !=
        multipaste::TransactionAction::CaptureClipboard) {
        return;
    }

    KillTimer(window_, kTransactionTimer);
    std::wstring copied;
    const Failure read_failure = read_clipboard_text(window_, copied, true);
    if (read_failure != Failure::None) {
        secure_clear(base_text_);
        notify_failure(read_failure);
        return;
    }

    std::wstring separator;
    multipaste::decode_separator_expression(config_.separator_expression, separator);
    std::wstring final_text =
        multipaste::merge_text(base_text_, copied, transaction_.mode(), separator);
    const Failure write_failure = write_clipboard_text(window_, final_text);
    secure_clear(base_text_);
    if (write_failure != Failure::None) {
        secure_clear(copied);
        secure_clear(final_text);
        notify_failure(write_failure);
        return;
    }
    self_write_sequence_ = GetClipboardSequenceNumber();
    cleanup_transient_history(window_, std::move(copied), std::move(final_text));
}

void App::cancel_transaction() {
    KillTimer(window_, kTransactionTimer);
    transaction_.cancel();
    secure_clear(base_text_);
}

void App::open_settings() {
    if (g_settings_window != nullptr) {
        SetForegroundWindow(g_settings_window);
        return;
    }
    g_settings_window = CreateWindowExW(WS_EX_TOOLWINDOW, kSettingsClass,
        L"MultiPaste \u9762\u677f", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 430, 290, nullptr, nullptr, instance_, nullptr);
    if (g_settings_window != nullptr) {
        ShowWindow(g_settings_window, SW_SHOW);
        UpdateWindow(g_settings_window);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    OleInitialize(nullptr);
    App app(instance);
    g_app = &app;
    const int result = app.run();
    g_app = nullptr;
    OleUninitialize();
    return result;
}
