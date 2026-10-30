#include "RackView.h"
#include "Graphics.h"
#include "Util.h"

namespace {

constexpr float kLineY = 14.0f;
constexpr float kCardTop = 42.0f;
/* Tendedero medium: photo max 136x104, card width 150, spacing +24. */
constexpr float kPhotoMaxWidth = 136.0f;
constexpr float kNoteCardWidth = 150.0f;
constexpr float kNoteCardHeight = 136.0f;
constexpr float kPhotoMaxHeight = 104.0f;
constexpr float kClipSlotRatio = 0.32f;
constexpr float kGap = 24.0f;
constexpr float kWindowHeight = 520.0f;
constexpr float kNoteHeight = 24.0f;
constexpr float kFrame = 4.0f;
constexpr float kOuterPad = 24.0f;
constexpr float kAccessoryWidth = 260.0f;

struct NotePalette {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F ink;
    D2D1_COLOR_F edge;
    D2D1_COLOR_F fold;
};

NotePalette PaletteFor(int color, bool dark)
{
    switch (color % 3) {
    case 1:
        return dark ? NotePalette{D2D1::ColorF(0.36f, 0.52f, 0.44f), D2D1::ColorF(0.93f, 0.97f, 0.94f),
                                   D2D1::ColorF(0.27f, 0.42f, 0.35f), D2D1::ColorF(0.28f, 0.43f, 0.36f)}
                    : NotePalette{D2D1::ColorF(0.81f, 0.95f, 0.87f), D2D1::ColorF(0.08f, 0.31f, 0.23f),
                                  D2D1::ColorF(0.66f, 0.87f, 0.76f), D2D1::ColorF(0.70f, 0.89f, 0.80f)};
    case 2:
        return dark ? NotePalette{D2D1::ColorF(0.47f, 0.43f, 0.62f), D2D1::ColorF(0.95f, 0.94f, 0.99f),
                                   D2D1::ColorF(0.36f, 0.32f, 0.50f), D2D1::ColorF(0.38f, 0.34f, 0.52f)}
                    : NotePalette{D2D1::ColorF(0.89f, 0.86f, 0.98f), D2D1::ColorF(0.23f, 0.17f, 0.39f),
                                  D2D1::ColorF(0.79f, 0.75f, 0.93f), D2D1::ColorF(0.82f, 0.78f, 0.94f)};
    default:
        return dark ? NotePalette{D2D1::ColorF(0.62f, 0.55f, 0.34f), D2D1::ColorF(0.99f, 0.96f, 0.85f),
                                   D2D1::ColorF(0.48f, 0.42f, 0.24f), D2D1::ColorF(0.50f, 0.44f, 0.26f)}
                    : NotePalette{D2D1::ColorF(1.0f, 0.945f, 0.66f), D2D1::ColorF(0.35f, 0.29f, 0.07f),
                                  D2D1::ColorF(0.91f, 0.84f, 0.48f), D2D1::ColorF(0.93f, 0.86f, 0.53f)};
    }
}

D2D1_POINT_2F RotateAround(D2D1_POINT_2F point, D2D1_POINT_2F center, float degrees)
{
    const float radians = degrees * 3.1415926535f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return D2D1::Point2F(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
}

float CardEntryOffset(const Item& item)
{
    const uint64_t now = rack::NowMs();
    if (item.addedMs == 0 || now <= item.addedMs) {
        return 0.0f;
    }
    const float age = static_cast<float>(now - item.addedMs) / 1000.0f;
    if (age > 1.2f) {
        return 0.0f;
    }
    /* Same drop spring as PicRope: response 0.42 s, damping 0.72, offset -46. */
    constexpr float kResponse = 0.42f;
    constexpr float kDamping = 0.72f;
    const float natural = 2.0f * 3.14159265f / kResponse;
    const float decay = natural * kDamping;
    const float damped = natural * std::sqrt(1.0f - kDamping * kDamping);
    return -46.0f * std::exp(-decay * age) *
           (std::cos(damped * age) + (decay / damped) * std::sin(damped * age));
}

void ShiftCard(CardBox& box, float dy)
{
    box.card.top += dy;
    box.card.bottom += dy;
    box.image.top += dy;
    box.image.bottom += dy;
    if (box.hasNote) {
        box.note.top += dy;
        box.note.bottom += dy;
    }
}

} /* namespace */

bool RackView::Init(Graphics* graphics)
{
    graphics_ = graphics;
    if (!graphics_ || !graphics_->dwrite()) {
        return false;
    }
    EnsureFormats(1.0f);
    return noteFormat_ != nullptr && placeholderFormat_ != nullptr &&
           toastFormat_ != nullptr;
}

void RackView::LoadAssets(ID2D1RenderTarget* target)
{
    if (assetsAttempted_ || !graphics_ || !target) {
        return;
    }
    assetsAttempted_ = true;
    const std::wstring dir = util::JoinPath(util::GetDllDir(), L"assets");
    /* Decode each bitmap close to twice its on-screen size. Letting D2D
       minify a 340 px envelope down to 70 px at draw time is what produced
       the hard, stair-stepped edge. WIC Fant does that resize once here. */
    const float assetScale = std::max(1.0f, scale_);
    graphics_->images().Load(util::JoinPath(dir, L"basket.png"), 224.0f * assetScale,
                             target, basket_);
    static const int kBasketFile[4] = {3, 0, 2, 1};  // 银灰, 奶白, 木棉, 深沉
    for (int i = 0; i < 4; ++i) {
        graphics_->images().Load(
            util::JoinPath(dir, L"basket_" + std::to_wstring(kBasketFile[i]) + L".png"),
            224.0f * assetScale, target, basketTheme_[i]);
    }
    graphics_->images().Load(util::JoinPath(dir, L"envelope_back.png"),
                             140.0f * assetScale, target, envelope_);
    graphics_->images().Load(util::JoinPath(dir, L"envelope_front.png"),
                             140.0f * assetScale, target, envelopeFront_);
    graphics_->images().Load(util::JoinPath(dir, L"letter.png"), 130.0f * assetScale,
                             target, letter_);
    static const int kStationeryFile[4] = {1, 0, 2, 3};  // 银灰, 奶白, 木棉, 深沉
    for (int i = 0; i < 4; ++i) {
        const std::wstring prefix = L"envelope_" + std::to_wstring(kStationeryFile[i]);
        graphics_->images().Load(util::JoinPath(dir, prefix + L"_back.png"),
                                 140.0f * assetScale, target, envelopeThemeBack_[i]);
        graphics_->images().Load(util::JoinPath(dir, prefix + L"_front.png"),
                                 140.0f * assetScale, target, envelopeThemeFront_[i]);
    }
    LoadIconFont();
}

void RackView::LoadIconFont()
{
    if (iconFace_ || !graphics_ || !graphics_->dwrite() || !graphics_->d2d()) {
        return;
    }
    const std::wstring path =
        util::JoinPath(util::JoinPath(util::GetDllDir(), L"assets"), L"iconfont.ttf");
    ComPtr<IDWriteFontFile> file;
    if (FAILED(graphics_->dwrite()->CreateFontFileReference(
            path.c_str(), nullptr, file.GetAddressOf())) || !file) {
        return;
    }
    IDWriteFontFile* files[] = {file.Get()};
    if (FAILED(graphics_->dwrite()->CreateFontFace(
            DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, files, 0,
            DWRITE_FONT_SIMULATIONS_NONE, iconFace_.GetAddressOf())) ||
        !iconFace_) {
        return;
    }
    BuildIconGlyph(0xE641, iconLock_);
    BuildIconGlyph(0xE643, iconUnlock_);
    BuildIconGlyph(0xE624, iconClose_);
}

void RackView::BuildIconGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out)
{
    if (!iconFace_ || !graphics_ || !graphics_->d2d()) {
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(graphics_->d2d()->CreatePathGeometry(geometry.GetAddressOf())) ||
        !geometry) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) {
        return;
    }
    UINT16 glyph = 0;
    if (iconFace_->GetGlyphIndices(&codepoint, 1, &glyph) != S_OK) {
        return;
    }
    FLOAT advance = 0.0f;
    auto* writeSink = reinterpret_cast<IDWriteGeometrySink*>(sink.Get());
    const HRESULT hr = iconFace_->GetGlyphRunOutline(
        100.0f, &glyph, &advance, nullptr, 1, FALSE, FALSE, writeSink);
    sink->Close();
    if (SUCCEEDED(hr)) {
        out = geometry;
    }
}

