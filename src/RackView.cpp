#include "RackView.h"
#include "NotePalette.h"
#include "Graphics.h"
#include "Util.h"

namespace {

constexpr float kLineY = 14.0f;
constexpr float kCardTop = 42.0f;
/* 卡片规格：照片最大 136x104，卡片宽 150，卡片间距 24。 */
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
/* 截图/便签飞上绳子的时长，Layout、Draw 和 HasArrival 共用，
   保证交接只发生在同一帧。 */
constexpr float kArrivalSeconds = 0.65f;

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
    /* 上绳落位用的弹簧：响应 0.42 s、阻尼 0.72、初始偏移 -46。 */
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
    /* 每张位图按接近实际显示尺寸的两倍解码。让 D2D 在绘制时把 340px 的
       信封缩到 70px，边缘会出现明显的台阶；这里先用 WIC Fant 缩好。 */
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
            const bool textCard = items[i].kind == 1 || items[i].kind == 3 ||
                                  (items[i].kind == 2 && items[i].thumbPath.empty());
            if (textCard) {
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
    /* 收起时卡片停在窗口上方，展开时滑进视野。 */
    const float slide =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) * -(maxCardHeight + 30.0f * s);
    float x = kOuterPad * s + (photoWidth - total) * 0.5f;
    const float y = cardTop + slide;

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
    // 收起状态只保留顶部那条热区。留一整块看不见的高窗口在那儿，
    // 会把桌面上的点击吃掉。
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
                    float basketHover, float paper0, float cardHover, int cardHoverIndex,
                    int hovered, int pressed, int historyCount, int clipboardCount)
{
    if (!target || !graphics_) {
        return;
    }
    const float s = scale_;
    const RackLayout& layout = layout_;
    const bool expanded = expandAmount > 0.01f;
    lastHovered_ = hovered;

    /* 卡片开始绘制之前，先退休掉已经飞完的飞行。原来在 DrawArrival 里清索引，
       会让新卡片整整一帧不画：drawGroup 因为「还在飞」跳过它，DrawArrival
       又因为「已经落位」跳过它，结果图片/便签刚落到绳上时闪一帧。 */
    if (arrivalIndex_ >= 0 && (arrivalIndex_ >= static_cast<int>(cards_.size()) ||
                               !HasArrival())) {
        arrivalIndex_ = -1;
    }

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
    DrawAccessories(target, theme, s, expandAmount, basketHover, paper0, historyCount,
                    clipboardCount);

    /* 摆动用一次轻的角冲量做激励：周期约 0.95 s、阻尼比很低，
       比直接衰减的正弦更慢更软。 */
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
                // 晃动：前 0.30 s 缓出到新角度，之后用弹簧（刚度 38、阻尼 2.4）回落到 0。
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

    /* 阴影必须全部先画，否则后画的卡片阴影会盖到前一张的白边上。 */
    for (size_t i = 0; i < cards_.size() && i < items.size(); ++i) {
        if (static_cast<int>(i) == arrivalIndex_) continue;
        if (static_cast<int>(i) == editingCard_) continue;
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
        /* 编辑中的便签：卡片本体交给编辑器动画，绳子上只留夹子。 */
        const bool editing = static_cast<int>(i) == editingCard_;
        if (!editing) {
            const bool hotLock = hoverPart_ == static_cast<int>(RackPart::Lock) &&
                                 hoverPartIndex_ == static_cast<int>(i);
            const bool hotClose = hoverPart_ == static_cast<int>(RackPart::Close) &&
                                  hoverPartIndex_ == static_cast<int>(i);
            DrawCard(target, items[i], box, s, sway, hoverAmount, isHovered,
                     static_cast<int>(i) == pressed, hotLock, hotClose);
        }
        DrawPin(target, box, theme, s);
        if (box.hasNote && !editing) {
            DrawNote(target, items[i], box, theme, s, sway);
        }
        if (fadeLayer) {
            target->PopLayer();
        }
    };

    /* 先画没被悬停的卡片，最后画悬停的那张，让它压在邻居上面；
       夹子在卡片之后画，才像是真的夹住了照片上沿。 */
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

    DrawRemoved(target, s);
    if (arrivalIndex_ >= 0 && arrivalIndex_ < static_cast<int>(items.size())) {
        DrawArrival(target, items[static_cast<size_t>(arrivalIndex_)], s);
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
           static_cast<float>(rack::NowMs() - arrivalStartMs_) / 1000.0f < kArrivalSeconds;
}

void RackView::DrawArrival(ID2D1RenderTarget* target, const Item& item, float s)
{
    if (arrivalIndex_ < 0 || arrivalIndex_ >= static_cast<int>(cards_.size())) {
        arrivalIndex_ = -1;
        return;
    }
    const float age = static_cast<float>(rack::NowMs() - arrivalStartMs_) / 1000.0f;
    const CardBox& targetBox = cards_[static_cast<size_t>(arrivalIndex_)];
    /* 任何情况下都不能直接返回而不画：Draw() 已经把这卡从常规批次里跳过了，
       这里 return 就会让它闪掉一帧。 */
    const float raw = rack::Clamp(age / kArrivalSeconds, 0.0f, 1.0f);
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
    DrawCard(target, item, box, s, spin, 0.0f, false, false);
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

void RackView::DrawRemoved(ID2D1RenderTarget* target, float s)
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
            // 从绳上掉落：0.55 s、520 像素、缓入三次方，额外加一点旋转和线性淡出，
            // 不做成自由落体那种加速下坠。
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
            DrawCard(target, it->item, box, s, spin, 0.0f, false, false);
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
        DrawCard(target, it->item, box, s, spin, 0.0f, false, false);
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
    // 各主题下绳子的颜色与线宽。
    D2D1_COLOR_F ropeColor{};
    D2D1_COLOR_F ropeHighlight{};
    float ropeWidth = 1.6f;
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 1:  // 奶白
        ropeColor = D2D1::ColorF(0.68f, 0.69f, 0.69f);
        ropeHighlight = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        ropeWidth = 1.9f;
        break;
    case 2:  // 木棉
        ropeColor = D2D1::ColorF(0.73f, 0.65f, 0.54f);
        ropeHighlight = D2D1::ColorF(0.92f, 0.87f, 0.79f);
        ropeWidth = 2.3f;
        break;
    case 3:  // 深沉
        ropeColor = D2D1::ColorF(0.34f, 0.36f, 0.39f);
        ropeHighlight = D2D1::ColorF(0.58f, 0.61f, 0.64f);
        ropeWidth = 1.65f;
        break;
    default:  // 银灰
        ropeColor = D2D1::ColorF(0.54f, 0.59f, 0.65f);
        ropeHighlight = D2D1::ColorF(0.78f, 0.81f, 0.85f);
        ropeWidth = 1.6f;
        break;
    }
    const float y = layout.lineY + retract;
    const float sag = layout.sag;

    /* 绳子首尾各 8% 做淡出，看起来像从屏幕外延伸过来，
       而不是两个硬邦邦的断点。 */
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
    /* 夹子的槽正好落在绳子上，再让夹身向下压住足够多的白边，
       才像真的夹住了照片。 */
    DrawClipAt(target, cx, box.pinTop + lineRetract_ - clipHeight * kClipSlotRatio,
               theme, s);
}

