#include "NoteEditor.h"
#include "NotePalette.h"
#include "Util.h"

#include "Graphics.h"

#include <commdlg.h>

#include <cmath>

namespace {

/* 卡片尺寸全部按 96 DPI 的 DIP 设计，运行时再乘屏幕缩放。 */
constexpr float kShadow = 18.0f; /* 四周留给投影的透明边距 */
constexpr float kCardW = 384.0f;
constexpr float kCardH = 256.0f;
constexpr float kPad = 20.0f;
constexpr float kHeaderH = 26.0f;
constexpr float kCloseSize = 26.0f;
constexpr float kButtonW = 74.0f;
constexpr float kButtonH = 32.0f;
constexpr float kRadius = 16.0f;
constexpr float kFieldRadius = 6.0f;
constexpr float kButtonRadius = 6.0f;
constexpr float kNoteRadius = 4.0f;

/* 强调色色板：卡片类型默认色 + 七彩色块（打开系统颜色面板）。 */
constexpr uint32_t kAccentPresets[8] = {
    0x1677FF, 0x34C759, 0xFF8D28, 0xFF383C, 0xCB30E0, 0xF3CD5F, 0xAC7F5E, 0x515151,
};

constexpr UINT_PTR kTimerMorph = 11;
constexpr int kMorphOpenMs = 240;
constexpr int kMorphCloseMs = 180;
constexpr int kFlyMs = 380;     /* 便签从编辑位置返回夹子 */
constexpr int kAlphaMs = 130;   /* 落位后和卡片交叉淡出 */

D2D1_COLOR_F Opaque(D2D1_COLOR_F color)
{
    color.a = 1.0f;
    return color;
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return D2D1::ColorF(rack::Lerp(a.r, b.r, t), rack::Lerp(a.g, b.g, t),
                        rack::Lerp(a.b, b.b, t), rack::Lerp(a.a, b.a, t));
}

D2D1_COLOR_F White()
{
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
}

D2D1_COLOR_F HsvColor(float hue, float saturation, float value)
{
    float h = std::fmod(hue, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float c = value * saturation;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = value - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 60.0f) { r = c; g = x; }
    else if (h < 120.0f) { r = x; g = c; }
    else if (h < 180.0f) { g = c; b = x; }
    else if (h < 240.0f) { g = x; b = c; }
    else if (h < 300.0f) { r = x; b = c; }
    else { r = c; b = x; }
    return D2D1::ColorF(r + m, g + m, b + m, 1.0f);
}

D2D1_COLOR_F RgbColor(uint32_t rgb)
{
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

bool UsesDarkInk(const D2D1_COLOR_F& accent)
{
    return 0.299f * accent.r + 0.587f * accent.g + 0.114f * accent.b > 0.85f;
}

D2D1_COLOR_F Black()
{
    return D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
}

COLORREF ToColorRef(const D2D1_COLOR_F& color)
{
    auto channel = [](float value) {
        return static_cast<BYTE>(rack::Clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

int Px(float value)
{
    return static_cast<int>(std::lround(value));
}

int LerpInt(int a, int b, float t)
{
    return static_cast<int>(std::lround(static_cast<float>(a) + (b - a) * t));
}

} /* namespace */

bool NoteEditor::Edit(HWND owner, HINSTANCE instance, Graphics* graphics, const Theme& theme,
                      const std::wstring& title, const RECT& anchor,
                      const std::wstring& initial, std::wstring& text,
                      std::function<void()> onShown, std::function<void(const RECT&)> onClosed,
                      std::function<void()> onLanded, const CardLook& look,
                      std::function<void(uint32_t)> onAccent)
{
    if (!instance || !graphics) {
        return false;
    }

    UINT dpi = 0;
    if (owner && IsWindow(owner)) {
        dpi = GetDpiForWindow(owner);
    }
    if (!dpi) {
        dpi = GetDpiForSystem();
    }
    if (!dpi) {
        dpi = 96;
    }

    NoteEditor editor;
    editor.theme_ = theme;
    editor.look_ = look;
    editor.title_ = title.empty() ? L"便签" : title;
    editor.onClosed_ = std::move(onClosed);
    editor.onLanded_ = std::move(onLanded);
    editor.onAccent_ = std::move(onAccent);
    if (!editor.Create(graphics, instance, L"SnowAirRackNoteEditor", L"便签")) {
        RackLog(L"NoteEditor: Create failed");
        return false;
    }
    if (!editor.Open(owner, anchor, initial, static_cast<float>(dpi) / 96.0f)) {
        RackLog(L"NoteEditor: Open failed");
        editor.Destroy();
        return false;
    }
    /* 第一帧已经盖住便签卡片了，这时再让晾衣架把卡片藏掉，中间不会露空。 */
    if (onShown) {
        onShown();
    }

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, FALSE);
    }
    MSG message{};
    while (!editor.done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    editor.DestroyEdit();
    editor.Destroy();
    if (editor.accepted_) {
        text = editor.result_;
    }
    return editor.accepted_;
}

bool NoteEditor::Open(HWND owner, const RECT& anchor, const std::wstring& initial, float scale)
{
    if (!hwnd()) {
        return false;
    }
    owner_ = owner;
    initial_ = initial;
    cardAnchor_ = anchor;
    scale_ = std::max(0.8f, scale);
    contentScale_ = 1.0f;
    morphT_ = 1.0f;
    EnsureFormats();

    const int windowW = Px((kCardW + kShadow * 2.0f) * scale_);
    const int windowH = Px((kCardH + kShadow * 2.0f) * scale_);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor =
        MonitorFromWindow(owner ? owner : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    if (monitor) {
        GetMonitorInfoW(monitor, &info);
    }
    workArea_ = info.rcWork;
    if (workArea_.right <= workArea_.left || workArea_.bottom <= workArea_.top) {
        workArea_ = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    RECT target{};
    if (anchor.right > anchor.left && anchor.bottom > anchor.top) {
        const int centerX = (anchor.left + anchor.right) / 2;
        target.left = centerX - windowW / 2;
        /* 整体往上一点，离被编辑的卡片更近。 */
        target.top = anchor.bottom - Px(2.0f * scale_) - Px(kShadow * scale_);
    } else {
        target.left = workArea_.left + (workArea_.right - workArea_.left - windowW) / 2;
        target.top = workArea_.top + (workArea_.bottom - workArea_.top - windowH) / 2;
    }
    target.right = target.left + windowW;
    target.bottom = target.top + windowH;
    const int minX = workArea_.left + 8;
    const int minY = workArea_.top + 8;
    const int maxX = std::max(minX, static_cast<int>(workArea_.right) - windowW - 8);
    const int maxY = std::max(minY, static_cast<int>(workArea_.bottom) - windowH - 8);
    const int left = rack::ClampInt(static_cast<int>(target.left), minX, maxX);
    const int top = rack::ClampInt(static_cast<int>(target.top), minY, maxY);
    target.left = left;
    target.top = top;
    target.right = left + windowW;
    target.bottom = top + windowH;

    if (owner && IsWindow(owner)) {
        SetWindowLongPtrW(hwnd(), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    }
    /* 挂在宿主上的窗口一律是 NOACTIVATE；编辑器需要拿到键盘焦点。 */
    SetWindowLongPtrW(hwnd(), GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd(), GWL_EXSTYLE) &
                          ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));

    CreateEdit();
    if (edit_) {
        ShowWindow(edit_, SW_HIDE);
    }

    /* 动画起点就是被编辑的那张便签卡片：原地长大，纸色慢慢褪成编辑卡片。 */
    RECT start = target;
    if (anchor.right > anchor.left && anchor.bottom > anchor.top) {
        start = anchor;
    }
    morphFrom_ = start;
    morphTo_ = target;
    morphing_ = true;
    closing_ = false;
    const float startScale =
        static_cast<float>(start.right - start.left) / ((kCardW + kShadow * 2.0f) * scale_);
    contentScale_ = rack::Clamp(startScale, 0.2f, 1.0f);
    morphT_ = 0.0f;
    EnsureFormats();
    MoveTo(start.left, start.top, start.right - start.left, start.bottom - start.top);
    Render();
    ShowWindow(hwnd(), SW_SHOW);
    ActivateForInput();
    StartMorph(start, target, false, kMorphOpenMs);
    return true;
}

void NoteEditor::StartMorph(const RECT& from, const RECT& to, bool closing, int milliseconds)
{
    if (!hwnd()) {
        return;
    }
    morphFrom_ = from;
    morphTo_ = to;
    closing_ = closing;
    morphing_ = true;
    morphMs_ = std::max(1, milliseconds);
    morphStart_ = rack::NowMs();
    SetTimer(hwnd(), kTimerMorph, 15, nullptr);
    TickMorph();
}

void NoteEditor::TickMorph()
{
    if (!hwnd()) {
        return;
    }
    const uint64_t now = rack::NowMs();

    /* 第三段：已经落在夹子上，和下面那张卡片交叉淡出，交接看不出缝。 */
    if (alphaFading_) {
        float a = static_cast<float>(now - alphaStart_) / static_cast<float>(kAlphaMs);
        a = rack::Clamp(a, 0.0f, 1.0f);
        SetOpacity(1.0f - a);
        Render();
        if (a >= 1.0f) {
            SetOpacity(1.0f);
            CompleteFinish();
        }
        return;
    }
    if (!morphing_) {
        return;
    }

    float t = static_cast<float>(now - morphStart_) / static_cast<float>(morphMs_);
    t = rack::Clamp(t, 0.0f, 1.0f);
    const float eased = rack::EaseOutCubic(t);

    RECT rect{};
    rect.left = LerpInt(morphFrom_.left, morphTo_.left, eased);
    rect.top = LerpInt(morphFrom_.top, morphTo_.top, eased);
    rect.right = LerpInt(morphFrom_.right, morphTo_.right, eased);
    rect.bottom = LerpInt(morphFrom_.bottom, morphTo_.bottom, eased);
    const int w = std::max(1, static_cast<int>(rect.right - rect.left));
    const int h = std::max(1, static_cast<int>(rect.bottom - rect.top));

    contentScale_ = static_cast<float>(w) / ((kCardW + kShadow * 2.0f) * scale_);
    contentScale_ = rack::Clamp(contentScale_, 0.15f, 1.0f);
    /* 打开：前段把便签纸色淡成编辑卡片；缩回：反过来，从编辑卡片淡回便签。
       （之前缩回也用同一个方向，动画第一帧就整块跳成黄色，造成持续闪烁。） */
    const float rise = rack::Clamp((eased - 0.35f) / 0.45f, 0.0f, 1.0f);
    morphT_ = closing_ ? 1.0f - rise : rise;
    if (noteOnly_) {
        morphT_ = 0.0f; /* 已经变回便签，返回段保持便签样式 */
    }
    EnsureFormats();
    ApplyEditFont();
    /* 移动/缩放和换画面一次完成：分两步（先 SetWindowPos 再 Render）会被系统
       拉伸上一帧的画面，动画看起来一直闪。 */
    RenderAt(rect.left, rect.top, w, h);

    if (t >= 1.0f) {
        morphing_ = false;
        KillTimer(hwnd(), kTimerMorph);
        if (closing_) {
            if (!flying_ && cardAnchor_.right > cardAnchor_.left &&
                cardAnchor_.bottom > cardAnchor_.top) {
                /* 第二段：变成便签之后，再返回绳子的夹子。 */
                flying_ = true;
                noteOnly_ = true;
                RECT current{};
                GetWindowRect(hwnd(), &current);
                StartMorph(current, cardAnchor_, true, kFlyMs);
                return;
            }
            /* 第三段：让下面那张卡片先画出来，再淡出自己。 */
            if (onLanded_) {
                onLanded_();
            }
            alphaFading_ = true;
            alphaStart_ = rack::NowMs();
            SetTimer(hwnd(), kTimerMorph, 15, nullptr);
            return;
        }
        morphT_ = 1.0f;
        contentScale_ = 1.0f;
        EnsureFormats();
        ApplyEditFont();
        RenderAt(morphTo_.left, morphTo_.top, morphTo_.right - morphTo_.left,
                 morphTo_.bottom - morphTo_.top);
        LayoutEdit();
        if (edit_) {
            ShowWindow(edit_, SW_SHOWNOACTIVATE);
            if (!look_.readOnly) SetFocus(edit_);
        }
    }
}

void NoteEditor::CompleteFinish()
{
    if (done_) {
        return;
    }
    accepted_ = pendingAccepted_;
    if (onClosed_) {
        RECT rect{};
        GetWindowRect(hwnd(), &rect);
        onClosed_(rect);
    }
    DestroyEdit();
    Hide();
    done_ = true;
    /* 模态循环正等在 GetMessage 上，用一条空消息把它唤醒。 */
    PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
}

void NoteEditor::CreateEdit()
{
    if (edit_ || !hwnd() || !instance_) {
        return;
    }
    edit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", initial_.c_str(),
                            WS_POPUP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                ES_NOHIDESEL | ES_WANTRETURN,
                            0, 0, 10, 10, hwnd(), nullptr, instance_, nullptr);
    if (!edit_) {
        RackLog(L"NoteEditor: CreateWindowExW(EDIT) failed %lu", GetLastError());
        return;
    }
    SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    editOldProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NoteEditor::EditThunk)));