void RackView::DrawIcon(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                        D2D1_POINT_2F center, float size, ID2D1Brush* brush)
{
    if (!target || !glyph || !brush) {
        return;
    }
    D2D1_RECT_F bounds{};
    if (FAILED(glyph->GetBounds(D2D1::Matrix3x2F::Identity(), &bounds))) {
        return;
    }
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    const float scale = size / std::max(width, height);
    D2D1_MATRIX_3X2_F saved{};
    target->GetTransform(&saved);
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(
            center.x - (bounds.left + bounds.right) * 0.5f * scale,
            center.y - (bounds.top + bounds.bottom) * 0.5f * scale);
    target->SetTransform(transform * saved);
    target->FillGeometry(glyph, brush);
    target->SetTransform(saved);
}

void RackView::EnsureFormats(float scale)
{
    if (!graphics_ || !graphics_->dwrite()) {
        return;
    }
    if (noteFormat_ && placeholderFormat_ && badgeFormat_ && toastFormat_ &&
        std::abs(formatScale_ - scale) < 0.001f) {
        return;
    }

    auto makeFormat = [&](float size) -> ComPtr<IDWriteTextFormat> {
        ComPtr<IDWriteTextFormat> format;
        HRESULT hr = graphics_->dwrite()->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        if (FAILED(hr)) {
            hr = graphics_->dwrite()->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        }
        if (SUCCEEDED(hr) && format) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        return format;
    };

    noteFormat_ = makeFormat(10.0f * scale);
    placeholderFormat_ = makeFormat(9.0f * scale);
    badgeFormat_ = makeFormat(10.0f * scale);
    if (badgeFormat_) {
        badgeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    toastFormat_ = makeFormat(12.0f * scale);
    if (toastFormat_) {
        toastFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        toastFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        toastFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    formatScale_ = scale;
}

void RackView::Shutdown()
{
    noteFormat_.Reset();
    placeholderFormat_.Reset();
    badgeFormat_.Reset();
    toastFormat_.Reset();
    iconFace_.Reset();
    iconLock_.Reset();
    iconUnlock_.Reset();
    iconClose_.Reset();
    basket_.Reset();
    for (auto& basket : basketTheme_) basket.Reset();
    envelope_.Reset();
    envelopeFront_.Reset();
    letter_.Reset();
    for (int i = 0; i < 4; ++i) {
        envelopeThemeBack_[i].Reset();
        envelopeThemeFront_[i].Reset();
    }
    assetsAttempted_ = false;
    cards_.clear();
    graphics_ = nullptr;
}

RackLayout RackView::Layout(const std::vector<Item>& items, float maxWidth, float scale,
                            float expandAmount, float cardScale)
{
    RackLayout result;
    scale_ = std::max(0.5f, scale);
    cardScale_ = rack::Clamp(cardScale, 0.8f, 1.35f);
    const float cs = scale_ * cardScale_;
    EnsureFormats(scale_);
    const float s = scale_;
    result.width = std::max(320.0f * s, maxWidth);
    result.height = kWindowHeight * s;
    result.lineY = kLineY * s;
    result.expanded = expandAmount > 0.5f;
    layout_ = result;

    cards_.clear();
    basketRect_ = {};
    envelopeRect_ = {};

    const float photoWidth =
        std::max(120.0f * s, result.width - (2.0f * kOuterPad + kAccessoryWidth) * s);
    const float maxContent = photoWidth;
    std::vector<float> widths(items.size(), 0.0f);
    std::vector<float> heights(items.size(), 0.0f);

    auto measure = [&]() {
        float total = 0.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].path.empty() && !items[i].note.empty()) {
                widths[i] = kNoteCardWidth * cs + 2.0f * kFrame * cs;
                heights[i] = kNoteCardHeight * cs + 2.0f * kFrame * cs;
                total += widths[i];
                continue;
            }
            const float aspect = (items[i].aspect > 0.01f) ? items[i].aspect : 1.4f;
            float photoW = kPhotoMaxWidth * cs;
            float photoH = photoW / aspect;
            if (photoH > kPhotoMaxHeight * cs) {
                photoH = kPhotoMaxHeight * cs;
                photoW = photoH * aspect;
            }
            widths[i] = photoW + 2.0f * kFrame * cs;
            heights[i] = photoH + 2.0f * kFrame * cs;
            total += widths[i];
        }
        total += kGap * cs * static_cast<float>(items.size() - 1);
        return total;
    };

    float total = measure();
    if (total > maxContent) {
        const float factor = rack::Clamp(maxContent / total, 0.45f, 1.0f);
        for (size_t i = 0; i < items.size(); ++i) {
            widths[i] *= factor;
            heights[i] *= factor;
        }
        total = 0.0f;
        for (float width : widths) {
            total += width;
        }
        total += kGap * cs * static_cast<float>(items.size() - 1);
    }

    float gap = kGap * cs;
    if (total > maxContent && items.size() > 1) {
        gap = std::max(3.0f * s, (maxContent - total) / static_cast<float>(items.size() - 1) +
                                     gap);
        total = 0.0f;
        for (float width : widths) {
            total += width;
        }
        total += gap * static_cast<float>(items.size() - 1);
    }

    const float lineY = kLineY * s;
    const float sag = (16.0f + std::min<float>(6.0f, static_cast<float>(items.size())) * 1.2f) * s;
    const float lineSpan = std::max(1.0f, result.width - 2.0f * kOuterPad * s);
    result.lineY = lineY;
    result.sag = sag;
    result.lineStart = kOuterPad * s;
    result.lineEnd = result.width - kOuterPad * s;

    const float cardTop = kCardTop * s;
    float maxCardHeight = 0.0f;
    for (float height : heights) {
        maxCardHeight = std::max(maxCardHeight, height);
    }
    /* Collapsed cards sit above the window and slide down into view. */
    const float slide =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) * -(maxCardHeight + 30.0f * s);
    float x = kOuterPad * s + (photoWidth - total) * 0.5f;
    const float y = cardTop + slide;

    float contentBottom = 0.0f;
    cards_.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        CardBox box;
        const float w = widths[i];
        const float offset = items[i].rackOffsetX;
        box.card = D2D1::RectF(x + offset, y, x + w + offset, y + heights[i]);
        box.image = rack::Inset(box.card, kFrame * cs, kFrame * cs);
        const bool hasNote = !items[i].note.empty() && !items[i].path.empty();
        box.hasNote = hasNote;
        if (hasNote) {
            box.note = D2D1::RectF(box.card.left + 4.0f * s,
                                   box.card.bottom + 4.0f * s,
                                   box.card.right - 4.0f * s,
                                   box.card.bottom + 4.0f * s + kNoteHeight * s);
        }
        const float cx = (box.card.left + box.card.right) * 0.5f;
        const float t = rack::Clamp((cx - result.lineStart) / std::max(1.0f, lineSpan), 0.0f,
                                    1.0f);
        box.pinTop = lineY + 4.0f * sag * t * (1.0f - t);
        const int variant = (items[i].id * 37) % 8;
        box.angle = static_cast<float>(variant - 4) * 0.6f;
        cards_.push_back(box);
        x += w + gap;
    }

    float top = lineY - 2.0f * s;
    float bottom = lineY + sag + 136.0f * s;
    float left = 0.0f;
    float right = 0.0f;
    if (!cards_.empty()) {
        bottom = cards_.front().card.bottom;
        left = cards_.front().card.left;
        right = cards_.back().card.right;
    }
    for (const auto& box : cards_) {
        top = std::min(top, box.card.top);
        bottom = std::max(bottom, box.card.bottom);
        left = std::min(left, box.card.left);
        right = std::max(right, box.card.right);
        if (box.hasNote) {
            bottom = std::max(bottom, box.note.bottom);
            right = std::max(right, box.note.right);
        }
    }
    (void)left;
    (void)right;
    bottom = std::max(bottom, lineY + sag + 136.0f * s);
    // Collapsed rack is only the top hot-zone strip. Keeping a tall invisible
    // window alive there was stealing clicks from the desktop.
    result.height = expandAmount < 0.3f
                        ? 60.0f * s
                        : std::max(240.0f * s, bottom + 28.0f * s);
    layout_.height = result.height;
    result.content = D2D1::RectF(0.0f, lineY - 2.0f * s, result.width, bottom + 8.0f * s);

    const float accessoriesRight = result.width - kOuterPad * s;
    const float basketCenter = accessoriesRight - 78.0f * s;
    const float envelopeCenter = accessoriesRight - 196.0f * s;
    const float accessoryLineSpan = std::max(1.0f, result.lineEnd - result.lineStart);
    auto ropeYAt = [&](float x) {
        const float t = rack::Clamp((x - result.lineStart) / accessoryLineSpan, 0.0f, 1.0f);
        return lineY + 4.0f * sag * t * (1.0f - t);
    };
    const float basketTop = ropeYAt(basketCenter) - 14.0f * s;
    basketRect_ = D2D1::RectF(basketCenter - 54.0f * s, basketTop + 24.0f * s,
                              basketCenter + 54.0f * s, basketTop + 112.0f * s);
    const float envelopeTop = ropeYAt(envelopeCenter) + 8.0f * s;
    const float envelopeWidth = 70.0f * s;
    const float envelopeHeight = envelopeWidth * 376.0f / 340.0f;
    envelopeRect_ = D2D1::RectF(envelopeCenter - envelopeWidth * 0.5f, envelopeTop,
                                envelopeCenter + envelopeWidth * 0.5f,
                                envelopeTop + envelopeHeight + 26.0f * s);
    layout_ = result;
    return result;
}