void RackView::DrawClipAt(ID2D1RenderTarget* target, float cx, float top, const Theme& theme,
                          float s, float heightDips)
{
    /* 夹子是整块圆角金属片（9x26），不是两瓣的晾衣夹：
       轮廓简单，做一道柔和的高光即可。 */
    const float width = 8.0f * s;
    const float height = heightDips * s;
    const D2D1_RECT_F rect =
        D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);

    // 夹子用主题的金属色，而不是固定一套钢色。
    D2D1_COLOR_F peg[3]{};
    D2D1_COLOR_F pegEdge{};
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 3:  // 深沉，石墨灰 #575E66
        peg[0] = D2D1::ColorF(0.43f, 0.46f, 0.49f);
        peg[1] = D2D1::ColorF(0.34f, 0.37f, 0.40f);
        peg[2] = D2D1::ColorF(0.27f, 0.30f, 0.32f);
        pegEdge = D2D1::ColorF(0.26f, 0.29f, 0.31f);
        break;
    case 2:  // 木棉，亚麻米 #DFC5A3（浅榉木）
        peg[0] = D2D1::ColorF(0.92f, 0.84f, 0.74f);
        peg[1] = D2D1::ColorF(0.87f, 0.77f, 0.64f);
        peg[2] = D2D1::ColorF(0.80f, 0.69f, 0.54f);
        pegEdge = D2D1::ColorF(0.68f, 0.56f, 0.41f);
        break;
    case 1:  // 奶白，瓷白 #F3F0E8
        peg[0] = D2D1::ColorF(0.98f, 0.97f, 0.95f);
        peg[1] = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        peg[2] = D2D1::ColorF(0.91f, 0.89f, 0.85f);
        pegEdge = D2D1::ColorF(0.76f, 0.75f, 0.71f);
        break;
    default:  // 银灰，经典银 #DCE3EC
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
        // 木夹子的后夹片，稍微偏右一点。
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
            // 一道很淡的木纹，不要画成条纹木纹。
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

    /* 中间那道槽当作夹子的弹簧轴。 */
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

