#include "ContextMenu.h"
#include "Graphics.h"
#include "Util.h"

namespace {
constexpr float kRowHeight = 28.0f;
constexpr float kSeparatorHeight = 11.0f;
constexpr float kMenuWidth = 168.0f;
constexpr float kPadding = 4.0f;
} /* namespace */

ContextMenu::ContextMenu() = default;

bool ContextMenu::Init(Graphics* graphics)
{
    return Create(graphics, util::GetDllModule(), L"SnowAirRackMenu", L"SnowAir Rack");
}

void ContextMenu::EnsureFormat(float scale)
{
    if (!graphics_ || !graphics_->dwrite()) {
        return;
    }
    if (textFormat_ && std::abs(formatScale_ - scale) < 0.001f) {
        return;
    }
    ComPtr<IDWriteTextFormat> format;
    HRESULT hr = graphics_->dwrite()->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale, L"zh-CN", format.GetAddressOf());
    if (FAILED(hr)) {
        hr = graphics_->dwrite()->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale, L"zh-CN", format.GetAddressOf());
    }
    if (format) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormat_ = format;
        formatScale_ = scale;
    }
}

int ContextMenu::MenuHeight() const
{
    float height = kPadding * 2.0f;
    for (const auto& item : items_) {
        height += item.kind == MenuItem::Kind::Separator ? kSeparatorHeight : kRowHeight;
    }
    return static_cast<int>(std::lround(height * scale_));
}

void ContextMenu::Show(const std::vector<MenuItem>& items, POINT screen, const RECT& workArea,
                       float scale, const Theme& theme, std::function<void(int)> onSelect,
                       std::function<void()> onClose)
{
    if (!hwnd() || items.empty()) {
        return;
    }
    items_ = items;
    theme_ = theme;
    onSelect_ = std::move(onSelect);
    onClose_ = std::move(onClose);
    scale_ = std::max(0.5f, scale);
    EnsureFormat(scale_);
    width_ = kMenuWidth * scale_;
    height_ = static_cast<float>(MenuHeight());

    int x = screen.x;
    int y = screen.y;
    if (x + static_cast<int>(width_) > workArea.right) {
        x = workArea.right - static_cast<int>(width_);
    }
    if (y + static_cast<int>(height_) > workArea.bottom) {
        y = workArea.bottom - static_cast<int>(height_);
    }
    x = std::max<int>(workArea.left, x);
    y = std::max<int>(workArea.top, y);

    hovered_ = -1;
    pressed_ = -1;
    closing_ = false;
    MoveTo(x, y, static_cast<int>(std::lround(width_)),
           static_cast<int>(std::lround(height_)));
    Render();
    ShowNoActivate();
    SetCapture(hwnd());
}

void ContextMenu::Close()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    if (GetCapture() == hwnd()) {
        ReleaseCapture();
    }
    Hide();
    auto onClose = std::move(onClose_);
    onClose_ = nullptr;
    onSelect_ = nullptr;
    items_.clear();
    hovered_ = -1;
    pressed_ = -1;
    if (onClose) {
        onClose();
    }
    closing_ = false;
}

D2D1_RECT_F ContextMenu::RowRect(int index) const
{
    float y = kPadding * scale_;
    for (int i = 0; i < index && i < static_cast<int>(items_.size()); ++i) {
        y += (items_[static_cast<size_t>(i)].kind == MenuItem::Kind::Separator
                  ? kSeparatorHeight
                  : kRowHeight) *
             scale_;
    }
    const float h = (items_[static_cast<size_t>(index)].kind == MenuItem::Kind::Separator
                         ? kSeparatorHeight
                         : kRowHeight) *
                    scale_;
    return D2D1::RectF(0, y, width_, y + h);
}

int ContextMenu::HitRow(int x, int y) const
{
    if (x < 0 || y < 0 || x > width_ || y > height_) {
        return -1;
    }
    float top = kPadding * scale_;
    for (size_t i = 0; i < items_.size(); ++i) {
        const float h = (items_[i].kind == MenuItem::Kind::Separator ? kSeparatorHeight
                                                                     : kRowHeight) *
                        scale_;
        if (y >= top && y < top + h) {
            return items_[i].kind == MenuItem::Kind::Separator ? -1
                                                               : static_cast<int>(i);
        }
        top += h;
    }
    return -1;
}