void RackView::DrawShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                          float dy, float spread, float alpha)
{
    (void)radius;
    D2D1_GRADIENT_STOP stops[3]{};
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.95f);
    stops[1].position = 0.58f;
    stops[1].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.42f);
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

void RackView::Draw(ID2D1RenderTarget* target, const std::vector<Item>& items,
                    const Theme& theme, float expandAmount, float swaySeconds,
                    float basketHover, float paper0, float paper1, float paper2,
                    float cardHover, int cardHoverIndex, int hovered, int pressed,
                    int historyCount, int clipboardCount)
{
    if (!target || !graphics_) {
        return;
    }
    const float s = scale_;
    const RackLayout& layout = layout_;
    const bool expanded = expandAmount > 0.01f;
    lastHovered_ = hovered;

    if (expanded && layout.content.right > layout.content.left &&
        layout.content.bottom > layout.content.top) {
        ComPtr<ID2D1SolidColorBrush> hit;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(theme.canvas.r, theme.canvas.g, theme.canvas.b, 1.0f / 255.0f),
                hit.GetAddressOf()))) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(layout.content, 10.0f * s, 10.0f * s), hit.Get());
        }
    }

    lineRetract_ =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);

    if (!expanded) {
        return;
    }

    DrawLine(target, theme, s, expandAmount);
    DrawAccessories(target, items, theme, s, expandAmount, basketHover, paper0, paper1,
                    paper2, historyCount, clipboardCount);

    /* PicRope-style spring: a gentle angular impulse, ~0.95 s period and a
       low damping ratio. This is slower and softer than a plain sine decay. */
    auto swayFor = [&](const Item& item) {
        float base = 0.0f;
        if (swaySeconds > 0.0f) {
            constexpr float kResponse = 0.93f;
            constexpr float kDamping = 0.19f;
            constexpr float kImpulse = 9.6f;
            const float natural = 2.0f * 3.14159265f / kResponse;
            const float decay = natural * kDamping;
            const float damped = natural * std::sqrt(1.0f - kDamping * kDamping);
            base = std::exp(-decay * swaySeconds) * (kImpulse / damped) *
                   std::sin(damped * swaySeconds);
        }
        if (item.shakeStartMs != 0 && item.shakeAmp > 0.0f) {
            const float age = static_cast<float>(rack::NowMs() - item.shakeStartMs) / 1000.0f;
            if (age >= 0.0f && age < 1.2f) {
                // Tendedero: ease out to the new angle over 0.30 s, then an
                // interpolating spring (stiffness 38, damping 2.4) back to 0.
                if (age < 0.30f) {
                    base += item.shakeAmp * rack::EaseOutCubic(age / 0.30f);
                } else {
                    const float t = age - 0.30f;
                    constexpr float kStiffness = 38.0f;
                    constexpr float kDamping = 2.4f;
                    const float w = std::sqrt(kStiffness);
                    const float zeta = kDamping / (2.0f * w);
                    const float wd = w * std::sqrt(1.0f - zeta * zeta);
                    base += item.shakeAmp * std::exp(-zeta * w * t) *
                            (std::cos(wd * t) + (zeta / std::sqrt(1.0f - zeta * zeta)) *
                                                      std::sin(wd * t));
                }
            }
        }
        return base;
    };

    /* All shadows are painted first, otherwise a later card's shadow falls on
       the white frame of the previous card. */
    for (size_t i = 0; i < cards_.size() && i < items.size(); ++i) {
        if (static_cast<int>(i) == arrivalIndex_) continue;
        CardBox box = cards_[i];
        ShiftCard(box, CardEntryOffset(items[i]));
        DrawCardShadow(target, box, s, static_cast<int>(i) == hovered);
    }

    auto drawGroup = [&](size_t i, bool isHovered) {
        if (static_cast<int>(i) == arrivalIndex_) return;
        const float sway = swayFor(items[i]);
        CardBox box = cards_[i];
        ShiftCard(box, CardEntryOffset(items[i]));

        ComPtr<ID2D1Layer> fadeLayer;
        const bool fade = static_cast<int>(i) == dragging_;
        if (fade) {
            if (SUCCEEDED(target->CreateLayer(fadeLayer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 0.45f),
                    fadeLayer.Get());
            } else {
                fadeLayer.Reset();
            }
        }
        const float hoverAmount =
            static_cast<int>(i) == cardHoverIndex ? cardHover : 0.0f;
        DrawCard(target, items[i], box, theme, s, sway, hoverAmount, isHovered,
                 static_cast<int>(i) == pressed);
        DrawPin(target, box, theme, s);
        if (box.hasNote) {
            DrawNote(target, items[i], box, theme, s, sway);
        }
        if (fadeLayer) {
            target->PopLayer();
        }
    };

    /* Non-hovered cards first, then the hovered card so it sits above neighbours.
       The clip is drawn after the card so it visibly grips the photo's top edge. */
    for (size_t i = 0; i < cards_.size() && i < items.size(); ++i) {
        if (static_cast<int>(i) == hovered) {
            continue;
        }
        drawGroup(i, false);
    }
    if (hovered >= 0 && hovered < static_cast<int>(cards_.size()) &&
        hovered < static_cast<int>(items.size())) {
        drawGroup(static_cast<size_t>(hovered), true);
    }

    DrawRemoved(target, theme, s);
    if (arrivalIndex_ >= 0 && arrivalIndex_ < static_cast<int>(items.size())) {
        DrawArrival(target, items[static_cast<size_t>(arrivalIndex_)], theme, s);
    }
    DrawCopyToast(target, s);
}

