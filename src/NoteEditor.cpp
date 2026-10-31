#include "NoteEditor.h"
#include "NotePalette.h"
#include "Util.h"

#include "Graphics.h"

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

constexpr UINT_PTR kTimerMorph = 11;
constexpr int kMorphOpenMs = 240;
constexpr int kMorphCloseMs = 180;
constexpr int kFlyMs = 380;     /* 便签从编辑位置飞回夹子 */
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
                      std::function<void()> onLanded)
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
    editor.title_ = title.empty() ? L"便签" : title;
    editor.onClosed_ = std::move(onClosed);
    editor.onLanded_ = std::move(onLanded);
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
        target.top = anchor.bottom + Px(10.0f * scale_) - Px(kShadow * scale_);
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
       （之前缩回也用同一个方向，动画第一帧就整块跳成黄色，看着一直在闪。） */
    const float rise = rack::Clamp((eased - 0.35f) / 0.45f, 0.0f, 1.0f);
    morphT_ = closing_ ? 1.0f - rise : rise;
    if (noteOnly_) {
        morphT_ = 0.0f; /* 已经变回便签，飞回去这一段保持便签的样子 */
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
                /* 第二段：变成便签之后，再飞回绳子的夹子。 */
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
            SetFocus(edit_);
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
    const int x = Px(layout.field.left + inset);
    const int y = Px(layout.field.top + inset);
    const int w = std::max(24, Px((layout.field.right - layout.field.left) - inset * 2.0f));
    const int h = std::max(24, Px((layout.field.bottom - layout.field.top) - inset * 2.0f));

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
    if (titleFormat_ && hintFormat_ && buttonFormat_ &&
        std::abs(formatScale_ - s) < 0.001f) {
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
    /* 跟「新建便签」弹窗一致：弹窗卡片色 / 深色下 #262626、浅色下 #FFFFFF。 */
    return theme_.dark ? D2D1::ColorF(0.149f, 0.149f, 0.149f, 1.0f) : White();
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
       再缩小飞回夹子（见 TickMorph 的飞行段），最后与卡片交叉淡出。 */
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
    rect.right = static_cast<LONG>(layout.field.right - 8.0f * s);
    rect.bottom = static_cast<LONG>(layout.field.bottom - 6.0f * s);
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
    const Layout layout = Measure();
    const float radius = kRadius * s;
    const float t = morphT_;

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

    /* 1) 卡片底：便签纸色 → 编辑卡片色，同时从「铺满窗口」收进卡片矩形。
       全程不透明，避免动画中透出桌面。 */
    /* 与晾衣架上的便签卡片同色：黄色纸张 + 深棕字。 */
    const NotePalette note{NotePaperTop(), NotePaperInk(), NotePaperEdge(), NotePaperBottom()};
    RECT client{};
    GetClientRect(hwnd(), &client);
    D2D1_RECT_F base = D2D1::RectF(
        rack::Lerp(0.0f, layout.card.left, t), rack::Lerp(0.0f, layout.card.top, t),
        rack::Lerp(static_cast<float>(client.right), layout.card.right, t),
        rack::Lerp(static_cast<float>(client.bottom), layout.card.bottom, t));
    const float baseRadius = rack::Lerp(kNoteRadius * s, radius, t);
    /* 便签纸和绳子上那张卡片一样是斜向渐变（F4D476 → F7E3A7），
       落位交接时才不会突然从纯色变成渐变。 */
    {
        const D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, Opaque(Mix(D2D1::ColorF(0xF4D476), cardFill, t))},
            {1.0f, Opaque(Mix(D2D1::ColorF(0xF7E3A7), cardFill, t))},
        };
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

    /* 正文：全程只有一份，从「输入区里的 13px」连续变形成「便签上的 10px」——
       位置、字号、颜色一起插值，文字一直在，不会先消失再在别处长出来。
       终点用和 RackView::DrawNote 完全一样的内边距（16/18）和字号（10 * scale）。 */
    const std::wstring& morphText = closing_ ? result_ : initial_;
    if (!morphText.empty() && (closing_ || t < 0.999f)) {
        /* 正文在动画后半程才开始挪位置：前半程编辑界面还在淡出，
           文字先待在输入区里，不会横穿过正在消失的标题。 */
        const float textT = rack::Clamp(t / 0.55f, 0.0f, 1.0f);
        const float pad = 8.0f * s;
        const D2D1_RECT_F editorRect =
            D2D1::RectF(layout.field.left + pad, layout.field.top + pad,
                        layout.field.right - pad, layout.field.bottom - pad);
        const D2D1_RECT_F noteRect =
            D2D1::RectF(base.left + 16.0f * scale_, base.top + 18.0f * scale_,
                        base.right - 12.0f * scale_, base.bottom - 10.0f * scale_);
        const D2D1_RECT_F textRect = D2D1::RectF(
            rack::Lerp(noteRect.left, editorRect.left, textT),
            rack::Lerp(noteRect.top, editorRect.top, textT),
            rack::Lerp(noteRect.right, editorRect.right, textT),
            rack::Lerp(noteRect.bottom, editorRect.bottom, textT));
        const float textSize = rack::Lerp(10.0f * scale_, 13.0f * s, textT);
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

    ink->SetColor(fade(Mix(note.edge, border, t)));
    target->DrawRoundedRectangle(D2D1::RoundedRect(base, baseRadius, baseRadius), ink.Get(),
                                 1.0f * s);

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

    /* 输入区：与弹窗一致——同卡片底色 + 描边 */
    fill->SetColor(fade(cardFill));
    target->FillRoundedRectangle(
        D2D1::RoundedRect(layout.field, kFieldRadius * s, kFieldRadius * s), fill.Get());
    ink->SetColor(fade(border));
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(layout.field, kFieldRadius * s, kFieldRadius * s), ink.Get(), 1.0f * s);

    /* 拖动中：正文由卡片用 GDI 补画（见 AfterDraw），和 EDIT 里的一模一样。 */

    if (hintFormat_) {
        static const wchar_t kHint[] = L"Enter 完成 · Esc 取消";
        ink->SetColor(fade(muted));
        target->DrawText(kHint, static_cast<UINT32>(wcslen(kHint)), hintFormat_.Get(),
                         layout.hint, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    {
        const bool hovered = hot_ == Hot::Cancel;
        fill->SetColor(fade(hovered ? Mix(Opaque(theme_.card), fg, 0.06f) : Opaque(theme_.card)));
        target->FillRoundedRectangle(
            D2D1::RoundedRect(layout.cancel, kButtonRadius * s, kButtonRadius * s), fill.Get());
        ink->SetColor(fade(border));
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(layout.cancel, kButtonRadius * s, kButtonRadius * s), ink.Get(),
            1.0f * s);
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
    case WM_CTLCOLOREDIT: {
        if (!edit_ || reinterpret_cast<HWND>(lParam) != edit_) {
            break;
        }
        HDC dc = reinterpret_cast<HDC>(wParam);
        const COLORREF bg = ToColorRef(FieldColor());
        SetTextColor(dc, ToColorRef(Opaque(theme_.fg)));
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
        if (hot == Hot::Close || hot == Hot::Cancel || hot == Hot::Save) {
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
        const Hot hot = HitTest(x, y);
        if (hot == Hot::Close || hot == Hot::Cancel || hot == Hot::Save) {
            pressed_ = hot;
            Render();
            return 0;
        }
        if (edit_) {
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
                SetFocus(edit_);
            }
            Render();
            return 0;
        }
        if (morphing_) {
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
                SetFocus(edit_);
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