void ContextMenu::Paint(ID2D1RenderTarget* target)
{
    if (!target || items_.empty()) {
        return;
    }
    const float s = scale_;
    const float radius = 8.0f * s;
    const D2D1_RECT_F panel = D2D1::RectF(0.5f * s, 0.5f * s, width_ - 0.5f * s,
                                          height_ - 0.5f * s);

    ComPtr<ID2D1SolidColorBrush> shadow;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f),
                                                shadow.GetAddressOf()))) {
        for (int i = 7; i >= 1; --i) {
            shadow->SetColor(D2D1::ColorF(0.f, 0.f, 0.f, 0.025f));
            D2D1_RECT_F rect = D2D1::RectF(panel.left - i * s, panel.top - i * s + 3 * s,
                                           panel.right + i * s, panel.bottom + i * s + 3 * s);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(rect, radius + i * s, radius + i * s), shadow.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> bg;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> hover;
    ComPtr<ID2D1SolidColorBrush> fg;
    ComPtr<ID2D1SolidColorBrush> disabled;
    ComPtr<ID2D1SolidColorBrush> accent;
    if (FAILED(target->CreateSolidColorBrush(theme_.bg, bg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_.border, border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_.accent, hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_.fg, fg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_.fgTertiary, disabled.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_.accent, accent.GetAddressOf()))) {
        return;
    }

    target->FillRoundedRectangle(D2D1::RoundedRect(panel, radius, radius), bg.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(panel, radius, radius), border.Get(),
                                 1.0f * s);

    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& item = items_[i];
        const D2D1_RECT_F row = RowRect(static_cast<int>(i));
        if (item.kind == MenuItem::Kind::Separator) {
            const float mid = (row.top + row.bottom) * 0.5f;
            ComPtr<ID2D1SolidColorBrush> sep;
            const D2D1_COLOR_F sepColor =
                theme_.dark ? D2D1::ColorF(1.f, 1.f, 1.f, 0.08f)
                            : D2D1::ColorF(0.f, 0.f, 0.f, 0.05f);
            if (SUCCEEDED(target->CreateSolidColorBrush(sepColor, sep.GetAddressOf()))) {
                target->DrawLine(D2D1::Point2F(kPadding * s * 2.0f, mid),
                                 D2D1::Point2F(width_ - kPadding * s * 2.0f, mid),
                                 sep.Get(), 1.0f * s);
            }
            continue;
        }

        if (static_cast<int>(i) == hovered_ || static_cast<int>(i) == pressed_) {
            D2D1_RECT_F highlight = rack::Inset(row, 4.0f * s, 1.0f * s);
            target->FillRoundedRectangle(D2D1::RoundedRect(highlight, 4.0f * s, 4.0f * s),
                                         hover.Get());
        }

        float textLeft = 12.0f * s;
        if (item.kind == MenuItem::Kind::Color) {
            const float cx = 20.0f * s;
            const float cy = (row.top + row.bottom) * 0.5f;
            ComPtr<ID2D1SolidColorBrush> swatch;
            if (SUCCEEDED(target->CreateSolidColorBrush(item.swatch, swatch.GetAddressOf()))) {
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.0f * s, 6.0f * s),
                                    swatch.Get());
                target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.0f * s, 6.0f * s),
                                    border.Get(), 1.0f * s);
            }
            textLeft = 34.0f * s;
        }

        if (textFormat_) {
            D2D1_RECT_F textRect = D2D1::RectF(textLeft, row.top, width_ - 26.0f * s,
                                               row.bottom);
            target->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                             textFormat_.Get(), textRect,
                             item.enabled ? fg.Get() : disabled.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (item.checked) {
            const float cx = width_ - 16.0f * s;
            const float cy = (row.top + row.bottom) * 0.5f;
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.5f * s, 3.5f * s),
                                accent.Get());
        }
    }
}

LRESULT ContextMenu::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd(), &point);
        if (HitRow(point.x, point.y) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        const int row = HitRow(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (row != hovered_) {
            hovered_ = row;
            Render();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (x < 0 || y < 0 || x >= static_cast<int>(width_) ||
            y >= static_cast<int>(height_)) {
            Close();
            return 0;
        }
        pressed_ = HitRow(x, y);
        Render();
        return 0;
    }
    case WM_LBUTTONUP: {
        const int row = HitRow(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        const int pressed = pressed_;
        pressed_ = -1;
        if (row >= 0 && row == pressed && row < static_cast<int>(items_.size()) &&
            items_[static_cast<size_t>(row)].enabled) {
            const int id = items_[static_cast<size_t>(row)].id;
            auto onSelect = std::move(onSelect_);
            Close();
            if (onSelect) {
                onSelect(id);
            }
            return 0;
        }
        if (row < 0 && (GET_X_LPARAM(lParam) < 0 || GET_Y_LPARAM(lParam) < 0 ||
                        GET_X_LPARAM(lParam) >= static_cast<int>(width_) ||
                        GET_Y_LPARAM(lParam) >= static_cast<int>(height_))) {
            Close();
            return 0;
        }
        Render();
        return 0;
    }
    case WM_RBUTTONUP:
        Close();
        return 0;
    case WM_CAPTURECHANGED:
        if (!closing_ && GetCapture() != hwnd()) {
            Close();
        }
        return 0;
    case WM_MOUSEWHEEL:
        return 0;
    case WM_DPICHANGED:
        return 0;
    default:
        break;
    }
    return LayeredWindow::HandleMessage(message, wParam, lParam);
}