D2D1_POINT_2F RackView::EnvelopeBadgeCenter() const
{
    const float s = scale_;
    const float span = std::max(1.0f, layout_.lineEnd - layout_.lineStart);
    const float center = layout_.width - kOuterPad * s - 196.0f * s;
    const float t = rack::Clamp((center - layout_.lineStart) / span, 0.0f, 1.0f);
    const float ropeY = layout_.lineY + 4.0f * layout_.sag * t * (1.0f - t);
    const float envelopeBottom = ropeY + 8.0f * s + 70.0f * s * 376.0f / 340.0f;
    const float basketCenter = layout_.width - kOuterPad * s - 78.0f * s;
    const float bt = rack::Clamp((basketCenter - layout_.lineStart) / span, 0.0f, 1.0f);
    const float basketBottom =
        layout_.lineY + 4.0f * layout_.sag * bt * (1.0f - bt) - 14.0f * s + 112.0f * s;
    const float badgeTop = std::max(envelopeBottom, basketBottom) + 2.0f * s;
    return D2D1::Point2F(center, badgeTop + 18.0f * s);
}

void RackView::BeginArrival(int index)
{
    if (index < 0) return;
    arrivalIndex_ = index;
    arrivalStartMs_ = rack::NowMs();
    arrivalStart_ = D2D1::Point2F(layout_.width * 0.5f, layout_.height * 0.72f);
    arrivalStartSet_ = false;
}

void RackView::SetArrivalStart(float x, float y, float w, float h)
{
    arrivalStart_ = D2D1::Point2F(x, y);
    arrivalW_ = w;
    arrivalH_ = h;
    arrivalStartSet_ = true;
}

bool RackView::HasArrival() const
{
    return arrivalIndex_ >= 0 &&
           rack::NowMs() - arrivalStartMs_ < 650;
}

void RackView::DrawArrival(ID2D1RenderTarget* target, const Item& item, const Theme& theme,
                           float s)
{
    if (arrivalIndex_ < 0 || arrivalIndex_ >= static_cast<int>(cards_.size())) {
        arrivalIndex_ = -1;
        return;
    }
    const float age = static_cast<float>(rack::NowMs() - arrivalStartMs_) / 1000.0f;
    if (age >= 0.65f) {
        arrivalIndex_ = -1;
        return;
    }
    const CardBox& targetBox = cards_[static_cast<size_t>(arrivalIndex_)];
    const float raw = rack::Clamp(age / 0.65f, 0.0f, 1.0f);
    const float t = raw < 0.5f ? 4.0f * raw * raw * raw
                               : 1.0f - std::pow(-2.0f * raw + 2.0f, 3.0f) * 0.5f;
    const float targetW = targetBox.card.right - targetBox.card.left;
    const float targetH = targetBox.card.bottom - targetBox.card.top;
    const float startW = arrivalW_ > 0.0f ? arrivalW_ : targetW * 1.35f;
    const float startH = arrivalH_ > 0.0f ? arrivalH_ : targetH * 1.35f;
    const float width = startW + (targetW - startW) * t;
    const float height = startH + (targetH - startH) * t;
    const D2D1_POINT_2F start = arrivalStartSet_
                                    ? arrivalStart_
                                    : D2D1::Point2F(layout_.width * 0.5f,
                                                    layout_.height * 0.72f);
    const float targetCx = (targetBox.card.left + targetBox.card.right) * 0.5f;
    const float targetCy = (targetBox.card.top + targetBox.card.bottom) * 0.5f;
    const float x = start.x + (targetCx - start.x) * t;
    const float y = start.y + (targetCy - start.y) * t -
                    30.0f * s * std::sin(3.14159265f * t);
    CardBox box = targetBox;
    box.card = D2D1::RectF(x - width * 0.5f, y - height * 0.5f,
                           x + width * 0.5f, y + height * 0.5f);
    const float inset = std::min(4.0f * s, kFrame * s * t);
    box.image = rack::Inset(box.card, inset, inset);
    const float spin = targetBox.angle * t;
    DrawCard(target, item, box, theme, s, spin, 0.0f, false, false);
}

void RackView::BeginRemove(int index, const Item& item, bool toBasket)
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return;
    }
    RemovedCard removed;
    removed.item = item;
    removed.box = cards_[static_cast<size_t>(index)];
    removed.removedMs = rack::NowMs();
    removed.toBasket = toBasket;
    removed_.push_back(std::move(removed));
}

void RackView::DrawRemoved(ID2D1RenderTarget* target, const Theme& theme, float s)
{
    if (removed_.empty()) {
        return;
    }
    const uint64_t now = rack::NowMs();
    const float basketCenterX = layout_.width - kOuterPad * s - 78.0f * s;
    const float mouthY = basketRect_.top > 0.0f ? basketRect_.top + 8.0f * s
                                                : layout_.lineY + 40.0f * s;
    for (auto it = removed_.begin(); it != removed_.end();) {
        const float age = static_cast<float>(now - it->removedMs) / 1000.0f;
        if (!it->toBasket) {
            // Tendedero's fall: 0.55 s, 520 points, ease-in cubic, extra
            // rotation and a linear fade — not an accelerated screen drop.
            constexpr float kFallSeconds = 0.55f;
            if (age > kFallSeconds) {
                it = removed_.erase(it);
                continue;
            }
            const float raw = rack::Clamp(age / kFallSeconds, 0.0f, 1.0f);
            const float e = raw * raw * raw;
            CardBox box = it->box;
            ShiftCard(box, 520.0f * s * e);
            const float spin = (it->box.angle * 7.0f + 20.0f) * e;
            ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 1.0f - e),
                    layer.Get());
            }
            DrawCard(target, it->item, box, theme, s, spin, 0.0f, false, false);
            if (layer) target->PopLayer();
            ++it;
            continue;
        }
        if (age > 0.64f) {
            it = removed_.erase(it);
            continue;
        }
        const float t = rack::Clamp(age / 0.64f, 0.0f, 1.0f);
        const float ease = t * t * t * (10.0f + t * (-15.0f + 6.0f * t));
        CardBox box = it->box;
        const float originalW = box.card.right - box.card.left;
        const float originalH = box.card.bottom - box.card.top;
        const float smallW = std::min(22.0f * s, std::max(10.0f * s, originalW * 0.15f));
        const float smallH = smallW * originalH / std::max(1.0f, originalW);
        const float startX = (box.card.left + box.card.right) * 0.5f;
        const float startY = (box.card.top + box.card.bottom) * 0.5f;
        const float width = originalW + (smallW - originalW) * ease;
        const float height = originalH + (smallH - originalH) * ease;
        const float x = startX + (basketCenterX - startX) * ease;
        const float lift = std::min(38.0f * s, std::max(10.0f * s, startY - 10.0f * s));
        const float y = startY + (mouthY - startY) * ease -
                        lift * std::sin(3.14159265f * ease);
        box.card = D2D1::RectF(x - width * 0.5f, y - height * 0.5f,
                               x + width * 0.5f, y + height * 0.5f);
        const float inset = std::min(4.0f * s, kFrame * s);
        box.image = rack::Inset(box.card, inset, inset);
        const float spin = -box.angle * ease +
                           (basketCenterX < startX ? -1.0f : 1.0f) * 10.0f *
                               std::sin(3.14159265f * ease);
        const float fade = 1.0f - 0.8f * rack::Clamp((t - 0.82f) / 0.18f, 0.0f, 1.0f);
        ComPtr<ID2D1Layer> layer;
        if (SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
            target->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), fade),
                layer.Get());
        }
        DrawCard(target, it->item, box, theme, s, spin, 0.0f, false, false);
        if (layer) {
            target->PopLayer();
        }
        ++it;
    }
}

