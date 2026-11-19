#include <windows.h>
#include <uiautomation.h>
#include <combaseapi.h>
#include <shlobj.h>
#include <oleauto.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

constexpr wchar_t APP_NAME[] = L"ActionRecorder";
constexpr char APP_VERSION[] = "0.0.1-alpha";
constexpr int APP_VERSION_MAJOR = 0;
constexpr int APP_VERSION_MINOR = 0;
constexpr int APP_VERSION_PATCH = 1;
constexpr UINT WM_APP_LOG_EVENT = WM_APP + 1;

HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
DWORD g_mainThreadId = 0;
IUIAutomation* g_uiAutomation = nullptr;
std::ofstream g_log;
std::wstring g_targetExe;
POINT g_lastMousePoint{0, 0};

enum class EventKind {
    KeyDown,
    KeyUp,
    MouseDown,
    MouseUp
};

struct LogEvent {
    EventKind kind;
    DWORD vkCode;
    int mouseCode;
    LONG x;
    LONG y;
};

std::unordered_map<DWORD, std::chrono::steady_clock::time_point> g_keyDownTimes;
std::unordered_map<int, std::chrono::steady_clock::time_point> g_mouseDownTimes;
std::chrono::steady_clock::time_point g_lastActionTime =
    std::chrono::steady_clock::now();

std::string Utf8FromWide(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8.data(), size - 1,
        nullptr, nullptr);
    return utf8;
}

std::wstring WideFromUtf8(const char* value) {
    if (value == nullptr || *value == '\0') {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide.data(), size - 1);
    return wide;
}

std::string EscapeUtf8(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::wstring GetExeNameForProcess(DWORD processId) {
    if (processId == 0) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    DWORD size = ARRAYSIZE(path);
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
    CloseHandle(process);
    if (!ok) {
        return {};
    }
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash != nullptr ? std::wstring(slash + 1) : std::wstring(path);
}

bool IsTargetForeground() {
    if (g_targetExe.empty()) {
        return false;
    }
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    const std::wstring exe = GetExeNameForProcess(processId);
    return _wcsicmp(exe.c_str(), g_targetExe.c_str()) == 0;
}

std::wstring ParseTarget(const wchar_t* commandLine) {
    if (commandLine == nullptr) {
        return {};
    }
    constexpr wchar_t prefix[] = L"--target=";
    constexpr std::size_t prefixLength = sizeof(prefix) / sizeof(prefix[0]) - 1;
    const wchar_t* found = wcsstr(commandLine, prefix);
    if (found == nullptr) {
        return {};
    }
    const wchar_t* value = found + prefixLength;
    const wchar_t* end = value;
    while (*end != L'\0' && *end != L' ' && *end != L'\t') {
        ++end;
    }
    return std::wstring(value, end);
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &seconds);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

    std::ostringstream output;
    output << std::setfill('0')
           << std::setw(2) << localTime.tm_hour << ':'
           << std::setw(2) << localTime.tm_min << ':'
           << std::setw(2) << localTime.tm_sec << '.'
           << std::setw(3) << milliseconds.count();
    return output.str();
}

std::string FileTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &seconds);
    std::ostringstream output;
    output << std::setfill('0')
           << std::setw(4) << (localTime.tm_year + 1900)
           << std::setw(2) << (localTime.tm_mon + 1)
           << std::setw(2) << localTime.tm_mday << '_'
           << std::setw(2) << localTime.tm_hour
           << std::setw(2) << localTime.tm_min
           << std::setw(2) << localTime.tm_sec;
    return output.str();
}

std::wstring SelectSavePath() {
    IFileSaveDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr) {
        throw std::runtime_error("Не удалось создать стандартный диалог сохранения");
    }

    dialog->SetTitle(L"Сохранить лог тестового сценария");
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetDefaultExtension(L"txt");
    dialog->SetFileName(L"testcase");

    COMDLG_FILTERSPEC filters[] = {
        {L"Текстовые логи (*.txt)", L"*.txt"}
    };
    dialog->SetFileTypes(1, filters);
    dialog->SetFileTypeIndex(1);

    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(hr)) {
        dialog->Release();
        return {};
    }

    IShellItem* item = nullptr;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || item == nullptr) {
        dialog->Release();
        throw std::runtime_error("Не удалось получить выбранный файл");
    }

    wchar_t* path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    dialog->Release();
    if (FAILED(hr) || path == nullptr) {
        throw std::runtime_error("Не удалось получить полный путь файла");
    }

    std::wstring fullPath(path);
    CoTaskMemFree(path);
    return fullPath;
}

