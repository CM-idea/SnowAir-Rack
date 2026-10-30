#include "NoteEditor.h"
#include "Util.h"

#include <commctrl.h>

namespace {

constexpr int kIdSave = 1;
constexpr int kIdCancel = 2;
constexpr int kIdClose = 3;

struct EditorState {
    HINSTANCE instance = nullptr;
    HWND edit = nullptr;
    HWND save = nullptr;
    HWND cancel = nullptr;
    HWND title = nullptr;
    HWND close = nullptr;
    HFONT titleFont = nullptr;
    HBRUSH background = nullptr;
    bool dark = false;
    bool accepted = false;
    std::wstring text;
};

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR)
{
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(kIdSave, BN_CLICKED), 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(kIdCancel, BN_CLICKED), 0);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<EditorState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE: {
        if (!state) {
            return -1;
        }
        const COLORREF background = state->dark ? RGB(38, 39, 43) : RGB(255, 255, 255);
        state->background = CreateSolidBrush(background);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int pad = 14;
        const int headerH = 28;
        const int buttonW = 88;
        const int buttonH = 30;
        const int footerH = buttonH + 18;
        const int editTop = pad + headerH + 6;
        const int editH = client.bottom - editTop - footerH;
        const UINT dpi = GetDpiForWindow(hwnd);
        const int titlePt = MulDiv(14, static_cast<int>(dpi), 96);

        state->titleFont = CreateFontW(
            -titlePt, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
            L"Microsoft YaHei UI");

        state->title = CreateWindowExW(
            0, L"STATIC", L"给这张截图写一句用途", WS_CHILD | WS_VISIBLE | SS_LEFT,
            pad, pad + 2, client.right - pad * 2 - 34, headerH, hwnd, nullptr, state->instance,
            nullptr);
        state->close = CreateWindowExW(
            0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            client.right - pad - 28, pad, 28, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClose)), state->instance, nullptr);

        state->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN,
            pad, editTop, client.right - pad * 2, editH, hwnd, nullptr, state->instance,
            nullptr);
        state->save = CreateWindowExW(
            0, L"BUTTON", L"保存",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, client.right - pad - buttonW,
            client.bottom - pad - buttonH, buttonW, buttonH, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSave)), state->instance, nullptr);
        state->cancel = CreateWindowExW(
            0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            client.right - pad - buttonW * 2 - 8, client.bottom - pad - buttonH, buttonW,
            buttonH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)),
            state->instance, nullptr);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND control : {state->edit, state->save, state->cancel}) {
            if (control) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
        if (state->title && state->titleFont) {
            SendMessageW(state->title, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->titleFont), TRUE);
        }
        if (state->close) {
            SendMessageW(state->close, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (state->edit) {
            SetWindowSubclass(state->edit, EditSubclassProc, 1, 0);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
        }
        return 0;
    }
    case WM_COMMAND:
        if (!state) {
            break;
        }
        if (LOWORD(wParam) == kIdSave) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring value(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
            if (length > 0) {
                GetWindowTextW(state->edit, value.data(), length + 1);
            }
            value.resize(static_cast<size_t>(std::max(0, length)));
            state->text = std::move(value);
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == kIdCancel || LOWORD(wParam) == kIdClose) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        if (!state) {
            break;
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, state->dark ? RGB(232, 233, 238) : RGB(30, 34, 40));
        SetBkColor(dc, state->dark ? RGB(38, 39, 43) : RGB(255, 255, 255));
        HBRUSH brush = state->background
                           ? state->background
                           : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_CTLCOLORSTATIC: {
        if (!state) {
            break;
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, state->dark ? RGB(232, 233, 238) : RGB(30, 34, 40));
        SetBkColor(dc, state->dark ? RGB(38, 39, 43) : RGB(255, 255, 255));
        HBRUSH brush = state->background
                           ? state->background
                           : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            if (state->titleFont) {
                DeleteObject(state->titleFont);
                state->titleFont = nullptr;
            }
            if (state->background) {
                DeleteObject(state->background);
                state->background = nullptr;
            }
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HBRUSH brush = (state && state->background)
                           ? state->background
                           : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        FillRect(dc, &rect, brush);
        return 1;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} /* namespace */

bool NoteEditor::Edit(HWND owner, HINSTANCE instance, const std::wstring& initial, bool dark,
                      std::wstring& text)
{
    if (!instance) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &EditorProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SnowAirRackNoteEditor";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    EditorState state;
    state.instance = instance;
    state.dark = dark;
    state.text = initial;

    RECT anchor{0, 0, 0, 0};
    if (owner && IsWindow(owner)) {
        GetWindowRect(owner, &anchor);
    }
    HMONITOR monitor = MonitorFromWindow(owner ? owner : GetForegroundWindow(),
                                         MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    const float scale = static_cast<float>(dpi) / 96.0f;
    const int width = static_cast<int>(std::lround(400.0f * scale));
    const int height = static_cast<int>(std::lround(260.0f * scale));

    int x = anchor.left + (anchor.right - anchor.left) / 2 - width / 2;
    int y = (anchor.bottom > anchor.top ? anchor.bottom : info.rcWork.top) + 16;
    x = std::max<int>(info.rcWork.left + 8, std::min<int>(x, info.rcWork.right - width - 8));
    y = std::max<int>(info.rcWork.top + 8, std::min<int>(y, info.rcWork.bottom - height - 8));

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SnowAirRackNoteEditor",
                                L"纸条", WS_POPUP | WS_BORDER, x, y, width, height, owner,
                                nullptr, instance, &state);
    if (!hwnd) {
        return false;
    }

    if (owner) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    if (state.edit) {
        SetFocus(state.edit);
    }

    MSG msg{};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (state.accepted) {
        text = state.text;
    }
    return state.accepted;
}
