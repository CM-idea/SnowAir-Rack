#pragma once

#include "Common.h"
#include "Item.h"
#include "Theme.h"

class Graphics;

enum class RackPart {
    None,
    Line,
    Background,
    Card,
    Note,
    Close,
    Lock,
    Basket,
    Envelope,
    Clip,
    CardLink,  /* 链接卡的正文 URL */
    CardColor  /* 颜色卡的色块 / hex / rgb 文本 */
};

struct RackHit {
    RackPart part = RackPart::None;
    int index = -1;
    int colorIndex = -1; /* CardColor：第几个颜色 */
    int colorPart = 0;   /* CardColor：0=色块，1=hex，2=rgb */
};

struct ColorRowBox {
    int index = 0;
    D2D1_RECT_F row{};
    D2D1_RECT_F swatch{};
    D2D1_RECT_F hex{};
    D2D1_RECT_F rgb{};
};

struct CardBox {
    int id = 0;
    D2D1_RECT_F card{};
    D2D1_RECT_F image{};
    D2D1_RECT_F note{};
    D2D1_RECT_F header{};
    D2D1_RECT_F body{};
    D2D1_RECT_F linkText{};
    std::vector<ColorRowBox> colorRows;
    card::Type cardType = card::Type::Image;
    bool flat = false;
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
    /* 与 Layout 完全一致的最终槽位：重排时先把槽位钉到目标，
       再用 rackOffsetX 从当前视觉位置缓动回 0，避免槽位弹簧和偏移叠加。 */
    std::vector<float> TargetLefts(const std::vector<Item>& items) const;
    void SnapSlotsTo(const std::vector<Item>& items, const std::vector<float>& lefts);
    void ShowCopyToast(int index, uint64_t untilMs);
    void ClearCopyToast();
    /* 单击某个 hex/rgb 后让那一行短暂变绿，作为“已复制”的微提示。 */
    void ShowColorFlash(int itemId, int colorIndex, int part, uint64_t untilMs);
    void ClearColorFlash();
    void SetUiDark(bool dark) { uiDark_ = dark; }
    /* 轻雪「停用历史记录」：篮子连同角标一起隐藏，信封右移占住原来的位置。 */
    void SetHistoryDisabled(bool disabled);

    void Draw(ID2D1RenderTarget* target, const std::vector<Item>& items, const Theme& theme,
              float expandAmount, float swaySeconds, float basketHover, float paper0,
              float cardHover, int cardHoverIndex, int hovered, int pressed,
              int historyCount, int clipboardCount);
    /* 收起一段时间后把展开时才用到的素材位图放掉：常驻内存回落到接近刚启动的水平，
       下次展开由 LoadAssets 重新加载。 */
    void ReleaseCachedImages();

    /* 拖拽时跟随指针的卡片图：用和绳子上完全相同的画法渲染到一张 DIB，
       调用方负责 DeleteObject。 */
    HBITMAP MakeDragGhost(const Item& item, int width, int height, SIZE* sizeOut);
    /* 颜色卡拖单个颜色：一张纯色块卡片的 ghost。 */
    HBITMAP MakeColorGhost(const card::Rgba& color, int width, int height, SIZE* sizeOut);

    RackHit HitTest(float x, float y, float scale) const;
    const CardBox* Card(int index) const;
    float DefaultGap() const { return 24.0f * scale_ * cardScale_; }
    D2D1_POINT_2F EnvelopeBadgeCenter() const;
    void BeginRemove(int index, const Item& item, bool toBasket = true);
    bool HasRemoved() const { return !removed_.empty(); }
    void BeginArrival(int index);
    void SetArrivalStart(float x, float y, float w = 0.0f, float h = 0.0f);
    bool HasArrival() const;
    /* 掉落/出场动画实际需要的高度（相对窗口顶部）；用它决定窗口多高，
       不要为了动画把整个工作区都做成窗口，那会让每帧上传的位图非常大。 */
    float AnimationHeight() const;
    /* 卡片位置是否还在弹簧里往目标位置收（用于决定要不要继续跑动画）。 */
    bool HasSliding() const { return sliding_ || envelopeSliding_; }

