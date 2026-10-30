#include "PreviewWindow.h"
#include "Graphics.h"
#include "Util.h"

namespace {
constexpr float kShadowPad = 34.0f;
constexpr float kMaxImageWidth = 640.0f;
constexpr float kMaxImageHeight = 480.0f;
constexpr float kGap = 12.0f;
constexpr float kNotePadding = 12.0f;
constexpr float kMaxNoteHead = 520.0f;
constexpr float kMaxNoteText = 200.0f;

void DrawSoftShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                    float dy, float spread, float alpha)
{
    (void)radius;
    D2D1_GRADIENT_STOP stops[3]{};
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.95f);
    stops[1].position = 0.56f;
    stops[1].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.38f);
    stops[2].position = 1.0f;
    stops[2].color = D2D1::ColorF(0.f, 0.f, 0.f, 0.0f);

    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> brush;
    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = (rect.top + rect.bottom) * 0.5f + dy;
    const float radiusX = (rect.right - rect.left) * 0.5f + spread;
    const float radiusY = (rect.bottom - rect.top) * 0.5f + spread;
    if (FAILED(target->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2,
                                                    D2D1_EXTEND_MODE_CLAMP,
                                                    collection.GetAddressOf())) ||
        FAILED(target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(centerX, centerY),
                                                D2D1::Point2F(0.f, 0.f), radiusX, radiusY),
            collection.Get(), brush.GetAddressOf()))) {
        return;
    }
    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radiusX, radiusY), brush.Get());
}

} /* namespace */

PreviewWindow::PreviewWindow() = default;

bool PreviewWindow::Init(Graphics* graphics)
{
    if (!Create(graphics, util::GetDllModule(), L"SnowAirRackPreview", L"SnowAir Rack"))
        return false;
    HRESULT hr = graphics->dwrite()->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", noteFormat_.GetAddressOf());
    if (FAILED(hr)) {
        hr = graphics->dwrite()->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", noteFormat_.GetAddressOf());
    }
    if (noteFormat_) {
        noteFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        noteFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        noteFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    formatScale_ = 1.0f;
    return noteFormat_ != nullptr;
}

void PreviewWindow::EnsureFormat(float scale)
{
    if (!graphics_ || !graphics_->dwrite()) {
        return;
    }
    if (noteFormat_ && std::abs(formatScale_ - scale) < 0.001f) {
        return;
    }
    ComPtr<IDWriteTextFormat> format;
    HRESULT hr = graphics_->dwrite()->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"zh-CN", format.GetAddressOf());
    if (FAILED(hr)) {
        hr = graphics_->dwrite()->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"zh-CN", format.GetAddressOf());
    }
    if (format) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        noteFormat_ = format;
        formatScale_ = scale;
    }
}

