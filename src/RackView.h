#pragma once

#include "Common.h"
#include "Item.h"
#include "Theme.h"

class Graphics;

enum class RackPart { None, Line, Background, Card, Note, Close, Lock, Basket, Envelope, Clip };

struct RackHit {
    RackPart part = RackPart::None;
    int index = -1;
};

struct CardBox {
    D2D1_RECT_F card{};
    D2D1_RECT_F image{};
    D2D1_RECT_F note{};
    bool hasNote = false;
    float pinTop = 0.0f;
    float angle = 0.0f;
};

struct RackLayout {
    float width = 0.0f;
    float height = 0.0f;
    float lineY = 0.0f;
    float lineStart = 0.0f;
    float lineEnd = 0.0f;
    float sag = 0.0f;
    D2D1_RECT_F content{};
    bool expanded = false;
};

class RackView {
public:
    bool Init(Graphics* graphics);
    void Shutdown();
    void LoadAssets(ID2D1RenderTarget* target);

    RackLayout Layout(const std::vector<Item>& items, float maxWidth, float scale,
                      float expandAmount, float cardScale);
    void ShowCopyToast(int index, uint64_t untilMs);
    void ClearCopyToast();
    void SetUiDark(bool dark) { uiDark_ = dark; }

    void Draw(ID2D1RenderTarget* target, const std::vector<Item>& items, const Theme& theme,
              float expandAmount, float swaySeconds, float basketHover, float paper0,
              float paper1, float paper2, float cardHover, int cardHoverIndex, int hovered,
              int pressed, int historyCount, int clipboardCount);

    RackHit HitTest(float x, float y, float scale) const;
    const CardBox* Card(int index) const;
    float DefaultGap() const { return 24.0f * scale_ * cardScale_; }
    D2D1_POINT_2F EnvelopeBadgeCenter() const;
    void BeginRemove(int index, const Item& item, bool toBasket = true);
    bool HasRemoved() const { return !removed_.empty(); }
    void BeginArrival(int index);
    void SetArrivalStart(float x, float y, float w = 0.0f, float h = 0.0f);
    bool HasArrival() const;
    bool HasFalling() const {
        for (const auto& item : removed_) {
            if (!item.toBasket) return true;
        }
        return false;
    }
    void SetDragging(int index) { dragging_ = index; }

    const RackLayout& layout() const { return layout_; }
    bool hasLayout() const { return !cards_.empty(); }

private:
    void EnsureFormats(float scale);
    void DrawShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                    float dy, float spread, float alpha);
    void DrawCardShadow(ID2D1RenderTarget* target, const CardBox& box, float scale,
                        bool hovered);
    void DrawCard(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                  const Theme& theme, float scale, float swayAngle, float hoverAmount,
                  bool hovered, bool pressed);
    void DrawNote(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                  const Theme& theme, float scale, float swayAngle);
    void DrawPin(ID2D1RenderTarget* target, const CardBox& box, const Theme& theme,
                 float scale);
    /* 卡片左下角的类型角标：2=录屏，3=文件。 */
    void DrawTypeBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card, int kind,
                       float scale);
    void DrawClipAt(ID2D1RenderTarget* target, float centerX, float clipTop, const Theme& theme,
                    float scale, float heightDips = 24.0f);
    void DrawAccessories(ID2D1RenderTarget* target, const std::vector<Item>& items,
                         const Theme& theme, float scale, float expandAmount,
                         float basketHover, float paper0, float paper1, float paper2,
                         int historyCount, int clipboardCount);
    void DrawCountBadge(ID2D1RenderTarget* target, float centerX, float top, int count,
                        float scale);
    void DrawCopyToast(ID2D1RenderTarget* target, float scale);
    void DrawArrival(ID2D1RenderTarget* target, const Item& item, const Theme& theme,
                     float scale);
    void LoadIconFont();
    void BuildIconGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out);
    void DrawIcon(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                  D2D1_POINT_2F center, float size, ID2D1Brush* brush);
    void DrawLine(ID2D1RenderTarget* target, const Theme& theme, float scale,
                  float expandAmount);
    void DrawRemoved(ID2D1RenderTarget* target, const Theme& theme, float scale);

    Graphics* graphics_ = nullptr;
    ComPtr<IDWriteTextFormat> noteFormat_;
    ComPtr<IDWriteTextFormat> placeholderFormat_;
    ComPtr<IDWriteTextFormat> badgeFormat_;
    ComPtr<IDWriteTextFormat> toastFormat_;
    ComPtr<IDWriteFontFace> iconFace_;
    ComPtr<ID2D1PathGeometry> iconLock_;
    ComPtr<ID2D1PathGeometry> iconUnlock_;
    ComPtr<ID2D1PathGeometry> iconClose_;
    ImageBitmap basket_;
    ImageBitmap basketTheme_[4];
    ImageBitmap envelope_;
    ImageBitmap envelopeFront_;
    ImageBitmap letter_;
    ImageBitmap envelopeThemeBack_[4];
    ImageBitmap envelopeThemeFront_[4];
    bool assetsAttempted_ = false;
    struct RemovedCard {
        Item item;
        CardBox box;
        uint64_t removedMs = 0;
        bool toBasket = true;
    };
    std::vector<CardBox> cards_;
    std::vector<RemovedCard> removed_;
    int dragging_ = -1;
    RackLayout layout_;
    D2D1_RECT_F basketRect_{};
    D2D1_RECT_F envelopeRect_{};
    float scale_ = 1.0f;
    float formatScale_ = 0.0f;
    float lineRetract_ = 0.0f;
    float cardScale_ = 1.0f;
    int lastHovered_ = -1;
    int arrivalIndex_ = -1;
    uint64_t arrivalStartMs_ = 0;
    D2D1_POINT_2F arrivalStart_{};
    float arrivalW_ = 0.0f;
    float arrivalH_ = 0.0f;
    bool arrivalStartSet_ = false;
    bool uiDark_ = false;
    int copyToastIndex_ = -1;
    uint64_t copyToastUntil_ = 0;
};
