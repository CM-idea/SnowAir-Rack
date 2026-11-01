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
    /* 展开期间监听屏幕顶边的点击：点一下就收回。 */
    void SetTopBarWatch(bool enabled);
    bool IsTopBarPoint(POINT screen) const;
    static LRESULT CALLBACK TopBarHookProc(int code, WPARAM wParam, LPARAM lParam);
    static RackWindow* topBarOwner_;
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
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    enum TimerId {
        kTimerPoll = 1,
        kTimerAnim,
        kTimerClick,
        kTimerLongPress,
        kTimerEditWatch,
    kTimerCopyToast,
    };

    enum MenuId {
        kMenuCopy = 1,
        kMenuOpen,
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
    void PositionForMonitor(HMONITOR monitor);
    void StartAnimation(float target);
    void UpdateHover();
    void UpdatePreview();
    void UpdateAccessoryHover(const RackHit& hit);
    void UpdateClickThrough(const RackHit& hit);
    void SetClickThrough(bool enabled);
    void CheckEditing();

    RackHit HitAtCursor() const;
    int HitIndexAtCursor(RackPart* outPart = nullptr) const;
    bool CursorOverLine() const;
    bool CursorOverContent() const;
    bool PointOverPreview(const POINT& screen) const;

    void ShowPreviewFor(int index);
    void HidePreviewNow();
    void CopyItem(int index);
    void OpenItem(int index);
    void EditItemInPaint(int index);
    void SaveItemAs(int index);
    void RemoveItem(int index, bool toBasket = true);
    void EditNote(int index);
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
    };
    std::vector<OffsetAnim> offsetAnims_;
    int pendingAdjustId_ = -1;
    uint64_t pendingAdjustAt_ = 0;
    /* 点夹子贴图：等卡片下落动画走一半，再触发原生贴图。 */
    std::wstring pendingPinPath_;
    uint64_t pendingPinAt_ = 0;

    void FinishCardDrag(int id);
    void NormalizeLoadedSpacing();
    bool normalizeSpacingPending_ = false;
    bool longPressFired_ = false;
    POINT pressPoint_{};

    int clickItemId_ = -1;
    int lastHoverPart_ = -1;
    int lastHoverPartIndex_ = -1;
    /* 便签编辑器开着时绳子保持展开，不自动收回。 */
    bool editingNote_ = false;

    int previewIndex_ = -1;
    uint64_t previewHideAt_ = 0;
    RECT previewAnchor_{};

    bool hiddenForFullscreen_ = false;
    bool needsLayout_ = false;
    bool editingWatchActive_ = false;
};
