#pragma once

#include "Common.h"
#include "Item.h"
#include "Theme.h"

#include <map>

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
    /* 可见行 / 堆叠：容量按屏幕宽度和卡宽算出，滚动只移动整条内容。 */
    float rowLeft = 0.0f;
    float rowRight = 0.0f;
    float contentBase = 0.0f;
    float contentWidth = 0.0f;
    int capacity = 1;
    int visibleFirst = 0;
    int visibleCount = 0;
    int leftStackCount = 0;
    int rightStackCount = 0;
    /* 吸附组（占槽位的卡）数量：前面 count 张是吸附组，后面是自定义卡。 */
    int groupCount = 0;
    /* 两端叠组固定占位：左叠从屏幕左边距起、右叠到信封左边距止。 */
    float leftStackCenter = 0.0f;
    float rightStackCenter = 0.0f;
    /* 信封左边缘（自定义卡不允许停到它右边）。 */
    float accessoryLeft = 0.0f;
};

class RackView {
public:
    bool Init(Graphics* graphics);
    void Shutdown();
    void LoadAssets(ID2D1RenderTarget* target);

    RackLayout Layout(const std::vector<Item>& items, float maxWidth, float scale,
                      float expandAmount, float cardScale);
    /* 连续像素滚动：一格滚轮不再固定切一张卡。 */
    void ScrollByPixels(float delta);
    void ScrollToEnd() { followEnd_ = true; scrollTargetX_ = 1.0e9f; }
    bool ScrollSettled() const;
    bool AtScrollEnd() const;
    /* 正在滚动时不要实时把叠层提到最前，避免观感错位。 */
    bool IsScrolling() const;
    int Capacity() const { return layout_.capacity; }
    const CardBox* StackTop(bool left) const;
    int StackTopIndex(bool left) const;
    /* 叠在可见行后面的卡片也可以单独悬停/拖拽：返回它当前的显示框。 */
    bool IsStackIndex(int index) const;
    const CardBox* StackBoxForIndex(int index) const;
    const std::vector<CardBox>& StackBoxes(bool left) const
    {
        return left ? leftStackBoxes_ : rightStackBoxes_;
    }
    /* 滚轮/悬停判定：点是否落在绳子、可见卡或堆叠区域上。 */
    bool RackAreaContains(float x, float y) const;
    /* 与 Layout 完全一致的最终槽位：重排时先把槽位固定到目标位置，
       再用 rackOffsetX 从当前视觉位置缓动回 0，避免槽位弹簧和偏移叠加。 */
    std::vector<float> TargetLefts(const std::vector<Item>& items) const;
    /* 单张卡的槽位宽度（和 Layout/TargetLefts 同一套公式）。 */
    float CardWidth(const Item& item) const;
    /* 内容宽度变成 width 后，下一次 Layout 会用的居中基准（内容变窄时保持旧基准）。
       拖拽重排/让位在调用 Layout 之前需要它来算落点。 */
    float ProjectedContentBase(float contentWidth) const;
    void SnapSlotsTo(const std::vector<Item>& items, const std::vector<float>& lefts);
    /* 内容坐标 -> 屏幕坐标（拖动重排时换算落点用）。 */
    float ScreenLeftForContentLeft(float contentLeft) const
    {
        return layout_.contentBase + contentLeft - scrollX_;
    }
    float ContentXForScreenLeft(float screenLeft) const
    {
        return screenLeft - layout_.contentBase + scrollX_;
    }
    int GroupCount() const { return layout_.groupCount; }
    float AccessoryLeft() const { return layout_.accessoryLeft; }
    float ScreenLeftForRowX(float rowX) const { return layout_.rowLeft + rowX - scrollX_; }
    float RowXForScreenLeft(float screenLeft) const { return screenLeft - layout_.rowLeft + scrollX_; }
    float ContentWidthForItems(const std::vector<Item>& items) const;
    float ScrollX() const { return scrollX_; }
    float ScrollTargetX() const { return scrollTargetX_; }
    /* 拖动中把指定卡片的槽位固定在给定内容位置，避免弹簧把它带走。 */
    void SnapSlotForId(int id, float contentLeft);
    /* 让下一次槽位目标变化不要 140ms 停顿（拖动让位要跟手）。 */
    void SkipHold() { skipHoldOnce_ = true; }
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
    /* 屏幕上实际画这张卡的位置：可见行用真实槽位，堆叠顶层用堆叠位。 */
    const CardBox* DisplayCard(int index) const;
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
    bool HasSliding() const { return sliding_ || envelopeSliding_ || scrollSliding_; }

    bool HasFalling() const {
        for (const auto& item : removed_) {
            if (!item.toBasket) return true;
        }
        return false;
    }
    void SetDragging(int index) { dragging_ = index; }
    /* 横向拖动中的卡片画在最上面：拖过邻居时不会被压住。 */
    void SetFreeDragging(int index) { freeDrag_ = index; }
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

    struct BadgeCacheEntry {
        ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };
    std::map<int, BadgeCacheEntry> badgeCache_;
    ID2D1RenderTarget* badgeCacheOwner_ = nullptr;
    float badgeCacheScale_ = 0.0f;
    bool badgeCacheDark_ = false;

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
    int freeDrag_ = -1;
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
        /* 进入叠组后的位置也用弹簧过渡：叠组内每加入一张，旧卡只挪一个夹子位。 */
        float pilePos = 0.0f;
        float pileVelocity = 0.0f;
        bool pilePrimed = false;
        /* 滚轮滚动时的自然摆动：角度弹簧。 */
        float swingPos = 0.0f;
        float swingVel = 0.0f;
        bool swingPrimed = false;
    };
    std::vector<SlotSpring> slots_;
    uint64_t lastLayoutMs_ = 0;
    bool sliding_ = false;

    /* 内容坐标里的全局滚动：滚轮只改 scrollTargetX_，所有卡片共享同一个
       scrollX_ 弹簧，卡与卡的间距永远由 contentX 决定。 */
    int scrollFirst_ = 0;
    float scrollX_ = 0.0f;
    float scrollVelocity_ = 0.0f;
    float scrollTargetX_ = 0.0f;
    float maxScrollX_ = 0.0f;
    /* 吸附组整体居中位置也走弹簧：插入/搬走时整排是滑过去，不是瞬移。 */
    float baseShown_ = 0.0f;
    float baseVel_ = 0.0f;
    bool basePrimed_ = false;
    /* 上一帧内容宽度：删除卡片时内容变窄，不要重新居中（否则整排先右移）。 */
    float prevContentWidth_ = 0.0f;
    /* 内容变窄后保持原来的居中基准，直到内容再变宽（加卡）才重新居中。 */
    bool baseAnchored_ = false;
    uint64_t scrollHoldUntilMs_ = 0;
    bool scrollSliding_ = false;
    bool scrollPrimed_ = false;
    bool followEnd_ = true;
    int leftStackCount_ = 0;
    int rightStackCount_ = 0;
    std::vector<CardBox> leftStackBoxes_;
    std::vector<CardBox> rightStackBoxes_;
    std::vector<int> leftStackIndices_;
    std::vector<int> rightStackIndices_;

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