    bool HasFalling() const {
        for (const auto& item : removed_) {
            if (!item.toBasket) return true;
        }
        return false;
    }
    void SetDragging(int index) { dragging_ = index; }
    /* 松手时把被拖卡的弹簧起点钉在当前位置：重排后从落点弹回新槽位，
       不会先闪回旧槽位再跳过去。 */
    void PrepareCardRelease(int index);
    /* 颜色卡滚轮只在白卡内部生效，不包含 20% 外圈。 */
    bool ColorContentContains(int index, float x, float y) const;
    /* 光标压在卡片哪个控件上（锁定/关闭），用来做悬停反馈。 */
    void SetHoverPart(int part, int index) {
        hoverPart_ = part;
        hoverPartIndex_ = index;
    }
    /* 颜色卡悬停到第几个颜色的哪一段（0 色块 / 1 hex / 2 rgb）。 */
    void SetHoverColor(int colorIndex, int colorPart) {
        hoverColorIndex_ = colorIndex;
        hoverColorPart_ = colorPart;
    }
    /* 正在编辑器里的便签：卡片先藏起来，只留绳子上的夹子。 */
    void SetEditingCard(int index) { editingCard_ = index; }

    const RackLayout& layout() const { return layout_; }
    bool hasLayout() const { return !cards_.empty(); }
    const CardBox* CardAt(int index) const
    {
        return index >= 0 && index < static_cast<int>(cards_.size()) ? &cards_[index] : nullptr;
    }

private:
    void EnsureFormats(float scale);
    void DrawShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                    float dy, float spread, float alpha);
    void DrawCardShadow(ID2D1RenderTarget* target, const CardBox& box, float scale,
                        bool hovered);
    void DrawCard(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                  float scale, float swayAngle, float hoverAmount, bool hovered,
                  bool pressed, bool hotLock = false, bool hotClose = false,
                  bool hotLink = false, bool hotColor = false);
    void DrawNote(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                  const Theme& theme, float scale, float swayAngle);
    /* 新式非图片卡：白底 + 顶部纯色头 + 20% 类型色外圈。 */
    void DrawFlatCard(ID2D1RenderTarget* target, const Item& item, const D2D1_RECT_F& rect,
                      float scale, bool hotLink = false, bool hotColor = false);
    /* 便签纸卡：方角、右下折角、头部 #FFE087。 */
    void DrawPaperCard(ID2D1RenderTarget* target, const Item& item, const D2D1_RECT_F& rect,
                       float scale);
    void DrawFlatIcon(ID2D1RenderTarget* target, card::Type type, D2D1_POINT_2F center,
                      float size, ID2D1Brush* brush, float stroke);
    void DrawPin(ID2D1RenderTarget* target, const CardBox& box, const Theme& theme,
                 float scale);
    /* 文件卡片左下角：文件夹图标 + 类型/数量文字。 */
    void DrawFileBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card, const Item& item,
                       float scale);

    /* 卡片左下角的类型角标：2=录屏，3=文件。 */
    void DrawTypeBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card, int kind,
                       float scale);
    void DrawClipAt(ID2D1RenderTarget* target, float centerX, float clipTop, const Theme& theme,
                    float scale, float heightDips = 24.0f);
    void DrawAccessories(ID2D1RenderTarget* target, const Theme& theme, float scale,
                         float expandAmount, float basketHover, float paper0, int historyCount,
                         int clipboardCount);
    float EnvelopeCenterX() const;
    void DrawCountBadge(ID2D1RenderTarget* target, float centerX, float top, int count,
                        float scale);
    void DrawCopyToast(ID2D1RenderTarget* target, float scale);
    void DrawArrival(ID2D1RenderTarget* target, const Item& item, float scale);
    void LoadIconFont();
    void BuildIconGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out);
    void DrawIcon(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                  D2D1_POINT_2F center, float size, ID2D1Brush* brush);
    void DrawLine(ID2D1RenderTarget* target, const Theme& theme, float scale,
                  float expandAmount);
    void DrawRemoved(ID2D1RenderTarget* target, float scale);

    Graphics* graphics_ = nullptr;
    ComPtr<IDWriteTextFormat> noteFormat_;
    ComPtr<IDWriteTextFormat> placeholderFormat_;
    ComPtr<IDWriteTextFormat> badgeFormat_;
    ComPtr<IDWriteTextFormat> fileTagFormat_;
    ComPtr<IDWriteTextFormat> toastFormat_;
    ComPtr<IDWriteTextFormat> cardTitleFormat_;
    ComPtr<IDWriteTextFormat> cardMetaFormat_;
    ComPtr<IDWriteTextFormat> cardBodyFormat_;
    ComPtr<IDWriteTextFormat> cardColorHexFormat_;
    ComPtr<IDWriteTextFormat> cardColorRgbFormat_;
    ComPtr<IDWriteTextFormat> cardColorHexBoldFormat_;
    ComPtr<IDWriteTextFormat> cardColorRgbBoldFormat_;
    ComPtr<IDWriteTextFormat> cardFileNameFormat_;
    ComPtr<IDWriteTextFormat> cardFileNameWrapFormat_;
    ComPtr<IDWriteFontFace> iconFace_;
    ComPtr<ID2D1PathGeometry> iconLock_;
    ComPtr<ID2D1PathGeometry> iconUnlock_;
    ComPtr<ID2D1PathGeometry> iconClose_;
    ComPtr<ID2D1PathGeometry> iconFolder_;
    ComPtr<ID2D1PathGeometry> iconLink_;
    ComPtr<ID2D1PathGeometry> iconText_;
    ComPtr<ID2D1PathGeometry> iconColor_;
    ComPtr<ID2D1PathGeometry> iconVideo_;
    ComPtr<ID2D1PathGeometry> iconMusic_;
    ImageBitmap basket_;
    ImageBitmap basketTheme_[4];
    /* 篮子旋转走 CPU 重采样：软件光栅画「旋转的位图」会按整像素吸附，
       钩子看起来一跳一跳的。这里自己把篮子按角度转成一张小位图，
       再用无变换的贴图贴上去，钩子就能稳稳钉在枢轴上。 */
    bool EnsureBasketPixels(int themeIndex, UINT pixelSize);
    bool UpdateBasketWarp(ID2D1RenderTarget* target, int themeIndex, float angleDeg, float s,
                          float ppd);
    std::wstring assetsDir_;
    std::vector<BYTE> basketSrcPixels_;
    UINT basketSrcW_ = 0;
    UINT basketSrcH_ = 0;
    int basketSrcTheme_ = -1;
    UINT basketSrcPixelSize_ = 0;
    ComPtr<ID2D1Bitmap> basketWarpBitmap_;
    std::vector<BYTE> basketWarpPixels_;
    UINT basketWarpW_ = 0;
    UINT basketWarpH_ = 0;
    float basketWarpAngle_ = 1e9f;
    float basketWarpPpd_ = 0.0f;
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
    bool historyDisabled_ = false;
    /* 信封换槽位时和卡片共用同一套弹簧：位置逐帧跟随目标。 */
    float envelopeX_ = 0.0f;
    float envelopeXVelocity_ = 0.0f;
    bool envelopeXPrimed_ = false;
    bool envelopeSliding_ = false;
    /* 每张卡片的「显示位置」：目标位置由布局按索引算出，实际位置用弹簧平滑过渡。
       移除一张卡片后，后面卡片的目标位置整体左移，弹簧使其平滑过渡到新位置。 */
    struct SlotSpring {
        int id = 0;
        float pos = 0.0f;
        float velocity = 0.0f;
        float target = 0.0f;
        uint64_t holdUntil = 0;
        bool primed = false;
    };
    std::vector<SlotSpring> slots_;
    uint64_t lastLayoutMs_ = 0;
    bool sliding_ = false;

    float scale_ = 1.0f;
    float formatScale_ = 0.0f;
    float formatCardScale_ = 0.0f;
    float lineRetract_ = 0.0f;
    float cardScale_ = 1.0f;
    int lastHovered_ = -1;
    int hoverPart_ = 0;
    int hoverPartIndex_ = -1;
    int hoverColorIndex_ = -1;
    int hoverColorPart_ = -1;
    bool skipHoldOnce_ = false;
    int editingCard_ = -1;
    int arrivalIndex_ = -1;
    uint64_t arrivalStartMs_ = 0;
    D2D1_POINT_2F arrivalStart_{};
    float arrivalW_ = 0.0f;
    float arrivalH_ = 0.0f;
    bool arrivalStartSet_ = false;
    bool uiDark_ = false;
    int copyToastIndex_ = -1;
    uint64_t copyToastUntil_ = 0;
    int copyFlashItemId_ = -1;
    int copyFlashColorIndex_ = -1;
    int copyFlashPart_ = 0;
    uint64_t copyFlashUntil_ = 0;
};