std::filesystem::path OpenLogFile(const std::wstring& selectedPath) {
    std::filesystem::path selected(selectedPath);
    std::filesystem::path directory = selected.parent_path();
    std::wstring base = selected.stem().wstring();
    if (base.empty()) {
        base = L"testcase";
    }
    for (wchar_t& ch : base) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' ||
            ch == L'|') {
            ch = L'_';
        }
    }

    std::filesystem::create_directories(directory);
    const std::string timestampAscii = FileTimestamp();
    const std::wstring timestamp(timestampAscii.begin(), timestampAscii.end());
    const std::filesystem::path result =
        directory / (base + timestamp + L".txt");

    g_log.open(result, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!g_log.is_open()) {
        throw std::runtime_error("Не удалось открыть лог-файл");
    }

    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    g_log.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    g_log
        << "ActionRecorder alpha 0.0.1 - лог действий тестируемого приложения\r\n"
        << "Для нейросети: каждая строка имеет формат "
           "[TIMESTAMP] | [OBJECT_IDENTIFIER] | [ACTION] | [DURATION_MS]. "
           "TIMESTAMP - локальное время ЧЧ:ММ:СС.ммм. "
           "OBJECT_IDENTIFIER - UIA:ControlType(Name=\"[Name]\", AutomationId=\"[Id]\") "
           "либо POS:Mouse(X=[x], Y=[y]). "
           "ACTION - действие мыши или код физической клавиши VK_CODE без сохранения текста. "
           "DURATION_MS - длительность нажатия или интервал до предыдущего действия.\r\n"
        << "Запись выполняется только для активного процесса, указанного в "
           "--target=<Имя.exe>. Введённый текст намеренно не записывается.\r\n"
        << "Кодировка файла: UTF-8 с BOM.\r\n\r\n";
    g_log.flush();
    return result;
}

std::string PropertyText(IUIAutomationElement* element, PROPERTYID propertyId) {
    if (element == nullptr) {
        return {};
    }
    VARIANT value;
    VariantInit(&value);
    if (FAILED(element->GetCurrentPropertyValue(propertyId, &value))) {
        VariantClear(&value);
        return {};
    }
    std::string text;
    if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
        text = Utf8FromWide(value.bstrVal);
    }
    VariantClear(&value);
    return text;
}

std::string ControlTypeText(IUIAutomationElement* element) {
    if (element == nullptr) {
        return "Unknown";
    }
    VARIANT value;
    VariantInit(&value);
    if (FAILED(element->GetCurrentPropertyValue(
            UIA_ControlTypePropertyId, &value))) {
        VariantClear(&value);
        return "Unknown";
    }
    std::string result = "Unknown";
    if (value.vt == VT_I4) {
        switch (value.lVal) {
            case UIA_ButtonControlTypeId: result = "Button"; break;
            case UIA_EditControlTypeId: result = "Edit"; break;
            case UIA_TextControlTypeId: result = "Text"; break;
            case UIA_WindowControlTypeId: result = "Window"; break;
            case UIA_ListControlTypeId: result = "List"; break;
            case UIA_ListItemControlTypeId: result = "ListItem"; break;
            case UIA_MenuControlTypeId: result = "Menu"; break;
            case UIA_MenuItemControlTypeId: result = "MenuItem"; break;
            case UIA_TabControlTypeId: result = "Tab"; break;
            case UIA_TabItemControlTypeId: result = "TabItem"; break;
            case UIA_CheckBoxControlTypeId: result = "CheckBox"; break;
            case UIA_RadioButtonControlTypeId: result = "RadioButton"; break;
            case UIA_ComboBoxControlTypeId: result = "ComboBox"; break;
            case UIA_HyperlinkControlTypeId: result = "Hyperlink"; break;
            case UIA_DataGridControlTypeId: result = "DataGrid"; break;
            case UIA_DataItemControlTypeId: result = "DataItem"; break;
            case UIA_ToolBarControlTypeId: result = "ToolBar"; break;
            case UIA_TreeControlTypeId: result = "Tree"; break;
            case UIA_TreeItemControlTypeId: result = "TreeItem"; break;
            case UIA_SliderControlTypeId: result = "Slider"; break;
            case UIA_ScrollBarControlTypeId: result = "ScrollBar"; break;
            case UIA_DocumentControlTypeId: result = "Document"; break;
            default: result = "Control"; break;
        }
    }
    VariantClear(&value);
    return result;
}

