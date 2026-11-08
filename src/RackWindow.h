#pragma once

#include "Common.h"
#include "ContextMenu.h"
#include "LayeredWindow.h"
#include "PreviewWindow.h"
#include "RackView.h"

class Plugin;

class RackWindow : public LayeredWindow {
public:
    RackWindow();
    ~RackWindow() override;

    bool Init(Plugin* plugin, Graphics* graphics);
    void Shutdown();

    void OnItemsChanged(bool added);
    void OnConfigChanged();
    void OnThemeChanged();
    void RenderNow();

    void Expand(bool peek);
    void ToggleVisibility();
    void Collapse();
    /* 使某张卡片轻微晃动，用于提示「已经挂在这儿了」。 */
    void ShakeCard(int index);
    /* 从左往右检查最小间距，挨太近就用缓动推开。 */
    void NormalizeSpacing(float ms);
    /* 拖拽期间让绳子对鼠标透明：OLE 每次移动都要做命中测试，
       一块全宽分层窗口挡在指针下面会明显拖慢拖拽。 */
    void SetDragTransparent(bool on);
    /* 展开期间监听屏幕顶边点击和卡片右键。 */
    void SetTopBarWatch(bool enabled);
    bool IsTopBarPoint(POINT screen) const;
    int CardIndexAtScreen(POINT screen) const;
    static LRESULT CALLBACK TopBarHookProc(int code, WPARAM wParam, LPARAM lParam);
    static RackWindow* topBarOwner_;
    bool rightCardDown_ = false;
    uint64_t rightCardDownAt_ = 0;
    HHOOK topBarHook_ = nullptr;
    bool expanded() const { return expand_ > 0.5f; }

    void Reposition();
    void DismissPreview();
    void RefreshPreview();

    HMONITOR monitor() const { return monitor_; }
    const RECT& workArea() const { return workArea_; }
    float scale() const { return scale_; }

protected:
    void BeforeDraw(ID2D1RenderTarget* target) override;
    void Paint(ID2D1RenderTarget* target) override;
    void AfterDraw(HDC dc, void* bits, int width, int height) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    enum TimerId {
        kTimerPoll = 1,
        kTimerAnim,
        kTimerClick,
        kTimerLongPress,
        kTimerEditWatch,
        kTimerEditStart,
    kTimerCopyToast,
    kTimerFakeCursor,
    };

    enum MenuId {
        kMenuCopy = 1,
        kMenuOpen,
        kMenuEdit,
        kMenuEditColor,
        kMenuEditPaint,
        kMenuSaveAs,
        kMenuShowInFolder,
        kMenuNote,
        kMenuRemove,
        kMenuSettings,
        kMenuCollapse,
        kMenuBatch,
        kMenuBatchLeft,
        kMenuBatchRight,
        kMenuBatchKeep,
        kMenuBatchAll,
        kMenuRecycle,
        kMenuColorBase = 100,
    };

    void Tick();
    void Animate();
    void EnsureVisible();
    /* 读轻雪的「停用历史记录」，变了就重新布局并重绘。 */
    void RefreshHistoryDisabled();
    void PositionForMonitor(HMONITOR monitor);
    void StartAnimation(float target);
    void UpdateHover();
    void UpdatePreview();
    void UpdateAccessoryHover(const RackHit& hit);
    void UpdateClickThrough();
    void SetClickThrough(bool enabled);
    void CheckEditing();
    /* 卡片/便签条的精确命中形状（含旋转与悬停放大），extra 是外扩像素。 */
    bool PointInCardShape(const CardBox& box, float x, float y, float extra) const;
    bool PointInNoteShape(const CardBox& box, float x, float y, float extra) const;
    /* 指针下这个像素是否真的画了东西。分层窗口只看 alpha，
       低级鼠标钩子用它把空白区域让给下面的程序。 */
    bool PointHittable(POINT screen) const;
    /* 图片/文件被删除后，把绳子上的卡片自动取下。 */
    void CheckMissingFiles();

    RackHit HitAtCursor() const;
    int HitIndexAtCursor(RackPart* outPart = nullptr) const;
    bool CursorOverLine() const;
    bool CursorOverContent() const;
    bool PointOverPreview(const POINT& screen) const;

    void ShowPreviewFor(int index);
    void HidePreviewNow();
    void CopyItem(int index);
    void OpenItem(int index);
    /* 链接正文单击：打开默认浏览器（双击不重复开）。 */
    void OpenCardLink(int index);
    /* 颜色卡单个色块/hex/rgb：只复制这个颜色。part 0=色块 1=hex 2=rgb。 */
    void CopyCardColor(int index, int colorIndex, int part);
    /* 颜色卡滚轮：返回 true 表示这次滚轮已被色卡吃掉。 */
    bool ScrollColorCard(int index, int delta);
    /* 右键「编辑颜色」：改这张卡的头部强调色（便签是整张纸的主色）。 */
    void PickCardColor(int index);
    void EditItemInPaint(int index);
    void SaveItemAs(int index);
    void RemoveItem(int index, bool toBasket = true);
    void EditNote(int index);
    void QueueEditNote(int index);
    void SetNoteColor(int index, int color);
    void ShowItemMenu(int index, POINT screen);
    void ShowBatchMenu(int index, POINT screen);
    static void MenuSelected(void* user, int id);
    void ShowGeneralMenu(POINT screen);
    void HandleMenuCommand(int index, int command);