    ApplyEditFont();
    const int margin = Px(8.0f * scale_);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(margin, margin));
    if (look_.readOnly) {
        SendMessageW(edit_, EM_SETREADONLY, TRUE, 0);
        /* 只读时禁用它：文字变灰，也不会再出现闪烁光标。 */
        EnableWindow(edit_, FALSE);
    }

    /* 光标落到末尾：编辑已有便签时不该一上来就全选覆盖。 */
    const int length = GetWindowTextLengthW(edit_);
    SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
}

void NoteEditor::ApplyEditFont()
{
    if (!edit_) {
        return;
    }
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    const int size = Px(13.0f * scale_);
    editFont_ = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (editFont_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    }
}

void NoteEditor::DestroyEdit()
{
    if (edit_) {
        SetWindowLongPtrW(edit_, GWLP_USERDATA, 0);
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    editBrushColor_ = 0;
    lastEditW_ = 0;
    lastEditH_ = 0;
}

void NoteEditor::LayoutEdit()
{
    if (!edit_ || !hwnd()) return;
    const Layout layout = Measure();
    const float s = scale_ * contentScale_;
    const float inset = 1.0f * s;
    const int x = Px(layout.editArea.left + inset);
    const int y = Px(layout.editArea.top + inset);
    const int w = std::max(24, Px((layout.editArea.right - layout.editArea.left) - inset * 2.0f));
    const int h = std::max(24, Px((layout.editArea.bottom - layout.editArea.top) - inset * 2.0f));

    POINT origin{x, y};
    ClientToScreen(hwnd(), &origin);
    const bool resized = (w != lastEditW_ || h != lastEditH_);
    SetWindowPos(edit_, HWND_TOPMOST, origin.x, origin.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (resized) {
        const int round = Px(kFieldRadius * s);
        SetWindowRgn(edit_, CreateRoundRectRgn(0, 0, w + 1, h + 1, round, round), TRUE);
        lastEditW_ = w;
        lastEditH_ = h;
    }
    /* 让 EDIT 里的文字和拖动时卡片自绘的文字落在同一个位置：
       输入框比输入区缩进 1px（留给描边），再用 EM_SETRECT 补 7px 内边距，
       合计 8px，与自绘时用的 pad 一致。 */
    const int pad = std::max(0, Px(8.0f * s) - Px(1.0f * s));
    RECT format{pad, pad, std::max(pad + 16, w - pad), std::max(pad + 16, h - pad)};
    SendMessageW(edit_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&format));
}

IDWriteTextFormat* NoteEditor::MorphFormat(float size)
{
    /* 收回动画里字号是逐帧插值的；按 0.05px 量化缓存，避免每帧重建。 */
    if (!graphics_ || !graphics_->dwrite()) {
        return nullptr;
    }
    if (morphFormat_ && std::abs(morphFormatSize_ - size) < 0.05f) {
        return morphFormat_.Get();
    }
    morphFormat_.Reset();
    HRESULT hr = graphics_->dwrite()->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", morphFormat_.GetAddressOf());
    if (FAILED(hr)) {
        hr = graphics_->dwrite()->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", morphFormat_.GetAddressOf());
    }
    if (FAILED(hr) || !morphFormat_) {
        morphFormatSize_ = -1.0f;
        return nullptr;
    }
    morphFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    morphFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    morphFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    morphFormatSize_ = size;
    return morphFormat_.Get();
}

void NoteEditor::EnsureFormats()
{
    if (!graphics_ || !graphics_->dwrite()) {
        return;
    }
    const float s = scale_ * contentScale_;
    if (titleFormat_ && hintFormat_ && buttonFormat_ && cardTitleFormat_ &&
        cardMetaFormat_ && std::abs(formatScale_ - s) < 0.001f) {
        return;
    }
    formatScale_ = s;

    auto makeFormat = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_WORD_WRAPPING wrap,
                          DWRITE_TEXT_ALIGNMENT alignment) -> ComPtr<IDWriteTextFormat> {
        ComPtr<IDWriteTextFormat> format;
        HRESULT hr = graphics_->dwrite()->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        if (FAILED(hr)) {
            hr = graphics_->dwrite()->CreateTextFormat(
                L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        }
        if (SUCCEEDED(hr) && format) {
            format->SetWordWrapping(wrap);
            format->SetTextAlignment(alignment);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return format;
    };

    titleFormat_ = makeFormat(15.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_TEXT_ALIGNMENT_LEADING);
    hintFormat_ = makeFormat(11.0f * s, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_TEXT_ALIGNMENT_LEADING);
    buttonFormat_ = makeFormat(12.5f * s, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_TEXT_ALIGNMENT_CENTER);
    const float cardScale = std::max(0.5f, look_.cardScale);
    float titleSize = 12.0f;
    float metaSize = 10.0f;
    if (cardScale <= 0.9f) {
        titleSize = 10.0f;
        metaSize = 8.0f;
    } else if (cardScale >= 1.2f) {
        titleSize = 14.0f;
        metaSize = 12.0f;
    }
    cardTitleFormat_ = makeFormat(titleSize * s, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                  DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_TEXT_ALIGNMENT_LEADING);
    cardMetaFormat_ = makeFormat(metaSize * s, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_TEXT_ALIGNMENT_LEADING);
}

NoteEditor::Layout NoteEditor::Measure() const
{
    const float s = scale_ * contentScale_;
    Layout layout;
    const float shadow = kShadow * s;
    layout.card = D2D1::RectF(shadow, shadow, shadow + kCardW * s, shadow + kCardH * s);

    const float pad = kPad * s;
    const float close = kCloseSize * s;
    layout.close = D2D1::RectF(layout.card.right - pad - close, layout.card.top + pad - 3.0f * s,
                               layout.card.right - pad, layout.card.top + pad - 3.0f * s + close);
    layout.title = D2D1::RectF(layout.card.left + pad, layout.card.top + pad - 2.0f * s,
                               layout.close.left - 10.0f * s,
                               layout.card.top + pad - 2.0f * s + kHeaderH * s);

    const float footerTop = layout.card.bottom - pad - kButtonH * s;
    layout.field = D2D1::RectF(layout.card.left + pad,
                               layout.card.top + pad + kHeaderH * s + 8.0f * s,
                               layout.card.right - pad, footerTop - 16.0f * s);
    layout.editArea = layout.field;
    if (look_.colorMode) {
        const float rowH = 28.0f * s;
        layout.editArea.bottom = layout.field.bottom - rowH - 2.0f * s;
        const float size = 17.0f * s;
        const float gap = 7.0f * s;
        float sx = layout.field.left + 11.0f * s;
        const float sy = layout.field.bottom - 9.0f * s - size;
        for (int i = 0; i < 9; ++i) {
            layout.swatches[i] = D2D1::RectF(sx, sy, sx + size, sy + size);
            sx += size + gap;
        }
    }
    layout.save = D2D1::RectF(layout.card.right - pad - kButtonW * s, footerTop,
                              layout.card.right - pad, footerTop + kButtonH * s);
    layout.cancel = D2D1::RectF(layout.save.left - 8.0f * s - kButtonW * s, footerTop,
                                layout.save.left - 8.0f * s, footerTop + kButtonH * s);
    layout.hint = D2D1::RectF(layout.card.left + pad, footerTop, layout.cancel.left - 12.0f * s,
                              footerTop + kButtonH * s);
    return layout;
}

D2D1_COLOR_F NoteEditor::FieldColor() const
{
    /* 输入框和信封弹窗卡片同色：深色 #1A1A1A，浅色 #F5F4F4。 */
    return theme_.dark
               ? D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, 1.0f)
               : D2D1::ColorF(245.0f / 255.0f, 244.0f / 255.0f, 244.0f / 255.0f, 1.0f);
}

NoteEditor::Hot NoteEditor::HitTest(float x, float y) const
{
    const Layout layout = Measure();
    if (rack::PointIn(layout.close, x, y)) {
        return Hot::Close;
    }
    if (rack::PointIn(layout.save, x, y)) {
        return Hot::Save;
    }
    if (rack::PointIn(layout.cancel, x, y)) {
        return Hot::Cancel;
    }
    return Hot::None;
}

int NoteEditor::SwatchAt(float x, float y) const
{
    if (!look_.colorMode || morphT_ < 0.45f) {
        return -1;
    }
    const Layout layout = Measure();
    for (int i = 0; i < 9; ++i) {
        D2D1_RECT_F r = layout.swatches[i];
        const float grow = 3.0f * scale_;
        r = D2D1::RectF(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow);
        if (rack::PointIn(r, x, y)) {
            return i;
        }
    }
    return -1;
}

uint32_t NoteEditor::EditColorValue() const
{
    const std::wstring text = EditText();
    std::vector<card::Rgba> colors;
    if (card::ParseColors(text, colors) && !colors.empty()) {
        const card::Rgba& c = colors.front();
        return (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) |
               static_cast<uint32_t>(c.b);
    }
    return look_.accent & 0xFFFFFFu;
}

void NoteEditor::SetEditColor(uint32_t rgb)
{
    if (!edit_) return;
    const std::wstring text = card::HexText(card::Rgba{
        static_cast<uint8_t>((rgb >> 16) & 0xFF),
        static_cast<uint8_t>((rgb >> 8) & 0xFF),
        static_cast<uint8_t>(rgb & 0xFF), 255});
    SetWindowTextW(edit_, text.c_str());
    SendMessageW(edit_, EM_SETSEL, 0, -1);
    Render();
}

std::wstring NoteEditor::EditText() const
{
    if (!edit_) {
        return result_;
    }
    const int length = GetWindowTextLengthW(edit_);
    if (length <= 0) {
        return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void NoteEditor::Finish(bool accepted)
{
    if (done_ || closing_) {
        return;
    }
    if (morphing_) {
        return; /* 开场动画没完，先让用户看清 */
    }
    pendingAccepted_ = accepted;
    if (accepted) {
        result_ = EditText();
    }
    if (edit_) {
        ShowWindow(edit_, SW_HIDE);
    }
    /* 收回分三段：先原地把编辑卡片淡回便签的外观（位置不动），
       再缩小返回夹子（见 TickMorph 的动画段），最后与卡片交叉淡出。 */
    RECT current{};
    GetWindowRect(hwnd(), &current);
    StartMorph(current, current, true, kMorphCloseMs);
}

void NoteEditor::ActivateForInput()
{
    if (!hwnd()) {
        return;
    }
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    const bool attach = foregroundThread && foregroundThread != currentThread;
    if (attach) {
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    }
    BringWindowToTop(hwnd());
    SetForegroundWindow(hwnd());
    SetActiveWindow(hwnd());
    SetFocus(hwnd());
    if (attach) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
}

void NoteEditor::AfterDraw(HDC dc, void* bits, int width, int height)
{
    /* 拖动时 EDIT 被藏起来了，正文改由这里用 GDI 画：
       同一个字体句柄、同一个起点（从 EM_POSFROMCHAR 量出来），
       所以和输入框里的文字逐像素一致，拖动前后不会位移。 */
    if (!dragging_ || dragText_.empty() || !editFont_ || !dc) {
        return;
    }
    const Layout layout = Measure();
    const float s = scale_ * contentScale_;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(Opaque(theme_.fg)));
    HGDIOBJ old = SelectObject(dc, editFont_);
    RECT rect{};
    rect.left = dragTextOrigin_.x;
    rect.top = dragTextOrigin_.y;
    rect.right = static_cast<LONG>(layout.editArea.right - 8.0f * s);
    rect.bottom = static_cast<LONG>(layout.editArea.bottom - 6.0f * s);
    if (rect.right > rect.left && rect.bottom > rect.top) {
        DrawTextW(dc, dragText_.c_str(), static_cast<int>(dragText_.size()), &rect,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
    }
    SelectObject(dc, old);

    /* GDI 写字不维护 alpha 通道：把这块的 alpha 补回不透明，
       否则分层窗口合成时这些像素会变透明（文字看着像消失了）。 */
    if (bits && width > 0 && height > 0) {
        const int left = std::max(0, static_cast<int>(rect.left) - 2);
        const int top = std::max(0, static_cast<int>(rect.top) - 2);
        const int right = std::min(width, static_cast<int>(rect.right) + 2);
        const int bottom = std::min(height, static_cast<int>(rect.bottom) + 2);
        auto* pixels = static_cast<BYTE*>(bits);
        for (int y = top; y < bottom; ++y) {
            BYTE* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
            for (int x = left; x < right; ++x) {
                row[static_cast<size_t>(x) * 4u + 3u] = 255;
            }
        }
    }
}

void NoteEditor::Paint(ID2D1RenderTarget* target)
{
    if (!target) {
        return;
    }
    const float s = scale_ * contentScale_;
    const float cardS = scale_ * std::max(0.5f, look_.cardScale);
    const Layout layout = Measure();
    const float radius = kRadius * s;
    const float t = morphT_;

    /* 弹窗背景用信封面板色（深色 #262626 / 浅色 #FFFFFF）；
       输入框单独用更深的卡片色（#1A1A1A / #F5F4F4）。 */
    const D2D1_COLOR_F cardFill =
        theme_.dark ? D2D1::ColorF(0.149f, 0.149f, 0.149f, 1.0f) : White();
    const D2D1_COLOR_F border = Opaque(theme_.dark ? D2D1::ColorF(0.290f, 0.290f, 0.290f, 1.0f)
                                                   : theme_.border);
    const D2D1_COLOR_F fg = Opaque(theme_.fg);
    const D2D1_COLOR_F muted = Opaque(theme_.fgTertiary);
    const D2D1_COLOR_F primary = Opaque(theme_.primary);
    const D2D1_COLOR_F primaryFg = Opaque(theme_.primaryFg);

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> ink;
    if (FAILED(target->CreateSolidColorBrush(Black(), fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(Black(), ink.GetAddressOf()))) {
        return;
    }

    /* 阴影先画，否则会盖在卡片底上把卡片压暗。 */
    if (t > 0.001f) {
        for (int i = 10; i >= 1; --i) {
            const float spread = static_cast<float>(i) * s;
            fill->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.022f * t));
            const D2D1_RECT_F halo =
                D2D1::RectF(layout.card.left - spread, layout.card.top - spread + 3.0f * s,
                            layout.card.right + spread, layout.card.bottom + spread + 3.0f * s);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(halo, radius + spread, radius + spread), fill.Get());
        }
    }

    /* 1) 卡片底：绳子上那张卡的外观 → 编辑卡片，同时从「铺满窗口」收进卡片矩形。
       全程不透明，避免动画中透出桌面。 */
    const bool imageNote = look_.type == card::Type::Image;
    const bool noteLook = look_.type == card::Type::Note;
    const bool flatLook = !imageNote && !noteLook;
    D2D1_COLOR_F paperTop = NotePaperTop();
    D2D1_COLOR_F paperBottom = NotePaperBottom();
    D2D1_COLOR_F headerColor = NotePaperHeader();
    D2D1_COLOR_F cardTitleColor = NotePaperTitle();
    D2D1_COLOR_F cardBodyColor = NotePaperInk();
    D2D1_COLOR_F cardAccent = D2D1::ColorF(0.1f, 0.1f, 0.1f);
    if (flatLook) {
        cardAccent = RgbColor(look_.accentSet ? look_.accent : card::DefaultAccent(look_.type));
        headerColor = cardAccent;
        cardTitleColor = UsesDarkInk(cardAccent) ? D2D1::ColorF(0.318f, 0.318f, 0.318f) : White();
        cardBodyColor = D2D1::ColorF(0.318f, 0.318f, 0.318f);
        paperTop = White();
        paperBottom = White();
    } else if (noteLook && look_.accentSet) {
        cardAccent = RgbColor(look_.accent);
        headerColor = cardAccent;
        paperTop = Mix(cardAccent, White(), 0.70f);
        paperBottom = paperTop;
        cardTitleColor = White();
        cardBodyColor = NotePaperInk();
    }
    const NotePalette note{paperTop, cardBodyColor,
                           flatLook ? util::WithAlpha(headerColor, 0.20f) : NotePaperEdge(),
                           paperBottom};
    RECT client{};
    GetClientRect(hwnd(), &client);
    D2D1_RECT_F base = D2D1::RectF(
        rack::Lerp(0.0f, layout.card.left, t), rack::Lerp(0.0f, layout.card.top, t),
        rack::Lerp(static_cast<float>(client.right), layout.card.right, t),
        rack::Lerp(static_cast<float>(client.bottom), layout.card.bottom, t));
    const float cardRadius = flatLook ? 15.0f * cardS
                                      : (noteLook ? 0.0f : kNoteRadius * s);
    const float baseRadius = rack::Lerp(cardRadius, radius, t);
    {
        const D2D1_COLOR_F top =
            flatLook ? Mix(White(), cardFill, t) : Mix(paperTop, cardFill, t);
        const D2D1_COLOR_F bottom =
            flatLook ? Mix(White(), cardFill, t) : Mix(paperBottom, cardFill, t);
        const D2D1_GRADIENT_STOP stops[2] = {{0.0f, Opaque(top)}, {1.0f, Opaque(bottom)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> paper;
        if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2, collection.GetAddressOf())) &&
            SUCCEEDED(target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(base.left, base.top), D2D1::Point2F(base.right, base.bottom)),
                collection.Get(), paper.GetAddressOf()))) {
            target->FillRoundedRectangle(D2D1::RoundedRect(base, baseRadius, baseRadius),
                                         paper.Get());
        } else {
            fill->SetColor(Opaque(Mix(note.fill, cardFill, t)));
            target->FillRoundedRectangle(D2D1::RoundedRect(base, baseRadius, baseRadius),
                                         fill.Get());
        }
    }

    /* 文字卡的 20% 外圈；便签没有。头部会把顶部那段盖掉。 */
    if (flatLook && t < 0.999f) {
        const float ring = 4.5f * cardS;
        fill->SetColor(util::WithAlpha(Opaque(cardAccent), 0.20f * (1.0f - t)));
        const D2D1_RECT_F ringRect = rack::Inset(base, ring * 0.5f, ring * 0.5f);
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(ringRect, std::max(1.0f, baseRadius - ring * 0.5f),
                              std::max(1.0f, baseRadius - ring * 0.5f)),
            fill.Get(), ring);
    }

    /* 卡片头：标题/时间。t=0 时和绳子上的卡片一致，随后淡成编辑卡片。 */
    if (!imageNote) {
        const float headerHeight = 33.0f * cardS;
        const D2D1_RECT_F headerRect =
            D2D1::RectF(base.left, base.top, base.right, base.top + headerHeight);
        fill->SetColor(Opaque(Mix(headerColor, cardFill, t)));
        if (baseRadius > 0.5f) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(headerRect, baseRadius, baseRadius), fill.Get());
            target->FillRectangle(D2D1::RectF(headerRect.left, headerRect.top + baseRadius,
                                              headerRect.right, headerRect.bottom),
                                  fill.Get());
        } else {
            target->FillRectangle(headerRect, fill.Get());
        }
        if (t < 0.999f) {
            const std::wstring label = noteLook ? L"便签" : card::TypeLabel(look_.type);
            const std::wstring time = card::FormatCardTime(look_.createdAt);
            if (cardTitleFormat_) {
                ink->SetColor(util::WithAlpha(Opaque(Mix(cardTitleColor, fg, t)), 1.0f - t));
                target->DrawText(
                    label.c_str(), static_cast<UINT32>(label.size()), cardTitleFormat_.Get(),
                    D2D1::RectF(base.left + 11.0f * cardS, base.top + 4.0f * cardS,
                                base.right - 11.0f * cardS, base.top + 19.0f * cardS),
                    ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (!time.empty() && cardMetaFormat_) {
                ink->SetColor(
                    util::WithAlpha(Opaque(Mix(cardTitleColor, muted, t)), (1.0f - t) * 0.50f));
                target->DrawText(
                    time.c_str(), static_cast<UINT32>(time.size()), cardMetaFormat_.Get(),
                    D2D1::RectF(base.left + 11.0f * cardS, base.top + 19.0f * cardS,
                                base.right - 11.0f * cardS, base.top + 31.0f * cardS),
                    ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    /* 正文：从「输入区里的 13px」连续变形成卡片正文。 */
    const std::wstring& morphText = closing_ ? result_ : initial_;
    if (!morphText.empty() && (closing_ || t < 0.999f)) {
        const float textT = rack::Clamp(t / 0.55f, 0.0f, 1.0f);
        const float pad = 8.0f * s;
        const D2D1_RECT_F editorRect =
            D2D1::RectF(layout.field.left + pad, layout.field.top + pad,
                        layout.field.right - pad, layout.field.bottom - pad);
        const float cardTextTop = imageNote ? 18.0f * scale_ : 41.0f * cardS;
        const float cardTextLeft = imageNote ? 16.0f * scale_ : 11.0f * cardS;
        const D2D1_RECT_F noteRect =
            D2D1::RectF(base.left + cardTextLeft, base.top + cardTextTop,
                        base.right - (imageNote ? 12.0f * scale_ : 11.0f * cardS),
                        base.bottom - 10.0f * (imageNote ? scale_ : cardS));
        const D2D1_RECT_F textRect = D2D1::RectF(
            rack::Lerp(noteRect.left, editorRect.left, textT),
            rack::Lerp(noteRect.top, editorRect.top, textT),
            rack::Lerp(noteRect.right, editorRect.right, textT),
            rack::Lerp(noteRect.bottom, editorRect.bottom, textT));
        float cardBodySize = 11.5f;
        if (look_.cardScale <= 0.9f) {
            cardBodySize = 9.5f;
        } else if (look_.cardScale >= 1.2f) {
            cardBodySize = 13.5f;
        }
        const float cardTextSize = imageNote ? 10.0f * scale_ : cardBodySize * s;
        const float textSize = rack::Lerp(cardTextSize, 13.0f * s, textT);
        if (IDWriteTextFormat* format = MorphFormat(textSize)) {
            /* 收回时正文始终不透明（它就是同一段文字在变形）；
               打开时淡出，把位置让给随后的输入框。 */
            const float textAlpha = closing_ ? 1.0f : (1.0f - t);
            ink->SetColor(util::WithAlpha(Mix(note.ink, fg, textT), textAlpha));
            target->DrawText(morphText.c_str(), static_cast<UINT32>(morphText.size()), format,
                             textRect, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    /* 2) 编辑卡片：描边与内容随动画淡入 */
    if (t <= 0.001f) {
        return;
    }
    auto fade = [t](D2D1_COLOR_F color) { return util::WithAlpha(color, color.a * t); };

    if (titleFormat_) {
        ink->SetColor(fade(fg));
        target->DrawText(title_.c_str(), static_cast<UINT32>(title_.size()),
                         titleFormat_.Get(), layout.title, ink.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (hot_ == Hot::Close) {
        fill->SetColor(fade(Mix(cardFill, fg, 0.08f)));
        target->FillRoundedRectangle(
            D2D1::RoundedRect(rack::Inset(layout.close, 2.0f * s, 2.0f * s), 7.0f * s, 7.0f * s),
            fill.Get());
    }
    {
        const float cx = (layout.close.left + layout.close.right) * 0.5f;
        const float cy = (layout.close.top + layout.close.bottom) * 0.5f;
        const float arm = 4.6f * s;
        ink->SetColor(fade(hot_ == Hot::Close ? fg : muted));
        target->DrawLine(D2D1::Point2F(cx - arm, cy - arm), D2D1::Point2F(cx + arm, cy + arm),
                         ink.Get(), 1.3f * s);
        target->DrawLine(D2D1::Point2F(cx + arm, cy - arm), D2D1::Point2F(cx - arm, cy + arm),
                         ink.Get(), 1.3f * s);
    }

    /* 输入区：比弹窗背景更深一档的卡片色，不再描边。 */
    fill->SetColor(fade(FieldColor()));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(layout.field, kFieldRadius * s, kFieldRadius * s), fill.Get());

    if (look_.colorMode && t > 0.15f) {
        const uint32_t current = look_.accent & 0xFFFFFFu;
        bool presetSelected = false;
        for (uint32_t preset : kAccentPresets) {
            if ((preset & 0xFFFFFFu) == (current & 0xFFFFFFu)) presetSelected = true;
        }
        /* 选中绿圈不跟着色块一起缩小：色块 17，圈仍按原来的 22 左右。 */
        const float ringRadius = 11.0f * s;
        for (int i = 0; i < 9; ++i) {
            const D2D1_RECT_F r = layout.swatches[i];
            const float cx = (r.left + r.right) * 0.5f;
            const float cy = (r.top + r.bottom) * 0.5f;
            const float radius = (r.right - r.left) * 0.5f;
            if (i == 8) {
                /* 纯圆彩虹渐变：用足够细的扇形近似锥形渐变，最上面是蓝，
                   顺时针经过紫/红/黄/绿/青回到蓝。 */
                constexpr int kSegments = 180;
                const float step = 6.2831853f / static_cast<float>(kSegments);
                for (int k = 0; k < kSegments; ++k) {
                    const float a0 = -1.5707964f + static_cast<float>(k) * step;
                    const float a1 = a0 + step * 1.15f;
                    ComPtr<ID2D1PathGeometry> wedge;
                    if (graphics_ && graphics_->d2d() &&
                        SUCCEEDED(graphics_->d2d()->CreatePathGeometry(
                            wedge.GetAddressOf())) && wedge) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(wedge->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(D2D1::Point2F(cx, cy),
                                              D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(cx + std::cos(a0) * radius,
                                                        cy + std::sin(a0) * radius));
                            sink->AddLine(D2D1::Point2F(cx + std::cos(a1) * radius,
                                                        cy + std::sin(a1) * radius));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            const float hue =
                                240.0f + (static_cast<float>(k) + 0.5f) /
                                             static_cast<float>(kSegments) * 360.0f;
                            fill->SetColor(fade(HsvColor(hue, 1.0f, 1.0f)));
                            target->FillGeometry(wedge.Get(), fill.Get());
                        }
                    }
                }
                if (!presetSelected) {
                    ink->SetColor(fade(D2D1::ColorF(0.2f, 0.78f, 0.35f, 1.0f)));
                    target->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), ringRadius, ringRadius),
                        ink.Get(), 2.0f * s);
                }
            } else {
                fill->SetColor(fade(RgbColor(kAccentPresets[i])));
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                                    fill.Get());
                if ((kAccentPresets[i] & 0xFFFFFFu) == current) {
                    ink->SetColor(fade(D2D1::ColorF(0.2f, 0.78f, 0.35f, 1.0f)));
                    target->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), ringRadius, ringRadius),
                        ink.Get(), 2.0f * s);
                }
            }
        }
    }

    /* 拖动中：正文由卡片用 GDI 补画（见 AfterDraw），和 EDIT 里的一模一样。 */

    if (hintFormat_) {
        static const wchar_t kHint[] = L"Enter 完成 · Esc 取消";
        ink->SetColor(fade(muted));
        target->DrawText(kHint, static_cast<UINT32>(wcslen(kHint)), hintFormat_.Get(),
                         layout.hint, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    {
        const bool hovered = hot_ == Hot::Cancel;
        fill->SetColor(fade(theme_.PopupCancelBg(hovered)));
        target->FillRoundedRectangle(
            D2D1::RoundedRect(layout.cancel, kButtonRadius * s, kButtonRadius * s), fill.Get());
        if (buttonFormat_) {
            static const wchar_t kLabel[] = L"取消";
            ink->SetColor(fade(fg));
            target->DrawText(kLabel, static_cast<UINT32>(wcslen(kLabel)), buttonFormat_.Get(),
                             layout.cancel, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    {
        D2D1_COLOR_F accent = primary;
        if (hot_ == Hot::Save) {
            accent = Mix(accent, White(), 0.09f);
        }
        if (pressed_ == Hot::Save) {
            accent = Mix(accent, Black(), 0.08f);
        }
        fill->SetColor(fade(accent));
        target->FillRoundedRectangle(
            D2D1::RoundedRect(layout.save, kButtonRadius * s, kButtonRadius * s), fill.Get());
        if (buttonFormat_) {
            static const wchar_t kLabel[] = L"完成";
            ink->SetColor(fade(primaryFg));
            target->DrawText(kLabel, static_cast<UINT32>(wcslen(kLabel)), buttonFormat_.Get(),
                             layout.save, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

LRESULT CALLBACK NoteEditor::EditThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<NoteEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == WM_KEYDOWN) {
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            self->Finish(true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            self->Finish(false);
            return 0;
        }
    }
    if (message == WM_SETCURSOR) {
        /* 输入区固定是工字光标，别让外面那层卡片的箭头抢掉。 */
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return TRUE;
    }
    if (self && self->editOldProc_) {
        return CallWindowProcW(self->editOldProc_, hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT NoteEditor::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_TIMER:
        if (wParam == kTimerMorph) {
            TickMorph();
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        if (!edit_ || reinterpret_cast<HWND>(lParam) != edit_) {
            break;
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        const COLORREF bg = ToColorRef(FieldColor());
        SetTextColor(dc, ToColorRef(Opaque(look_.readOnly ? theme_.fgTertiary
                                                          : theme_.fg)));
        SetBkColor(dc, bg);
        if (!editBrush_ || editBrushColor_ != bg) {
            if (editBrush_) {
                DeleteObject(editBrush_);
            }
            editBrush_ = CreateSolidBrush(bg);
            editBrushColor_ = bg;
        }
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd(), &point);
        const Hot hot = HitTest(static_cast<float>(point.x), static_cast<float>(point.y));
        if (SwatchAt(static_cast<float>(point.x), static_cast<float>(point.y)) >= 0 ||
            hot == Hot::Close || hot == Hot::Cancel || hot == Hot::Save) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (dragging_) {
            POINT point{static_cast<LONG>(GET_X_LPARAM(lParam)),
                        static_cast<LONG>(GET_Y_LPARAM(lParam))};
            ClientToScreen(hwnd(), &point);
            const int left = dragStart_.left + (point.x - dragOrigin_.x);
            const int top = dragStart_.top + (point.y - dragOrigin_.y);
            const int w = width();
            const int h = height();
            const int minX = workArea_.left + 8;
            const int minY = workArea_.top + 8;
            const int maxX = std::max(minX, static_cast<int>(workArea_.right) - w - 8);
            const int maxY = std::max(minY, static_cast<int>(workArea_.bottom) - h - 8);
            MovePresent(rack::ClampInt(left, minX, maxX), rack::ClampInt(top, minY, maxY));
            return 0;
        }
        if (morphing_) {
            return 0;
        }
        if (look_.colorMode && SwatchAt(x, y) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return 0;
        }
        const Hot hot = HitTest(x, y);
        if (hot != hot_) {
            hot_ = hot;
            if (pressed_ != Hot::None && pressed_ != hot) {
                pressed_ = Hot::None;
            }
            Render();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (morphing_) {
            return 0;
        }
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (look_.colorMode) {
            const int swatch = SwatchAt(x, y);
            if (swatch >= 0) {
                pressedSwatch_ = swatch;
                return 0;
            }
        }
        const Hot hot = HitTest(x, y);
        if (hot == Hot::Close || hot == Hot::Cancel || hot == Hot::Save) {
            pressed_ = hot;
            Render();
            return 0;
        }
        if (edit_ && !look_.readOnly) {
            SetFocus(edit_);
        }
        /* 卡片空白处（标题/边距）按住就能拖，光标不变；输入区由 EDIT 接管。
           拖动期间先把 EDIT 藏起来、正文改由卡片用 GDI 补画（同一个字体和
           起点），这样既不会一格格重绘闪烁，文字也不会位移。 */
        dragText_ = EditText();
        if (edit_) {
            RECT editRect{};
            GetWindowRect(edit_, &editRect);
            POINT origin{editRect.left, editRect.top};
            ScreenToClient(hwnd(), &origin);
            const LRESULT textPos = SendMessageW(edit_, EM_POSFROMCHAR, 0, 0);
            if (textPos != -1) {
                dragTextOrigin_.x = origin.x + GET_X_LPARAM(textPos);
                dragTextOrigin_.y = origin.y + GET_Y_LPARAM(textPos);
            } else {
                dragTextOrigin_.x = origin.x + Px(8.0f * scale_);
                dragTextOrigin_.y = origin.y + Px(8.0f * scale_);
            }
            ShowWindow(edit_, SW_HIDE);
        }
        POINT point{static_cast<LONG>(GET_X_LPARAM(lParam)),
                    static_cast<LONG>(GET_Y_LPARAM(lParam))};
        ClientToScreen(hwnd(), &point);
        dragOrigin_ = point;
        GetWindowRect(hwnd(), &dragStart_);
        dragging_ = true;
        SetCapture(hwnd());
        Render();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (dragging_) {
            dragging_ = false;
            if (GetCapture() == hwnd()) {
                ReleaseCapture();
            }
            dragText_.clear();
            LayoutEdit();
            if (edit_) {
                ShowWindow(edit_, SW_SHOWNOACTIVATE);
                if (!look_.readOnly) SetFocus(edit_);
            }
            Render();
            return 0;
        }
        if (morphing_) {
            return 0;
        }
        if (pressedSwatch_ >= 0) {
            const int swatch = SwatchAt(static_cast<float>(GET_X_LPARAM(lParam)),
                                        static_cast<float>(GET_Y_LPARAM(lParam)));
            const int pressed = pressedSwatch_;
            pressedSwatch_ = -1;
            if (swatch == pressed) {
                auto applyAccent = [this](uint32_t rgb) {
                    look_.accent = rgb & 0xFFFFFFu;
                    look_.accentSet = true;
                    if (onAccent_) onAccent_(look_.accent);
                    Render();
                };
                if (pressed < 8) {
                    applyAccent(kAccentPresets[pressed]);
                } else {
                    COLORREF custom[16]{};
                    const uint32_t current = look_.accent & 0xFFFFFFu;
                    CHOOSECOLORW choose{};
                    choose.lStructSize = sizeof(choose);
                    choose.hwndOwner = hwnd();
                    choose.rgbResult = RGB((current >> 16) & 0xFF, (current >> 8) & 0xFF,
                                           current & 0xFF);
                    choose.lpCustColors = custom;
                    choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&choose)) {
                        applyAccent((static_cast<uint32_t>(GetRValue(choose.rgbResult)) << 16) |
                                    (static_cast<uint32_t>(GetGValue(choose.rgbResult)) << 8) |
                                    static_cast<uint32_t>(GetBValue(choose.rgbResult)));
                    }
                }
                if (edit_ && !look_.readOnly) SetFocus(edit_);
            }
            Render();
            return 0;
        }
        const Hot hot = HitTest(static_cast<float>(GET_X_LPARAM(lParam)),
                                static_cast<float>(GET_Y_LPARAM(lParam)));
        const Hot pressed = pressed_;
        pressed_ = Hot::None;
        if (pressed != Hot::None && pressed == hot) {
            if (pressed == Hot::Save) {
                Finish(true);
                return 0;
            }
            if (pressed == Hot::Cancel || pressed == Hot::Close) {
                Finish(false);
                return 0;
            }
        }
        Render();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) {
            dragging_ = false;
            dragText_.clear();
            LayoutEdit();
            if (edit_) {
                ShowWindow(edit_, SW_SHOWNOACTIVATE);
                if (!look_.readOnly) SetFocus(edit_);
            }
            Render();
        }
        return 0;
    case WM_MOVE:
        if (!morphing_) {
            LayoutEdit();
        }
        return 0;
    case WM_CLOSE:
        Finish(false);
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
        EnsureFormats();
        ApplyEditFont();
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        const int w = Px((kCardW + kShadow * 2.0f) * scale_);
        const int h = Px((kCardH + kShadow * 2.0f) * scale_);
        MoveTo(suggested ? suggested->left : 0, suggested ? suggested->top : 0, w, h);
        LayoutEdit();
        Render();
        return 0;
    }
    default:
        break;
    }
    return LayeredWindow::HandleMessage(message, wParam, lParam);
}