void RackView::DrawLine(ID2D1RenderTarget* target, const Theme& theme, float s,
                        float expandAmount)
{
    const RackLayout& layout = layout_;
    if (layout.lineEnd <= layout.lineStart) {
        return;
    }
    const float retract =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);
    // PicRope rope colours and widths per theme.
    D2D1_COLOR_F ropeColor{};
    D2D1_COLOR_F ropeHighlight{};
    float ropeWidth = 1.6f;
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 1:  // 奶白, porcelain
        ropeColor = D2D1::ColorF(0.68f, 0.69f, 0.69f);
        ropeHighlight = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        ropeWidth = 1.9f;
        break;
    case 2:  // 木棉, linen
        ropeColor = D2D1::ColorF(0.73f, 0.65f, 0.54f);
        ropeHighlight = D2D1::ColorF(0.92f, 0.87f, 0.79f);
        ropeWidth = 2.3f;
        break;
    case 3:  // 深沉, graphite
        ropeColor = D2D1::ColorF(0.34f, 0.36f, 0.39f);
        ropeHighlight = D2D1::ColorF(0.58f, 0.61f, 0.64f);
        ropeWidth = 1.65f;
        break;
    default:  // 银灰, classic
        ropeColor = D2D1::ColorF(0.54f, 0.59f, 0.65f);
        ropeHighlight = D2D1::ColorF(0.78f, 0.81f, 0.85f);
        ropeWidth = 1.6f;
        break;
    }
    const float y = layout.lineY + retract;
    const float sag = layout.sag;

    /* Tendedero fades the rope out over the first/last 8% so it feels like it
       comes from beyond the screen, instead of ending in two hard dots. */
    auto makeFadeBrush = [&](const D2D1_COLOR_F& color,
                             ComPtr<ID2D1LinearGradientBrush>& out) -> bool {
        D2D1_GRADIENT_STOP stops[4]{};
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(color.r, color.g, color.b, 0.0f);
        stops[1].position = 0.08f;
        stops[1].color = color;
        stops[2].position = 0.92f;
        stops[2].color = color;
        stops[3].position = 1.0f;
        stops[3].color = D2D1::ColorF(color.r, color.g, color.b, 0.0f);
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(target->CreateGradientStopCollection(
                stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                collection.GetAddressOf()))) {
            return false;
        }
        return SUCCEEDED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(layout.lineStart, y), D2D1::Point2F(layout.lineEnd, y)),
            collection.Get(), out.GetAddressOf()));
    };

    ComPtr<ID2D1LinearGradientBrush> shadow;
    ComPtr<ID2D1LinearGradientBrush> line;
    ComPtr<ID2D1LinearGradientBrush> highlight;
    if (!makeFadeBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.22f), shadow) ||
        !makeFadeBrush(ropeColor, line) ||
        !makeFadeBrush(ropeHighlight, highlight)) {
        return;
    }
    const float midX = (layout.lineStart + layout.lineEnd) * 0.5f;
    auto buildCurve = [&](float dy, ComPtr<ID2D1PathGeometry>& path) -> bool {
        if (FAILED(graphics_->d2d()->CreatePathGeometry(path.GetAddressOf()))) {
            return false;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(sink.GetAddressOf()))) {
            return false;
        }
        const D2D1_POINT_2F start = D2D1::Point2F(layout.lineStart, y + dy);
        const D2D1_POINT_2F end = D2D1::Point2F(layout.lineEnd, y + dy);
        const D2D1_POINT_2F control = D2D1::Point2F(midX, y + sag * 2.0f + dy);
        const D2D1_POINT_2F c1 = D2D1::Point2F(start.x + (control.x - start.x) * 2.0f / 3.0f,
                                              start.y + (control.y - start.y) * 2.0f / 3.0f);
        const D2D1_POINT_2F c2 = D2D1::Point2F(end.x + (control.x - end.x) * 2.0f / 3.0f,
                                              end.y + (control.y - end.y) * 2.0f / 3.0f);
        sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        return true;
    };

    ComPtr<ID2D1PathGeometry> shadowPath;
    if (buildCurve(1.0f * s, shadowPath)) {
        target->DrawGeometry(shadowPath.Get(), shadow.Get(), 2.2f * s);
    }
    ComPtr<ID2D1PathGeometry> linePath;
    if (buildCurve(0.0f, linePath)) {
        target->DrawGeometry(linePath.Get(), line.Get(), ropeWidth * s);
    }
    ComPtr<ID2D1PathGeometry> highlightPath;
    if (buildCurve(-0.35f * s, highlightPath)) {
        target->DrawGeometry(highlightPath.Get(), highlight.Get(),
                             std::max(0.4f, ropeWidth * 0.26f) * s);
    }
}

void RackView::DrawPin(ID2D1RenderTarget* target, const CardBox& box, const Theme& theme,
                       float s)
{
    const float cx = (box.card.left + box.card.right) * 0.5f;
    const float clipHeight = 24.0f * s;
    /* Put the slot exactly on the rope, then let the body hang down over
       enough of the white frame to actually grip the photo. */
    DrawClipAt(target, cx, box.pinTop + lineRetract_ - clipHeight * kClipSlotRatio,
               theme, s);
}