    bool IsFullscreenOn(HMONITOR monitor) const;
    RECT MonitorWorkArea(HMONITOR monitor) const;
    HMONITOR CursorMonitor() const;
    D2D1_RECT_F CardScreenRect(int index) const;

    Plugin* plugin_ = nullptr;
    RackView view_;
    PreviewWindow preview_;
    ContextMenu menu_;
    ContextMenu batchMenu_;
    bool menuOpen_ = false;
    int menuIndex_ = -1;
    bool hostMenuWasOpen_ = false;

    HMONITOR monitor_ = nullptr;
    RECT workArea_{};
    float scale_ = 1.0f;

    float expand_ = 0.0f;
    float animFrom_ = 0.0f;
    float animTo_ = 0.0f;
    uint64_t animStart_ = 0;
    uint64_t revealStart_ = 0;
    uint64_t swingUntil_ = 0;
    /* 单张卡片晃动的时间线，和整体摆动分开。 */
    uint64_t shakeUntil_ = 0;
    float basketHover_ = 0.0f;
    float basketHoverTarget_ = 0.0f;
    float basketHoverVelocity_ = 0.0f;
    float envelopeHover_ = 0.0f;
    float envelopeHoverTarget_ = 0.0f;
    float envelopeHoverVelocity_ = 0.0f;
    float paperPos_ = 0.0f;
    float paperVelocity_ = 0.0f;
    uint64_t envelopeEnterMs_ = 0;
    float cardHover_ = 0.0f;
    float cardHoverTarget_ = 0.0f;
    float cardHoverVelocity_ = 0.0f;
    int cardHoverIndex_ = -1;
    uint64_t lastAnimMs_ = 0;
    bool clickThrough_ = false;
    bool animating_ = false;
    bool expanding_ = false;
    uint64_t peekUntil_ = 0;

    uint64_t lastInside_ = 0;
    uint64_t hoverLineSince_ = 0;
    bool lineHover_ = false;
    /* 刚在顶栏点过一下：指针离开顶边之前不再自动展开。 */
    bool topBarSuppressed_ = false;

    int hovered_ = -1;
    uint64_t hoveredSince_ = 0;
    int pressed_ = -1;
    int pressItemId_ = -1;
    RackPart pressedPart_ = RackPart::None;
    int pressColorIndex_ = -1;
    int pressColorPart_ = 0;
    uint64_t lastLinkOpenMs_ = 0;
    int lastLinkOpenIndex_ = -1;
    bool mouseDown_ = false;
    bool dragStarted_ = false;
    bool cardDragging_ = false;
    float cardDragStartOffset_ = 0.0f;
    uint64_t lastCardDragPaint_ = 0;
    bool hideWhenRemovalsDone_ = false;
    bool hotkeyPinned_ = false;
    struct OffsetAnim {
        int id = 0;
        float from = 0.0f;
        float to = 0.0f;
        uint64_t start = 0;
        float ms = 180.0f;
    };
    std::vector<OffsetAnim> offsetAnims_;
    int pendingAdjustId_ = -1;
    uint64_t pendingAdjustAt_ = 0;
    uint64_t menuCloseGraceUntil_ = 0;
    bool menuActionPending_ = false;
    /* 点夹子贴图：等卡片下落动画走一半，再触发原生贴图。 */
    enum class PendingPin { None, Image, Text, Files };
    PendingPin pendingPinKind_ = PendingPin::None;
    std::wstring pendingPinPath_;
    std::wstring pendingPinText_;
    std::vector<std::wstring> pendingPinPaths_;
    POINT pendingPinAnchor_{};
    uint64_t pendingPinAt_ = 0;

    void FinishCardDrag(int id);
    void NormalizeLoadedSpacing();
    bool normalizeSpacingPending_ = false;
    bool longPressFired_ = false;
    POINT pressPoint_{};

    int clickItemId_ = -1;
    int lastHoverPart_ = -1;
    int lastHoverPartIndex_ = -1;
    int lastHoverColorIndex_ = -1;
    int lastHoverColorPart_ = -1;
    int pendingEditIndex_ = -1;
    int lastCardSize_ = -1;
    float cardScaleValue_ = 1.0f;
    float cardScaleFrom_ = 1.0f;
    float cardScaleTarget_ = 1.0f;
    uint64_t cardScaleAnimStart_ = 0;
    bool cardScaleAnimating_ = false;
    /* 便签编辑器开着时绳子保持展开，不自动收回。 */
    bool editingNote_ = false;

    int previewIndex_ = -1;
    uint64_t previewHideAt_ = 0;
    RECT previewAnchor_{};

    bool hiddenForFullscreen_ = false;
    bool needsLayout_ = false;
    bool editingWatchActive_ = false;
    bool historyDisabled_ = false;
    /* 每次展开只查一次文件是否还在；不做定时轮询。 */
    bool missingChecked_ = false;
    /* 显示方式：0 悬停展开、1 快捷键切换、2 常驻显示。 */
    int lastDisplayMode_ = 0;
    /* 快捷键模式下手动隐藏：隐藏后不再被悬停/配置轮询自动拉起。 */
    bool manuallyHidden_ = false;
    bool shownBeforeFullscreen_ = false;
    /* 预览期间（设置页的「晾衣架外观」）收起会被宿主 pin 住；
       等预览结束再补一次收起。 */
    bool collapseAfterPreview_ = false;
};