float PreviewWindow::MeasureNoteHeight(float width, float scale) const
{
    if (!item_ || item_->note.empty() || !noteFormat_ || !graphics_) {
        return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = graphics_->dwrite()->CreateTextLayout(
        item_->note.c_str(), static_cast<UINT32>(item_->note.size()), noteFormat_.Get(), width,
        10000.0f, layout.GetAddressOf());
    if (FAILED(hr) || !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return std::min(metrics.height, kMaxNoteText * scale);
}

void PreviewWindow::ShowItem(const Item& item, const D2D1_RECT_F& cardScreen,
                             const RECT& workArea, float scale, const Theme& theme)
{
    item_ = &item;
    theme_ = theme;
    scale_ = scale;
    EnsureFormat(scale);

    if (!util::SamePath(loadedPath_, item.path)) {
        loadedPath_ = item.path;
        attempted_ = false;
        preview_.Reset();
        scrollY_ = 0;
    }

    const float s = scale;
    const float shadow = kShadowPad * s;
    const float workWidth = static_cast<float>(workArea.right - workArea.left);
    const float workHeight = static_cast<float>(workArea.bottom - workArea.top);

    const float aspect = item.aspect > 0.01f ? item.aspect : 1.4f;
    float imageWidth = std::min(kMaxImageWidth * s, workWidth - 2.0f * shadow - 24.0f * s);
    imageWidth = std::max(imageWidth, 160.0f * s);
    float imageHeight = imageWidth / aspect;
    if (imageHeight > kMaxImageHeight * s) {
        imageHeight = kMaxImageHeight * s;
        imageWidth = imageHeight * aspect;
    }

    hasNote_ = !item.note.empty();
    float noteWidth = 0.0f;
    float noteHeight = 0.0f;
    if (hasNote_) {
        noteWidth = std::min(imageWidth, kMaxNoteHead * s);
        noteWidth = std::max(noteWidth, 180.0f * s);
        const float textWidth = noteWidth - 2.0f * kNotePadding * s;
        const float textHeight = MeasureNoteHeight(textWidth, s);
        noteHeight = std::min(textHeight + 2.0f * kNotePadding * s, 224.0f * s);
    }

    const float availableHeight =
        std::max(220.0f * s, workHeight - 48.0f * s);
    float totalHeight = shadow + imageHeight + (hasNote_ ? kGap * s + noteHeight : 0.0f) +
                        shadow;
    if (totalHeight > availableHeight) {
        const float overflow = totalHeight - availableHeight;
        imageHeight = std::max(140.0f * s, imageHeight - overflow);
        imageWidth = imageHeight * aspect;
        if (hasNote_) {
            noteWidth = std::min(noteWidth, imageWidth);
        }
        totalHeight = shadow + imageHeight + (hasNote_ ? kGap * s + noteHeight : 0.0f) + shadow;
    }

    panelWidth_ = imageWidth + 2.0f * shadow;
    panelHeight_ = totalHeight;
    const float imageLeft = (panelWidth_ - imageWidth) * 0.5f;
    imageRect_ = D2D1::RectF(imageLeft, shadow, imageLeft + imageWidth,
                             shadow + imageHeight);

    if (hasNote_) {
        const float noteLeft = (panelWidth_ - noteWidth) * 0.5f;
        noteRect_ = D2D1::RectF(noteLeft, imageRect_.bottom + kGap * s,
                                noteLeft + noteWidth,
                                imageRect_.bottom + kGap * s + noteHeight);
        noteViewHeight_ =
            static_cast<int>(noteHeight - 2.0f * kNotePadding * s);
    } else {
        noteRect_ = D2D1::RectF();
        noteViewHeight_ = 0;
    }

    const float cardCenter = (cardScreen.left + cardScreen.right) * 0.5f;
    int x = static_cast<int>(std::lround(cardCenter - panelWidth_ * 0.5f));
    x = std::max<int>(static_cast<int>(workArea.left) + static_cast<int>(12 * s),
                      std::min<int>(x, static_cast<int>(workArea.right - panelWidth_) -
                                           static_cast<int>(12 * s)));
    const float belowOffset = item.note.empty() ? 12.0f : 42.0f;
    int y = static_cast<int>(std::lround(cardScreen.bottom + belowOffset * s));
    if (y + panelHeight_ > workArea.bottom) {
        y = static_cast<int>(std::lround(cardScreen.top - panelHeight_ - 12.0f * s));
    }
    y = std::max<int>(static_cast<int>(workArea.top) + static_cast<int>(8 * s),
                      std::min<int>(y, static_cast<int>(workArea.bottom - panelHeight_) -
                                           static_cast<int>(8 * s)));

    MoveTo(x, y, static_cast<int>(std::lround(panelWidth_)),
           static_cast<int>(std::lround(panelHeight_)));
    Render();
    ShowNoActivate();
}

void PreviewWindow::Dismiss()
{
    Hide();
    item_ = nullptr;
    scrollY_ = 0;
    noteContentHeight_ = 0;
}

RECT PreviewWindow::screenRect() const
{
    RECT rect{};
    if (hwnd()) {
        GetWindowRect(hwnd(), &rect);
    }
    return rect;
}

void PreviewWindow::BeforeDraw(ID2D1RenderTarget* target)
{
    if (!item_ || attempted_) {
        return;
    }
    attempted_ = true;
    graphics_->images().Load(item_->path, 2200.0f * scale_, target, preview_);
}

void PreviewWindow::Paint(ID2D1RenderTarget* target)
{
    if (!item_) {
        return;
    }
    const float s = scale_;
    const float imageRadius = 8.0f * s;

    DrawSoftShadow(target, imageRect_, imageRadius, 11.0f * s, 32.0f * s, 0.26f);

    if (preview_.valid()) {
        const D2D1_SIZE_F size = preview_.bitmap->GetSize();
        if (size.width > 0.0f && size.height > 0.0f) {
            const float fit = std::min((imageRect_.right - imageRect_.left) / size.width,
                                       (imageRect_.bottom - imageRect_.top) / size.height);
            const float w = size.width * fit;
            const float h = size.height * fit;
            const float cx = (imageRect_.left + imageRect_.right) * 0.5f;
            const float cy = (imageRect_.top + imageRect_.bottom) * 0.5f;
            D2D1_RECT_F dest = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                                           cy + h * 0.5f);
            ComPtr<ID2D1RoundedRectangleGeometry> geometry;
            graphics_->d2d()->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(dest, imageRadius, imageRadius), geometry.GetAddressOf());
            ComPtr<ID2D1Layer> layer;
            if (geometry && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
                target->DrawBitmap(preview_.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                target->PopLayer();
            } else {
                target->DrawBitmap(preview_.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
    } else {
        ComPtr<ID2D1SolidColorBrush> card;
        ComPtr<ID2D1SolidColorBrush> muted;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_.bg, card.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(theme_.fgTertiary, muted.GetAddressOf()))) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(imageRect_, imageRadius, imageRadius), card.Get());
            target->DrawText(L"图片暂时无法预览", 8, noteFormat_.Get(),
                             rack::Inset(imageRect_, 18 * s, 18 * s), muted.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (hasNote_ && noteFormat_) {
        DrawSoftShadow(target, noteRect_, 10.0f * s, 6.0f * s, 14.0f * s, 0.18f);

        ComPtr<ID2D1SolidColorBrush> bg;
        ComPtr<ID2D1SolidColorBrush> border;
        if (FAILED(target->CreateSolidColorBrush(theme_.bg, bg.GetAddressOf())) ||
            FAILED(target->CreateSolidColorBrush(
                D2D1::ColorF(theme_.border.r, theme_.border.g, theme_.border.b, 0.85f),
                border.GetAddressOf()))) {
            return;
        }
        target->FillRoundedRectangle(D2D1::RoundedRect(noteRect_, 10.0f * s, 10.0f * s),
                                     bg.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(noteRect_, 10.0f * s, 10.0f * s),
                                     border.Get(), 1.0f * s);

        const D2D1_RECT_F textRect =
            rack::Inset(noteRect_, kNotePadding * s, kNotePadding * s);
        const float textWidth = textRect.right - textRect.left;
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                item_->note.c_str(), static_cast<UINT32>(item_->note.size()),
                noteFormat_.Get(), textWidth, 10000.0f, layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                noteContentHeight_ = static_cast<int>(std::ceil(metrics.height));
            }
            const int viewHeight = static_cast<int>(textRect.bottom - textRect.top);
            const int maxScroll = std::max(0, noteContentHeight_ - viewHeight - 1);
            scrollY_ = rack::ClampInt(scrollY_, 0, maxScroll);

            target->PushAxisAlignedClip(textRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            ComPtr<ID2D1SolidColorBrush> ink;
            if (SUCCEEDED(target->CreateSolidColorBrush(theme_.fg, ink.GetAddressOf()))) {
                target->DrawTextLayout(
                    D2D1::Point2F(textRect.left, textRect.top - static_cast<float>(scrollY_)),
                    layout.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            target->PopAxisAlignedClip();

            if (maxScroll > 0) {
                ComPtr<ID2D1SolidColorBrush> bar;
                if (SUCCEEDED(target->CreateSolidColorBrush(
                        D2D1::ColorF(theme_.fgTertiary.r, theme_.fgTertiary.g,
                                     theme_.fgTertiary.b, 0.45f),
                        bar.GetAddressOf()))) {
                    const float barH = std::max(16.0f * s, viewHeight * viewHeight /
                                                               static_cast<float>(noteContentHeight_));
                    const float barY = textRect.top +
                                       (viewHeight - barH) * (static_cast<float>(scrollY_) /
                                                              static_cast<float>(maxScroll));
                    target->FillRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(textRect.right - 3.0f * s, barY, textRect.right,
                                        barY + barH),
                            1.5f * s, 1.5f * s),
                        bar.Get());
                }
            }
        }
    }
}

LRESULT PreviewWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEWHEEL: {
        if (item_ && hasNote_) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int maxScroll = std::max(0, noteContentHeight_ - noteViewHeight_ - 1);
            scrollY_ = rack::ClampInt(
                scrollY_ - (delta / WHEEL_DELTA) * static_cast<int>(48.0f * scale_), 0,
                maxScroll);
            Render();
        }
        return 0;
    }
    default:
        break;
    }
    return LayeredWindow::HandleMessage(message, wParam, lParam);
}