void RackView::DrawClipAt(ID2D1RenderTarget* target, float cx, float top, const Theme& theme,
                          float s, float heightDips)
{
    /* Tendedero's peg is one rounded, metallic 9x26 plate, not a two-piece
       clothespin. Keep that simplicity and the same soft horizontal finish. */
    const float width = 8.0f * s;
    const float height = heightDips * s;
    const D2D1_RECT_F rect =
        D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);

    // The peg belongs to the theme's hardware, not a single steel palette.
    D2D1_COLOR_F peg[3]{};
    D2D1_COLOR_F pegEdge{};
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 3:  // 深沉, PicRope graphite #575E66
        peg[0] = D2D1::ColorF(0.43f, 0.46f, 0.49f);
        peg[1] = D2D1::ColorF(0.34f, 0.37f, 0.40f);
        peg[2] = D2D1::ColorF(0.27f, 0.30f, 0.32f);
        pegEdge = D2D1::ColorF(0.26f, 0.29f, 0.31f);
        break;
    case 2:  // 木棉, PicRope linen #DFC5A3 (light beech)
        peg[0] = D2D1::ColorF(0.92f, 0.84f, 0.74f);
        peg[1] = D2D1::ColorF(0.87f, 0.77f, 0.64f);
        peg[2] = D2D1::ColorF(0.80f, 0.69f, 0.54f);
        pegEdge = D2D1::ColorF(0.68f, 0.56f, 0.41f);
        break;
    case 1:  // 奶白, PicRope porcelain #F3F0E8
        peg[0] = D2D1::ColorF(0.98f, 0.97f, 0.95f);
        peg[1] = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        peg[2] = D2D1::ColorF(0.91f, 0.89f, 0.85f);
        pegEdge = D2D1::ColorF(0.76f, 0.75f, 0.71f);
        break;
    default:  // 银灰, PicRope classic #DCE3EC
        peg[0] = D2D1::ColorF(0.90f, 0.92f, 0.95f);
        peg[1] = D2D1::ColorF(0.86f, 0.89f, 0.93f);
        peg[2] = D2D1::ColorF(0.76f, 0.80f, 0.85f);
        pegEdge = D2D1::ColorF(0.54f, 0.60f, 0.67f);
        break;
    }
    D2D1_GRADIENT_STOP stops[3]{};
    stops[0].position = 0.0f;
    stops[0].color = peg[0];
    stops[1].position = 0.45f;
    stops[1].color = peg[1];
    stops[2].position = 1.0f;
    stops[2].color = peg[2];

    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    if (FAILED(target->CreateGradientStopCollection(
            stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            collection.GetAddressOf())) ||
        FAILED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rect.left, rect.top),
                D2D1::Point2F(rect.right, rect.top)),
            collection.Get(), fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(pegEdge, edge.GetAddressOf()))) {
        return;
    }

    const bool wood = rack::ClampInt(theme.index, 0, 3) == 2;
    if (wood) {
        // Rear jaw of the wooden clothes peg, offset a little to the right.
        ComPtr<ID2D1SolidColorBrush> rear;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                    D2D1::ColorF(0.78f, 0.66f, 0.51f), rear.GetAddressOf()))) {
            D2D1_RECT_F back = rect;
            back.left += 1.6f * s;
            back.right += 1.6f * s;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(back, 2.4f * s, 2.4f * s), rear.Get());
        }
    }
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect, wood ? 2.4f * s : 3.0f * s,
                          wood ? 2.4f * s : 3.0f * s),
        fill.Get());
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(rect, wood ? 2.4f * s : 3.0f * s,
                          wood ? 2.4f * s : 3.0f * s),
        edge.Get(), wood ? 0.7f * s : 0.6f * s);
    if (wood) {
        ComPtr<ID2D1SolidColorBrush> grain;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(0.54f, 0.39f, 0.22f, 0.26f), grain.GetAddressOf()))) {
            // One faint grain strand, not a striped/wood-grain pattern.
            const float gx = rect.left + 3.0f * s;
            target->DrawLine(D2D1::Point2F(gx, rect.top + 4.0f * s),
                             D2D1::Point2F(gx + 0.4f * s, rect.bottom - 5.0f * s),
                             grain.Get(), 0.45f * s);
            const float slotY = top + height * kClipSlotRatio;
            target->DrawLine(D2D1::Point2F(rect.left + 1.0f * s, slotY),
                             D2D1::Point2F(rect.right - 1.0f * s, slotY),
                             grain.Get(), 0.55f * s);
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(rect.right - 0.5f * s,
                                            rect.top + height * 0.52f),
                              1.4f * s, 1.6f * s),
                grain.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> highlight;
    if (!wood && SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(1.f, 1.f, 1.f, theme.dark ? 0.18f : 0.50f),
            highlight.GetAddressOf()))) {
        target->DrawLine(D2D1::Point2F(rect.left + 2.0f * s, rect.top + 3.0f * s),
                         D2D1::Point2F(rect.left + 2.0f * s, rect.bottom - 3.0f * s),
                         highlight.Get(), 0.7f * s);
    }

    /* The middle slot reads as the spring/hinge, like Tendedero's peg. */
    ComPtr<ID2D1SolidColorBrush> slot;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(edge->GetColor().r, edge->GetColor().g, edge->GetColor().b, 0.72f),
            slot.GetAddressOf()))) {
        const float slotY = top + height * kClipSlotRatio;
        target->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(cx - 2.5f * s, slotY - 0.7f * s, cx + 2.5f * s,
                            slotY + 0.7f * s),
                0.7f * s, 0.7f * s),
            slot.Get());
    }
}