void RackView::DrawAccessories(ID2D1RenderTarget* target, const Theme& theme, float s,
                               float expandAmount, float basketHover, float paper0,
                               int historyCount, int clipboardCount)
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
    /* 两个数量角标共用同一条基线，即使信封和篮子的高度不同。 */
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

        /* 图层顺序（由下往上）：
           背面 → 夹子 → 信纸 → 正面。带主题的背面本身已经包含纸张，
           所以单独的「信纸」图层只给旧版素材兜底。 */
        target->DrawBitmap(envelopeBack->bitmap.Get(), dest, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        DrawClipAt(target, envelopeCenter,
                   ropeYAt(envelopeCenter) - 24.0f * s * kClipSlotRatio, theme, s);

        // 所有主题共用同一张信纸，换的只是它下面的信封配色。
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
        /* 弹簧值要保留符号：离开时它会冲过 0 再荡回来，
           篮子才像自然停住。 */
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
    /* 三位数以上要显示成 999+，胶囊跟着文字加宽；两位数以内保持原来的尺寸。 */
    wchar_t label[8]{};
    if (count > 999) {
        wcscpy_s(label, L"999+");
    } else {
        swprintf_s(label, L"%d", count);
    }

    float textWidth = 0.0f;
    if (graphics_ && graphics_->dwrite()) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                label, static_cast<UINT32>(wcslen(label)), badgeFormat_.Get(), 120.0f * s,
                18.0f * s, layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textWidth = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }
    const float width = std::max(28.0f * s, textWidth + 10.0f * s);
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