std::string ObjectIdentifier(const LogEvent& event) {
    if (g_uiAutomation != nullptr) {
        IUIAutomationElement* element = nullptr;
        HRESULT hr = E_FAIL;
        if (event.kind == EventKind::KeyDown ||
            event.kind == EventKind::KeyUp) {
            hr = g_uiAutomation->GetFocusedElement(&element);
        } else {
            POINT point{event.x, event.y};
            hr = g_uiAutomation->ElementFromPoint(point, &element);
        }
        if (SUCCEEDED(hr) && element != nullptr) {
            std::string name = EscapeUtf8(
                PropertyText(element, UIA_NamePropertyId));
            std::string automationId = EscapeUtf8(
                PropertyText(element, UIA_AutomationIdPropertyId));
            std::string controlType = EscapeUtf8(ControlTypeText(element));
            element->Release();
            return "UIA:" + controlType +
                   "(Name=\"" + name + "\", AutomationId=\"" +
                   automationId + "\")";
        }
    }
    if (event.kind == EventKind::KeyDown ||
        event.kind == EventKind::KeyUp) {
        std::ostringstream fallback;
        fallback << "POS:Mouse(X=" << g_lastMousePoint.x
                 << ", Y=" << g_lastMousePoint.y << ")";
        return fallback.str();
    }
    std::ostringstream fallback;
    fallback << "POS:Mouse(X=" << event.x << ", Y=" << event.y << ")";
    return fallback.str();
}