void RackView::DrawAccessories(ID2D1RenderTarget* target,
                               const std::vector<Item>& items, const Theme& theme, float s,
                               float expandAmount, float basketHover, float paper0,
                               float paper1, float paper2, int historyCount,
                               int clipboardCount)
{
    const RackLayout& layout = layout_;
    const float lineSpan = std::max(1.0f, layout.lineEnd - layout.lineStart);
    const float retract =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);
    auto ropeYAt = [&](float x) {
        const float t = rack::Clamp((x - layout.lineStart) / lineSpan, 0.0f, 1.0f);
        return layout.lineY + retract + 4.0f * layout.sag * t * (1.0f - t);
    };

    const float accessoriesRight = layout.width - kOuterPad * s;
    const float basketCenter = accessoriesRight - 78.0f * s;
    const float envelopeCenter = accessoriesRight - 196.0f * s;
    /* Both count pills sit on one shared baseline, regardless of the envelope
       and basket having different heights. */
    const float envelopeBottom =
        ropeYAt(envelopeCenter) + 8.0f * s + 70.0f * s * 376.0f / 340.0f;
    const float basketBottom = ropeYAt(basketCenter) - 14.0f * s + 112.0f * s;
    const float badgeTop = std::max(envelopeBottom, basketBottom) + 2.0f * s;

    const int themeIndex = rack::ClampInt(theme.index, 0, 3);
    const ImageBitmap* envelopeBack = envelopeThemeBack_[themeIndex].valid()
                                          ? &envelopeThemeBack_[themeIndex]
                                          : &envelope_;
    const ImageBitmap* envelopeFront = envelopeThemeFront_[themeIndex].valid()
                                           ? &envelopeThemeFront_[themeIndex]
                                           : &envelopeFront_;
    if (envelopeBack && envelopeBack->valid()) {
        const float envelopeWidth = 70.0f * s;
        const float envelopeHeight = envelopeWidth * 376.0f / 340.0f;
        const float top = ropeYAt(envelopeCenter) + 8.0f * s;
        const D2D1_RECT_F dest =
            D2D1::RectF(envelopeCenter - envelopeWidth * 0.5f, top,
                        envelopeCenter + envelopeWidth * 0.5f, top + envelopeHeight);

        /* Exactly the user's layer order, bottom to top:
           back -> clip -> letter -> front. The themed back already contains
           its own paper, so the separate letter layer is only for the legacy
           fallback artwork. */
        target->DrawBitmap(envelopeBack->bitmap.Get(), dest, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        DrawClipAt(target, envelopeCenter,
                   ropeYAt(envelopeCenter) - 24.0f * s * kClipSlotRatio, theme, s);

        // One shared sheet of stationery for every theme; only the envelope
        // colour changes underneath it.
        if (letter_.valid()) {
            const float amount = rack::Clamp(paper0, 0.0f, 1.05f);
            const float paperTop = top + (0.18f - 0.18f * amount) * envelopeHeight;
            const float paperWidth = envelopeWidth * (65.0f / 85.0f);
            const float paperHeight = paperWidth * 312.0f / 288.0f;
            const D2D1_RECT_F paper = D2D1::RectF(
                envelopeCenter - paperWidth * 0.5f, paperTop,
                envelopeCenter + paperWidth * 0.5f, paperTop + paperHeight);
            target->DrawBitmap(letter_.bitmap.Get(), paper, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        if (envelopeFront && envelopeFront->valid()) {
            target->DrawBitmap(envelopeFront->bitmap.Get(), dest, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        DrawCountBadge(target, envelopeCenter, badgeTop, clipboardCount, s);
    }

    const ImageBitmap* basket = basketTheme_[themeIndex].valid()
                                   ? &basketTheme_[themeIndex]
                                   : &basket_;
    if (basket && basket->valid()) {
        /* Keep the spring value signed: on leave it swings past zero and back,
           which is what makes the basket settle naturally. */
        const float basketHoverAmount = basketHover;
        const float basketSize = 112.0f * s;
        const float themeDrop = themeIndex == 1 ? 2.0f * s : 0.0f;
        const float top = ropeYAt(basketCenter) - 14.0f * s + themeDrop;
        D2D1_RECT_F dest = D2D1::RectF(basketCenter - basketSize * 0.5f, top,
                                       basketCenter + basketSize * 0.5f, top + basketSize);
        const D2D1_POINT_2F hook = D2D1::Point2F(basketCenter, ropeYAt(basketCenter));
        target->SetTransform(
            D2D1::Matrix3x2F::Rotation(-2.0f * basketHoverAmount, hook));
        target->DrawBitmap(basket->bitmap.Get(), dest, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        DrawCountBadge(target, basketCenter, badgeTop, historyCount, s);
    }
}

void RackView::DrawCountBadge(ID2D1RenderTarget* target, float centerX, float top, int count,
                              float s)
{
    if (!badgeFormat_) {
        return;
    }
    const float width = 28.0f * s;
    const float height = 18.0f * s;
    D2D1_RECT_F rect =
        D2D1::RectF(centerX - width * 0.5f, top, centerX + width * 0.5f, top + height);

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> text;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.15f, 0.82f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.99f),
                                             text.GetAddressOf()))) {
        return;
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 9.0f * s, 9.0f * s), fill.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 9.0f * s, 9.0f * s), border.Get(),
                                 0.8f * s);

    wchar_t label[8]{};
    if (count > 99) {
        wcscpy_s(label, L"99+");
    } else {
        swprintf_s(label, L"%d", count);
    }
    target->DrawText(label, static_cast<UINT32>(wcslen(label)), badgeFormat_.Get(), rect,
                     text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RackView::ShowCopyToast(int index, uint64_t untilMs)
{
    copyToastIndex_ = index;
    copyToastUntil_ = untilMs;
}

void RackView::ClearCopyToast()
{
    copyToastIndex_ = -1;
    copyToastUntil_ = 0;
}

void RackView::DrawCopyToast(ID2D1RenderTarget* target, float s)
{
    if (copyToastIndex_ < 0 || rack::NowMs() >= copyToastUntil_ ||
        copyToastIndex_ >= static_cast<int>(cards_.size()) || !toastFormat_) {
        return;
    }
    const CardBox& box = cards_[copyToastIndex_];
    const float width = 70.0f * s;
    const float height = 22.0f * s;
    const float centerX = (box.card.left + box.card.right) * 0.5f;
    const D2D1_RECT_F rect = D2D1::RectF(centerX - width * 0.5f,
                                         box.card.bottom + 10.0f * s,
                                         centerX + width * 0.5f,
                                         box.card.bottom + 10.0f * s + height);
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> text;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.15f, 0.82f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.99f),
                                             text.GetAddressOf()))) {
        return;
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 11.0f * s, 11.0f * s),
                                 fill.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 11.0f * s, 11.0f * s),
                                 border.Get(), 0.8f * s);
    target->DrawText(L"复制成功", 4, toastFormat_.Get(), rect, text.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RackView::DrawCardShadow(ID2D1RenderTarget* target, const CardBox& box, float s,
                              bool hovered)
{
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    target->SetTransform(D2D1::Matrix3x2F::Rotation(box.angle, pivot));
    DrawShadow(target, box.card, 12.0f * s, 5.0f * s, hovered ? 20.0f * s : 18.0f * s,
               hovered ? 0.16f : 0.12f);
    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RackView::DrawCard(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                        const Theme& theme, float s, float swayAngle, float hoverAmount,
                        bool hovered, bool pressed)
{
    const float radius = 12.0f * s;
    const float cardScale =
        pressed ? 0.97f : (1.0f + 0.040f * rack::Clamp(hoverAmount, 0.0f, 1.05f));
    D2D1_RECT_F rect = box.card;
    if (pressed) {
        rect.top += 1.0f * s;
        rect.bottom += 1.0f * s;
    }
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    /* Scale around the clip point: the photo lifts slightly but the clip stays
       fixed, matching PicRope's Surface scale-motion. */
    target->SetTransform(
        D2D1::Matrix3x2F::Scale(cardScale, cardScale, pivot) *
        D2D1::Matrix3x2F::Rotation(box.angle + swayAngle, pivot));

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> imageBack;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.88f, 0.91f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.95f, 0.97f),
                                             imageBack.GetAddressOf()))) {
        return;
    }

    const bool isNoteCard = item.path.empty() && !item.note.empty();
    if (!isNoteCard) {
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), fill.Get());
    }

    D2D1_RECT_F imageRect = rack::Inset(rect, kFrame * s, kFrame * s);
    const bool textCard = item.path.empty() && !item.note.empty();
    if (textCard) {
        const float sw = (rect.right - rect.left) / 496.0f;
        const float sh = (rect.bottom - rect.top) / 503.0f;
        auto pt = [&](float x, float y) {
            return D2D1::Point2F(rect.left + x * sw, rect.top + y * sh);
        };
        ComPtr<ID2D1PathGeometry> notePath;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(notePath.GetAddressOf())) && notePath) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(notePath->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(pt(494.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(pt(0.0f, 0.0f));
                sink->AddLine(pt(0.0f, 494.5f));
                sink->AddBezier(D2D1::BezierSegment(pt(188.7f, 497.2f), pt(294.4f, 499.8f),
                                                    pt(486.0f, 478.5f)));
                sink->AddBezier(D2D1::BezierSegment(pt(493.7f, 297.8f), pt(495.4f, 192.9f),
                                                    pt(494.0f, 0.0f)));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                D2D1_GRADIENT_STOP stops[2] = {
                    {0.0f, D2D1::ColorF(0xF4D476)},
                    {1.0f, D2D1::ColorF(0xF7E3A7)},
                };
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> noteFill;
                if (SUCCEEDED(target->CreateGradientStopCollection(
                        stops, 2, collection.GetAddressOf())) &&
                    SUCCEEDED(target->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(pt(9.0f, 0.0f), pt(480.0f, 468.5f)),
                        collection.Get(), noteFill.GetAddressOf()))) {
                    target->FillGeometry(notePath.Get(), noteFill.Get());
                }
            }
        }
        if (noteFormat_) {
            const D2D1_RECT_F textRect = rack::Inset(rect, 16.0f * s, 18.0f * s);
            ComPtr<ID2D1SolidColorBrush> ink;
            if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0x5A462A),
                                                        ink.GetAddressOf()))) {
                target->DrawText(item.note.c_str(), static_cast<UINT32>(item.note.size()),
                                 noteFormat_.Get(), textRect, ink.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }    } else {
    target->FillRoundedRectangle(D2D1::RoundedRect(imageRect, 7.0f * s, 7.0f * s),
                                 imageBack.Get());

    if (item.thumb.valid()) {
        const D2D1_SIZE_F size = item.thumb.bitmap->GetSize();
        if (size.width > 0.0f && size.height > 0.0f) {
            const float fit = std::min((imageRect.right - imageRect.left) / size.width,
                                       (imageRect.bottom - imageRect.top) / size.height);
            const float w = size.width * fit;
            const float h = size.height * fit;
            const float cx = (imageRect.left + imageRect.right) * 0.5f;
            const float cy = (imageRect.top + imageRect.bottom) * 0.5f;
            D2D1_RECT_F dest = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                                           cy + h * 0.5f);

            ComPtr<ID2D1RoundedRectangleGeometry> geometry;
            graphics_->d2d()->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(imageRect, 7.0f * s, 7.0f * s),
                geometry.GetAddressOf());
            ComPtr<ID2D1Layer> layer;
            if (geometry && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
                target->DrawBitmap(item.thumb.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                target->PopLayer();
            } else {
                target->DrawBitmap(item.thumb.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
    } else if (placeholderFormat_) {
        ComPtr<ID2D1SolidColorBrush> muted;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.38f, 0.42f, 0.9f),
                                                    muted.GetAddressOf()))) {
            D2D1_RECT_F textRect = rack::Inset(imageRect, 6.0f * s, 6.0f * s);
            target->DrawText(L"无法读取", 4, placeholderFormat_.Get(), textRect, muted.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    }
    if (!isNoteCard) {
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), border.Get(),
                                     1.0f * s);
    }

    if (hovered && hoverAmount > 0.15f) {
        const float size = 20.0f * s;
        const D2D1_POINT_2F closeCenter =
            D2D1::Point2F(rect.right - 16.0f * s, rect.top + 16.0f * s);
        const D2D1_POINT_2F lockCenter =
            D2D1::Point2F(rect.left + 16.0f * s, rect.top + 16.0f * s);
        ComPtr<ID2D1SolidColorBrush> fill;
        ComPtr<ID2D1SolidColorBrush> edge;
        ComPtr<ID2D1SolidColorBrush> ink;
        const Theme ui = Theme::MenuStyle(uiDark_);
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 0.94f),
                fill.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(ui.border, edge.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(ui.fg, ink.GetAddressOf()))) {
            auto glass = [&](const D2D1_POINT_2F& c) {
                target->FillEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f), fill.Get());
                target->DrawEllipse(D2D1::Ellipse(c, size * 0.5f, size * 0.5f),
                                    edge.Get(), 0.8f * s);
            };
            glass(closeCenter);
            if (iconClose_) {
                DrawIcon(target, iconClose_.Get(), closeCenter, 6.0f * s, ink.Get());
            } else {
                const float arm = 3.7f * s;
                target->DrawLine(D2D1::Point2F(closeCenter.x - arm, closeCenter.y - arm),
                                 D2D1::Point2F(closeCenter.x + arm, closeCenter.y + arm),
                                 ink.Get(), 1.4f * s);
                target->DrawLine(D2D1::Point2F(closeCenter.x + arm, closeCenter.y - arm),
                                 D2D1::Point2F(closeCenter.x - arm, closeCenter.y + arm),
                                 ink.Get(), 1.4f * s);
            }

            glass(lockCenter);
            if (iconLock_ && iconUnlock_) {
                DrawIcon(target, item.locked ? iconLock_.Get() : iconUnlock_.Get(),
                         lockCenter, 11.0f * s, ink.Get());
            } else {
                const float lockW = 7.0f * s;
                const float lockH = 5.5f * s;
                const D2D1_RECT_F body = D2D1::RectF(
                    lockCenter.x - lockW * 0.5f, lockCenter.y - 0.5f * s,
                    lockCenter.x + lockW * 0.5f, lockCenter.y + lockH);
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(body, 1.5f * s, 1.5f * s), ink.Get());
                const float shackle = 2.5f * s;
                target->DrawLine(
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 0.5f * s),
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 3.0f * s),
                    ink.Get(), 1.2f * s);
                target->DrawLine(
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 3.0f * s),
                    D2D1::Point2F(lockCenter.x + (item.locked ? shackle : 0.5f * s),
                                  lockCenter.y - 3.0f * s),
                    ink.Get(), 1.2f * s);
                if (item.locked) {
                    target->DrawLine(
                        D2D1::Point2F(lockCenter.x + shackle, lockCenter.y - 3.0f * s),
                        D2D1::Point2F(lockCenter.x + shackle, lockCenter.y - 0.5f * s),
                        ink.Get(), 1.2f * s);
                }
            }
        }
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RackView::DrawNote(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                        const Theme& theme, float s, float swayAngle)
{
    if (item.note.empty() || !box.hasNote) {
        return;
    }
    const NotePalette palette = PaletteFor(item.noteColor, theme.dark);
    const D2D1_POINT_2F center =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);

    target->SetTransform(D2D1::Matrix3x2F::Rotation(box.angle + swayAngle, center));
    DrawShadow(target, box.note, 2.0f * s, 2.0f * s, 5.0f * s, 0.12f);

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    ComPtr<ID2D1SolidColorBrush> ink;
    ComPtr<ID2D1SolidColorBrush> fold;
    if (FAILED(target->CreateSolidColorBrush(palette.fill, fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.edge, edge.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.ink, ink.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.fold, fold.GetAddressOf()))) {
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }

    target->FillRectangle(box.note, fill.Get());
    target->DrawRectangle(box.note, edge.Get(), 1.0f * s);

    /* Folded bottom-right corner. */
    const float foldSize = 8.0f * s;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(path.GetAddressOf())) &&
        SUCCEEDED(path->Open(sink.GetAddressOf()))) {
        sink->BeginFigure(D2D1::Point2F(box.note.right - foldSize, box.note.bottom),
                          D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(box.note.right, box.note.bottom - foldSize));
        sink->AddLine(D2D1::Point2F(box.note.right, box.note.bottom));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target->FillGeometry(path.Get(), fold.Get());
    }

    if (noteFormat_) {
        const float padding = 5.0f * s;
        const float maxWidth = (box.note.right - box.note.left) - padding * 2.0f - foldSize;
        const float maxHeight = (box.note.bottom - box.note.top) - 4.0f * s;
        ComPtr<IDWriteTextLayout> textLayout;
        if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                item.note.c_str(), static_cast<UINT32>(item.note.size()),
                noteFormat_.Get(), maxWidth, maxHeight, textLayout.GetAddressOf()))) {
            target->DrawTextLayout(D2D1::Point2F(box.note.left + padding,
                                                 box.note.top + padding),
                                   textLayout.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

RackHit RackView::HitTest(float x, float y, float scale) const
{
    RackHit hit;
    const RackLayout& layout = layout_;
    const float s = std::max(0.5f, scale);

    const float lineHitBottom = layout.lineY + layout.sag + 8.0f * s;
    const bool lineArea = y >= 0.0f && y <= lineHitBottom &&
                          x >= layout.lineStart - 6.0f * s &&
                          x <= layout.lineEnd + 6.0f * s;
    if (!layout.expanded) {
        if (lineArea) {
            hit.part = RackPart::Line;
        }
        return hit;
    }

    for (int i = static_cast<int>(cards_.size()) - 1; i >= 0; --i) {
        const CardBox& box = cards_[static_cast<size_t>(i)];
        const D2D1_POINT_2F pivot =
            D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
        const D2D1_POINT_2F local = RotateAround(D2D1::Point2F(x, y), pivot, -box.angle);
        if (i == lastHovered_) {
            const D2D1_RECT_F lockRect = D2D1::RectF(
                box.card.left + 6.0f * s, box.card.top + 6.0f * s,
                box.card.left + 26.0f * s, box.card.top + 26.0f * s);
            if (rack::PointIn(lockRect, local.x, local.y)) {
                hit.part = RackPart::Lock;
                hit.index = i;
                return hit;
            }
            const D2D1_RECT_F closeRect = D2D1::RectF(
                box.card.right - 26.0f * s, box.card.top + 6.0f * s,
                box.card.right - 6.0f * s, box.card.top + 26.0f * s);
            if (rack::PointIn(closeRect, local.x, local.y)) {
                hit.part = RackPart::Close;
                hit.index = i;
                return hit;
            }
        }
        if (box.hasNote && rack::PointIn(box.note, local.x, local.y)) {
            hit.part = RackPart::Note;
            hit.index = i;
            return hit;
        }
        if (rack::PointIn(box.card, local.x, local.y)) {
            hit.part = RackPart::Card;
            hit.index = i;
            return hit;
        }
    }

    if (rack::PointIn(basketRect_, x, y)) {
        hit.part = RackPart::Basket;
        return hit;
    }
    if (rack::PointIn(envelopeRect_, x, y)) {
        hit.part = RackPart::Envelope;
        return hit;
    }
    if (lineArea) {
        hit.part = RackPart::Line;
        return hit;
    }
    if (rack::PointIn(layout.content, x, y)) {
        hit.part = RackPart::Background;
    }
    return hit;
}

const CardBox* RackView::Card(int index) const
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return nullptr;
    }
    return &cards_[static_cast<size_t>(index)];
}