void RackView::DrawTypeBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card, int kind,
                             float s)
{
    /* 只画图标本身，不加底托，避免看起来像一个可以点的按钮。 */
    const float size = 18.0f * s;
    const D2D1_RECT_F area = D2D1::RectF(card.left + 7.0f * s,
                                         card.bottom - 7.0f * s - size,
                                         card.left + 7.0f * s + size,
                                         card.bottom - 7.0f * s);
    ComPtr<ID2D1SolidColorBrush> ink;
    ComPtr<ID2D1SolidColorBrush> outline;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                                             ink.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f),
                                             outline.GetAddressOf()))) {
        return;
    }
    const float edge = 0.9f * s;
    if (kind == 2) {
        /* 摄像机：机身 + 右侧镜头三角。 */
        const float bx = area.left + 3.5f * s;
        const float by = area.top + 6.2f * s;
        const float bw = 7.5f * s;
        const float bh = 5.6f * s;
        const D2D1_ROUNDED_RECT body =
            D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), 1.2f * s, 1.2f * s);
        target->FillRoundedRectangle(body, ink.Get());
        target->DrawRoundedRectangle(body, outline.Get(), edge);
        ComPtr<ID2D1PathGeometry> lens;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(lens.GetAddressOf())) && lens) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(lens->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(D2D1::Point2F(bx + bw + 0.8f * s, by + 1.6f * s),
                                  D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(bx + bw + 4.2f * s, by - 0.4f * s));
                sink->AddLine(D2D1::Point2F(bx + bw + 4.2f * s, by + bh + 0.4f * s));
                sink->AddLine(D2D1::Point2F(bx + bw + 0.8f * s, by + bh - 1.6f * s));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->FillGeometry(lens.Get(), ink.Get());
                target->DrawGeometry(lens.Get(), outline.Get(), edge);
            }
        }
    } else {
        /* 文件夹：主体 + 左上角标签。 */
        const float bx = area.left + 3.5f * s;
        const float by = area.top + 5.6f * s;
        const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
            D2D1::RectF(bx, by + 1.6f * s, bx + 11.0f * s, by + 8.4f * s), 1.2f * s,
            1.2f * s);
        const D2D1_ROUNDED_RECT tab = D2D1::RoundedRect(
            D2D1::RectF(bx, by, bx + 4.6f * s, by + 3.0f * s), 1.2f * s, 1.2f * s);
        target->FillRoundedRectangle(body, ink.Get());
        target->DrawRoundedRectangle(body, outline.Get(), edge);
        target->FillRoundedRectangle(tab, ink.Get());
        target->DrawRoundedRectangle(tab, outline.Get(), edge);
    }
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
                        float s, float swayAngle, float hoverAmount, bool hovered,
                        bool pressed, bool hotLock, bool hotClose)
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
    /* 以夹子为原点缩放：照片轻微抬起，夹子本身不动。 */
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

    /* 录屏无封面 / 文件：纯色卡片 + 文字 + 左下角类型图标。 */
    const bool isNoteCard = item.path.empty() && !item.note.empty();
    const bool plainCard = item.kind == 3 || (item.kind == 2 && !item.thumb.valid());
    if (plainCard) {
        const D2D1_COLOR_F cardColor =
            item.kind == 3 ? D2D1::ColorF(0.24f, 0.30f, 0.42f)
                           : D2D1::ColorF(0.31f, 0.29f, 0.25f);
        ComPtr<ID2D1SolidColorBrush> plain;
        ComPtr<ID2D1SolidColorBrush> ink;
        if (SUCCEEDED(target->CreateSolidColorBrush(cardColor, plain.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.97f),
                                                    ink.GetAddressOf()))) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                         fill.Get());
            /* 和图片卡一样留一圈同样粗细的白框，再填纯色内容。 */
            const D2D1_RECT_F inner = rack::Inset(rect, kFrame * s, kFrame * s);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(inner, std::max(2.0f * s, radius - kFrame * s),
                                  std::max(2.0f * s, radius - kFrame * s)),
                plain.Get());
            const std::wstring& text = item.label.empty() ? item.note : item.label;
            if (noteFormat_ && !text.empty()) {
                target->DrawText(
                    text.c_str(), static_cast<UINT32>(text.size()), noteFormat_.Get(),
                    rack::Inset(inner, 10.0f * s, 12.0f * s), ink.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            DrawTypeBadge(target, inner, item.kind, s);
        }
    } else {
        if (!isNoteCard) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                         fill.Get());
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
                    {0.0f, NotePaperTop()},
                    {1.0f, NotePaperBottom()},
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
            if (SUCCEEDED(target->CreateSolidColorBrush(NotePaperInk(),
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
    }
    if (item.kind == 2) {
        DrawTypeBadge(target, rect, item.kind, s);
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
        ComPtr<ID2D1SolidColorBrush> circle;
        ComPtr<ID2D1SolidColorBrush> ink;
        const Theme ui = Theme::MenuStyle(uiDark_);
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 0.94f),
                circle.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(ui.fg, ink.GetAddressOf()))) {
            /* 悬停反馈只改颜色，不改大小、也不描边：常态是透明度偏低的实心圆片，
               鼠标压上去就变实（锁定键回到卡片色，关闭键整颗变红）。 */
            auto glass = [&](const D2D1_POINT_2F& c, bool hot, bool danger) {
                const float radius = size * 0.5f;
                if (hot) {
                    if (danger) {
                        circle->SetColor(D2D1::ColorF(0.898f, 0.282f, 0.302f, 0.98f));
                    } else {
                        circle->SetColor(D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 1.0f));
                    }
                } else {
                    circle->SetColor(D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 0.70f));
                }
                target->FillEllipse(D2D1::Ellipse(c, radius, radius), circle.Get());
            };
            glass(closeCenter, hotClose, true);
            ink->SetColor(hotClose ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                   : D2D1::ColorF(ui.fg.r, ui.fg.g, ui.fg.b, 0.85f));
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

            glass(lockCenter, hotLock, false);
            ink->SetColor(D2D1::ColorF(ui.fg.r, ui.fg.g, ui.fg.b, hotLock ? 1.0f : 0.85f));
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
    const NotePalette palette = NotePaletteFor(item.noteColor, theme.dark);
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

    /* 右下角折角。 */
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
        if (i == editingCard_) continue; /* 编辑中的卡片已经藏起来了 */
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
        {
            /* 夹子：卡片顶部中间、绳子附近的一小块。图片/录屏点它把内容贴到桌面；
               便签没有可贴的东西——命中照样吃掉（不然会漏到下面那个窗口），
               但光标和点击反馈在 RackWindow 里按卡片类型过滤。 */
            const float cx = (box.card.left + box.card.right) * 0.5f;
            const D2D1_RECT_F clipRect =
                D2D1::RectF(cx - 10.0f * s, box.pinTop - 10.0f * s, cx + 10.0f * s,
                            box.card.top + 2.0f * s);
            if (rack::PointIn(clipRect, local.x, local.y)) {
                hit.part = RackPart::Clip;
                hit.index = i;
                return hit;
            }
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