void WriteLogLine(const LogEvent& event) {
    std::string action;
    std::uint64_t duration = 0;
    const auto now = std::chrono::steady_clock::now();

    if (event.kind == EventKind::KeyDown) {
        g_keyDownTimes[event.vkCode] = now;
        action = "KeyPress(" + std::to_string(event.vkCode) + ")";
    } else if (event.kind == EventKind::KeyUp) {
        const auto found = g_keyDownTimes.find(event.vkCode);
        if (found != g_keyDownTimes.end()) {
            duration = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - found->second).count());
            g_keyDownTimes.erase(found);
        } else {
            duration = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_lastActionTime).count());
        }
        action = "KeyRelease(" + std::to_string(event.vkCode) + ")";
    } else if (event.kind == EventKind::MouseDown) {
        g_mouseDownTimes[event.mouseCode] = now;
        const char* button = event.mouseCode == 0 ? "LEFT" : "RIGHT";
        action = std::string("MouseDown(") + button + ")";
    } else {
        const auto found = g_mouseDownTimes.find(event.mouseCode);
        if (found != g_mouseDownTimes.end()) {
            duration = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - found->second).count());
            g_mouseDownTimes.erase(found);
        } else {
            duration = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_lastActionTime).count());
        }
        const char* button = event.mouseCode == 0 ? "LEFT" : "RIGHT";
        action = std::string("MouseClick(") + button + ")";
    }

    g_lastActionTime = now;
    g_log << Timestamp() << " | " << ObjectIdentifier(event)
          << " | " << action << " | " << duration << "\r\n";
    g_log.flush();
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        const bool isDown =
            wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool isUp =
            wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
        if (isDown && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            info->vkCode == 'Q') {
            PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
        } else if ((isDown || isUp) && IsTargetForeground()) {
            auto* event = new (std::nothrow) LogEvent{
                isDown ? EventKind::KeyDown : EventKind::KeyUp,
                info->vkCode,
                0,
                g_lastMousePoint.x,
                g_lastMousePoint.y};
            if (event != nullptr) {
                PostThreadMessage(g_mainThreadId, WM_APP_LOG_EVENT,
                                  0, reinterpret_cast<LPARAM>(event));
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
        g_lastMousePoint = info->pt;
        if (!IsTargetForeground()) {
            return CallNextHookEx(nullptr, code, wparam, lparam);
        }

        EventKind kind = EventKind::MouseUp;
        int mouseCode = 0;
        bool shouldLog = true;
        if (wparam == WM_LBUTTONDOWN) {
            kind = EventKind::MouseDown;
        } else if (wparam == WM_LBUTTONUP) {
            kind = EventKind::MouseUp;
        } else if (wparam == WM_RBUTTONDOWN) {
            kind = EventKind::MouseDown;
            mouseCode = 1;
        } else if (wparam == WM_RBUTTONUP) {
            kind = EventKind::MouseUp;
            mouseCode = 1;
        } else {
            shouldLog = false;
        }
        if (shouldLog) {
            auto* event = new (std::nothrow) LogEvent{
                kind, 0, mouseCode, info->pt.x, info->pt.y};
            if (event != nullptr) {
                PostThreadMessage(g_mainThreadId, WM_APP_LOG_EVENT,
                                  0, reinterpret_cast<LPARAM>(event));
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::wcout << L"ActionRecorder alpha 0.0.1\n";
    g_targetExe = ParseTarget(GetCommandLineW());
    if (g_targetExe.empty()) {
        std::wcout
            << L"Для явной записи укажите целевой процесс: "
               L"--target=ADHD-limiter.exe\n";
        std::wcout
            << L"Пример: ActionRecorder.exe --target=ADHD-limiter.exe\n";
    } else {
        std::wcout << L"Целевой процесс: " << g_targetExe << L"\n";
    }

    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        std::wcerr << L"Не удалось инициализировать COM.\n";
        return 1;
    }

    try {
        const std::wstring selectedPath = SelectSavePath();
        if (selectedPath.empty()) {
            std::wcout << L"Выбор файла отменён. Программа завершена.\n";
            CoUninitialize();
            return 0;
        }
        const std::filesystem::path logPath = OpenLogFile(selectedPath);
        std::wcout << L"Лог-файл: " << logPath.wstring() << L"\n";
    } catch (const std::exception& error) {
        std::wcerr << L"Ошибка создания лог-файла: "
                   << WideFromUtf8(error.what()) << L"\n";
        CoUninitialize();
        return 1;
    }

    HRESULT uiResult = CoCreateInstance(
        CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_IUIAutomation, reinterpret_cast<void**>(&g_uiAutomation));
    if (FAILED(uiResult)) {
        g_uiAutomation = nullptr;
        std::wcout << L"UI Automation недоступен; будет использован POS fallback.\n";
    }

    g_mainThreadId = GetCurrentThreadId();
    if (!g_targetExe.empty()) {
        g_keyboardHook = SetWindowsHookEx(
            WH_KEYBOARD_LL, KeyboardHook, GetModuleHandle(nullptr), 0);
        g_mouseHook = SetWindowsHookEx(
            WH_MOUSE_LL, MouseHook, GetModuleHandle(nullptr), 0);
    }

    if (g_targetExe.empty()) {
        std::wcout
            << L"Хуки не установлены: режим записи требует --target=<Имя.exe>. "
               L"Закройте консоль для выхода.\n";
    } else if (g_keyboardHook == nullptr || g_mouseHook == nullptr) {
        std::wcerr
            << L"Не удалось установить один или оба хука для целевого процесса.\n";
    } else {
        std::wcout
            << L"Явная запись активна. Завершение: Ctrl+Shift+Q.\n";
        std::wcout
            << L"Набранный текст не сохраняется; записываются только коды клавиш.\n";
    }

    MSG message{};
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_APP_LOG_EVENT &&
            message.lParam != 0) {
            auto* event =
                reinterpret_cast<LogEvent*>(message.lParam);
            WriteLogLine(*event);
            delete event;
        }
    }

    if (g_keyboardHook != nullptr) {
        UnhookWindowsHookEx(g_keyboardHook);
    }
    if (g_mouseHook != nullptr) {
        UnhookWindowsHookEx(g_mouseHook);
    }
    if (g_uiAutomation != nullptr) {
        g_uiAutomation->Release();
        g_uiAutomation = nullptr;
    }
    if (g_log.is_open()) {
        g_log.close();
    }
    std::wcout << L"Запись остановлена.\n";
    CoUninitialize();
    return 0;
}
