#include <Windows.h>
#include <UIAutomation.h>
#include <ole2.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

constexpr wchar_t APP_NAME[] = L"ActionRecorder";
constexpr char APP_VERSION[] = "0.0.2-alpha";
constexpr int APP_VERSION_MAJOR = 0;
constexpr int APP_VERSION_MINOR = 0;
constexpr int APP_VERSION_PATCH = 2;
constexpr UINT WM_APP_LOG_EVENT = WM_APP + 1;

HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
DWORD g_mainThreadId = 0;
IUIAutomation* g_uiAutomation = nullptr;
std::ofstream g_log;
POINT g_lastMousePoint{0, 0};

enum class EventKind {
    KeyDown,
    KeyUp,
    MouseDown,
    MouseUp,
    MouseMove
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

std::wstring Trim(const std::wstring& value) {
    const std::size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return {};
    }
    const std::size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

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

std::filesystem::path PromptPath() {
    std::wcout << L"Введите путь для сохранения лог-файлов:\n> ";
    std::wstring input;
    std::getline(std::wcin, input);
    input = Trim(input);
    if (input.empty()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(input);
}

std::wstring PromptName() {
    std::wcout << L"Введите имя файла без расширения:\n> ";
    std::wstring input;
    std::getline(std::wcin, input);
    input = Trim(input);
    if (input.empty()) {
        input = L"log";
    }
    for (wchar_t& ch : input) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' ||
            ch == L'|') {
            ch = L'_';
        }
    }
    return input;
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &seconds);
#else
    localtime_r(&seconds, &localTime);
#endif
    const auto sinceSecond =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            % 1000;

    std::ostringstream output;
    output << std::setfill('0')
           << std::setw(2) << localTime.tm_hour << ':'
           << std::setw(2) << localTime.tm_min << ':'
           << std::setw(2) << localTime.tm_sec << '.'
           << std::setw(3) << sinceSecond.count();
    return output.str();
}

std::string FileTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &seconds);
#else
    localtime_r(&seconds, &localTime);
#endif
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

std::filesystem::path CreateLogFile(
    const std::filesystem::path& directory, const std::wstring& name) {
    std::filesystem::create_directories(directory);
    const std::string asciiTimestamp = FileTimestamp();
    const std::wstring wideTimestamp(
        asciiTimestamp.begin(), asciiTimestamp.end());
    std::filesystem::path result =
        directory / (name + L"_" + wideTimestamp + L".txt");
    g_log.open(result, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!g_log.is_open()) {
        throw std::runtime_error("Не удалось открыть лог-файл");
    }
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    g_log.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    g_log << "ActionRecorder alpha 0.0.2 — лог действий пользователя\r\n"
          << "Для нейросети: каждая строка имеет формат "
             "[TIMESTAMP] | [OBJECT_IDENTIFIER] | [ACTION] | [DURATION_MS]. "
             "TIMESTAMP — локальное время ЧЧ:ММ:СС.ммм. "
             "OBJECT_IDENTIFIER — UIA:ControlType(Name=\"[Name]\", "
             "AutomationId=\"[Id]\") либо POS:Mouse(X=[x], Y=[y]). "
             "ACTION описывает клавишу или действие мыши. "
             "DURATION_MS — длительность в миллисекундах.\r\n"
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
        return {};
    }
    VARIANT value;
    VariantInit(&value);
    if (FAILED(element->GetCurrentPropertyValue(
            UIA_ControlTypePropertyId, &value))) {
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
        duration = 0;
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
        action = event.mouseCode == 0 ? "MouseDown(LEFT)" :
                                        "MouseDown(RIGHT)";
    } else if (event.kind == EventKind::MouseUp) {
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

        g_lastActionTime = now;
        g_log << Timestamp() << " | " << ObjectIdentifier(event)
              << " | " << action << " | " << duration << "\r\n";
        action = std::string("MouseUp(") + button + ")";
    } else {
        duration = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_lastActionTime).count());
        action = "MouseMove";
    }

    g_lastActionTime = now;
    g_log << Timestamp() << " | " << ObjectIdentifier(event)
          << " | " << action << " | " << duration << "\r\n";
    g_log.flush();
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN ||
            wparam == WM_KEYUP || wparam == WM_SYSKEYUP) {
            const bool isDown =
                wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (isDown && ctrl && shift && info->vkCode == 'Q') {
                PostThreadMessage(g_mainThreadId, WM_QUIT, 0, 0);
            }
            auto* event = new (std::nothrow) LogEvent{
                isDown ? EventKind::KeyDown : EventKind::KeyUp,
                info->vkCode,
                0,
                0,
                0};
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
        EventKind kind = EventKind::MouseMove;
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
        } else if (wparam != WM_MOUSEMOVE) {
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

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::wcout << L"ActionRecorder alpha 0.0.2\n";

    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        std::wcerr << L"Не удалось инициализировать COM.\n";
        return 1;
    }

    try {
        std::filesystem::path directory = PromptPath();
        std::wstring name = PromptName();
        std::filesystem::path logPath = CreateLogFile(directory, name);
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
    }

    g_mainThreadId = GetCurrentThreadId();
    g_keyboardHook = SetWindowsHookEx(
        WH_KEYBOARD_LL, KeyboardHook, GetModuleHandle(nullptr), 0);
    g_mouseHook = SetWindowsHookEx(
        WH_MOUSE_LL, MouseHook, GetModuleHandle(nullptr), 0);

    if (g_keyboardHook == nullptr || g_mouseHook == nullptr) {
        std::wcerr << L"Не удалось установить один или оба глобальных хука.\n";
    } else {
        std::wcout << L"Запись активна. Завершение: Ctrl+Shift+Q\n";
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
