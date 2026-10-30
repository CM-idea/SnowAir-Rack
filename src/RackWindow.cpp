#include "RackWindow.h"

#include "Clipboard.h"
#include "Config.h"
#include "DragDrop.h"
#include "NoteEditor.h"
#include "NoteStore.h"
#include "Plugin.h"
#include "Util.h"

#include <commdlg.h>
#include <shellapi.h>

namespace {

constexpr int kPollIntervalMs = 50;
constexpr int kAnimDurationMs = 170;
constexpr int kPreviewDelayMs = 320;
constexpr int kPreviewHideDelayMs = 180;
constexpr int64_t kPeekDurationMs = 3000;

bool RecyclePath(const std::wstring& path)
{
    if (path.empty()) return false;
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/* PicRope's SpringMotion, integrated from the current state. */
void StepSpring(float& position, float& velocity, float target, float dt, float response,
                float damping)
{
    if (dt <= 0.0f) {
        return;
    }
    dt = std::min(dt, 0.05f);
    const float natural = 2.0f * 3.14159265f / response;
    const float decay = natural * damping;
    const float damped = natural * std::sqrt(1.0f - damping * damping);
    const float offset = position - target;
    const float sine = std::sin(damped * dt);
    const float cosine = std::cos(damped * dt);
    const float envelope = std::exp(-decay * dt);
    const float coefficient = (velocity + decay * offset) / damped;
    position = target + envelope * (offset * cosine + coefficient * sine);
    velocity = envelope *
               (velocity * cosine - (decay * coefficient + damped * offset) * sine);
    if (std::abs(position - target) < 0.0015f && std::abs(velocity) < 0.01f) {
        position = target;
        velocity = 0.0f;
    }
}

D2D1_RECT_F AddRect(const D2D1_RECT_F& rect, const POINT& offset)
{
    return D2D1::RectF(rect.left + static_cast<float>(offset.x),
                       rect.top + static_cast<float>(offset.y),
                       rect.right + static_cast<float>(offset.x),
                       rect.bottom + static_cast<float>(offset.y));
}

bool NearlySameRect(const RECT& a, const RECT& b)
{
    return std::abs(a.left - b.left) <= 2 && std::abs(a.top - b.top) <= 2 &&
           std::abs(a.right - b.right) <= 2 && std::abs(a.bottom - b.bottom) <= 2;
}

} /* namespace */

RackWindow::RackWindow() = default;

RackWindow::~RackWindow()
{
    Shutdown();
}

bool RackWindow::Init(Plugin* plugin, Graphics* graphics)
{
    plugin_ = plugin;
    if (!plugin_ || !graphics) {
        return false;
    }
    if (!view_.Init(graphics)) {
        RackLog(L"RackWindow: view initialization failed");
        return false;
    }
    if (!preview_.Init(graphics)) {
        RackLog(L"RackWindow: preview initialization failed");
        return false;
    }
    if (!menu_.Init(graphics)) {
        RackLog(L"RackWindow: menu initialization failed");
        return false;
    }
    if (!batchMenu_.Init(graphics)) {
        RackLog(L"RackWindow: batch menu initialization failed");
        return false;
    }
    if (!Create(graphics, util::GetDllModule(), L"SnowAirRackWindow", L"SnowAir Rack")) {
        return false;
    }
    monitor_ = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    workArea_ = MonitorWorkArea(monitor_);
    scale_ = util::ScaleForMonitor(monitor_);
    SetDpiScale(scale_);
    lastInside_ = rack::NowMs();
    normalizeSpacingPending_ = !plugin_->items().empty();
    SetTimer(hwnd(), kTimerPoll, kPollIntervalMs, nullptr);
    return true;
}

void RackWindow::Shutdown()
{
    if (hwnd()) {
        KillTimer(hwnd(), kTimerPoll);
        KillTimer(hwnd(), kTimerAnim);
        KillTimer(hwnd(), kTimerClick);
        KillTimer(hwnd(), kTimerLongPress);
        KillTimer(hwnd(), kTimerEditWatch);
    }
    editingWatchActive_ = false;
    menu_.Close();
    batchMenu_.Close();
    menu_.Destroy();
    batchMenu_.Destroy();
    preview_.Destroy();
    view_.Shutdown();
    Destroy();
    plugin_ = nullptr;
}

void RackWindow::OnItemsChanged(bool added)
{
    DismissPreview();
    if (!plugin_ || plugin_->items().empty()) {
        menu_.Close();
        batchMenu_.Close();
        menuOpen_ = false;
        if (view_.HasRemoved()) {
            // Let the last card finish flying into the basket before folding.
            hideWhenRemovalsDone_ = true;
            EnsureVisible();
            if (expand_ < 0.9f) {
                expanding_ = true;
                StartAnimation(1.0f);
            }
            if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            Render();
            return;
        }
        if (!plugin_->config().autoRetract) {
            // 常驻模式下即使没有卡片也保留衣架本体，而不是把窗口整个收掉。
            hideWhenRemovalsDone_ = false;
            EnsureVisible();
            if (expand_ < 0.9f) {
                Expand(false);
            } else {
                Render();
            }
            return;
        }
        expanding_ = false;
        expand_ = 0.0f;
        animating_ = false;
        if (hwnd()) {
            KillTimer(hwnd(), kTimerAnim);
        }
        Hide();
        return;
    }
    hideWhenRemovalsDone_ = false;

    if (!monitor_) {
        monitor_ = CursorMonitor();
    }
    if (added) {
        view_.BeginArrival(static_cast<int>(plugin_->items().size()) - 1);
    }
    PositionForMonitor(monitor_);
    if (added) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd(), &cursor);
        const Item& incoming = plugin_->items().back();
        if (incoming.captureW > 0 && incoming.captureH > 0) {
            POINT tl{incoming.captureX, incoming.captureY};
            POINT br{incoming.captureX + incoming.captureW,
                     incoming.captureY + incoming.captureH};
            ScreenToClient(hwnd(), &tl);
            ScreenToClient(hwnd(), &br);
            view_.SetArrivalStart((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f,
                                  static_cast<float>(br.x - tl.x),
                                  static_cast<float>(br.y - tl.y));
        } else {
            view_.SetArrivalStart(static_cast<float>(cursor.x),
                                  static_cast<float>(cursor.y));
        }
        if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
        Expand(true);
    } else {
        EnsureVisible();
        Render();
    }
}

void RackWindow::OnConfigChanged()
{
    if (!plugin_) {
        return;
    }
    if (!plugin_->config().enabled) {
        menu_.Close();
        batchMenu_.Close();
        menuOpen_ = false;
        DismissPreview();
        Hide();
        expanding_ = false;
        expand_ = 0.0f;
        animating_ = false;
        if (hwnd()) {
            KillTimer(hwnd(), kTimerAnim);
        }
        return;
    }
    if (!monitor_) {
        monitor_ = CursorMonitor();
    }
    PositionForMonitor(monitor_);
    EnsureVisible();
    /* 关闭「自动收回」后衣架常驻：任何配置变化都把它重新展开。 */
    if (!plugin_->config().autoRetract && expand_ < 0.9f) {
        Expand(false);
    }
    Render();
}

void RackWindow::OnThemeChanged()
{
    RefreshPreview();
    Render();
}

void RackWindow::RenderNow()
{
    Render();
}

void RackWindow::Expand(bool peek)
{
    if (!plugin_) {
        return;
    }
    const uint64_t now = rack::NowMs();
    if (peek) {
        peekUntil_ = now + static_cast<uint64_t>(kPeekDurationMs);
    } else {
        peekUntil_ = 0;
    }
    lastInside_ = now;
    revealStart_ = now;
    swingUntil_ = now + 2200;
    expanding_ = true;
    if (!hiddenForFullscreen_) {
        EnsureVisible();
    }
    StartAnimation(1.0f);
    if (hwnd()) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

void RackWindow::NormalizeLoadedSpacing()
{
    if (!plugin_) {
        return;
    }
    struct Entry {
        float left = 0.0f;
        int index = -1;
    };
    std::vector<Entry> order;
    for (int i = 0; i < static_cast<int>(plugin_->items().size()); ++i) {
        if (const CardBox* box = view_.Card(i)) {
            order.push_back({box->card.left, i});
        }
    }
    std::sort(order.begin(), order.end(),
              [](const Entry& a, const Entry& b) { return a.left < b.left; });
    const float gap = view_.DefaultGap();
    float previousRight = 0.0f;
    bool first = true;
    bool changed = false;
    for (const auto& entry : order) {
        const CardBox* box = view_.Card(entry.index);
        if (!box) continue;
        const float width = box->card.right - box->card.left;
        float left = box->card.left;
        if (!first && left - previousRight < gap) {
            const float shift = gap - (left - previousRight);
            plugin_->items()[entry.index].rackOffsetX += shift;
            left += shift;
            changed = true;
        }
        previousRight = left + width;
        first = false;
    }
    if (changed) {
        plugin_->SaveItems();
        needsLayout_ = true;
    }
}

void RackWindow::FinishCardDrag(int id)
{
    if (!plugin_) {
        return;
    }
    const int index = plugin_->IndexById(id);
    if (index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    Item& dragged = plugin_->items()[index];
    const CardBox* draggedBox = view_.Card(index);
    if (!draggedBox) {
        return;
    }

    struct Entry {
        float left = 0.0f;
        int index = -1;
    };
    std::vector<Entry> order;
    for (int i = 0; i < static_cast<int>(plugin_->items().size()); ++i) {
        if (const CardBox* box = view_.Card(i)) {
            order.push_back({box->card.left, i});
        }
    }
    std::sort(order.begin(), order.end(),
              [](const Entry& a, const Entry& b) { return a.left < b.left; });
    int position = -1;
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        if (order[i].index == index) {
            position = i;
            break;
        }
    }
    if (position < 0) {
        return;
    }

    const float gap = view_.DefaultGap();
    std::vector<std::pair<int, float>> changes;
    // Reflow strictly left-to-right. Each card gets its own default gap from
    // the card already placed before it; this cannot leave an overlap behind
    // no matter how many cards were moved during the drag.
    float previousRight = 0.0f;
    bool first = true;
    for (const auto& entry : order) {
        const CardBox* box = view_.Card(entry.index);
        if (!box) continue;
        float left = box->card.left;
        const float width = box->card.right - box->card.left;
        if (!first) {
            const float actualGap = left - previousRight;
            if (actualGap < gap) {
                const float shift = gap - actualGap;
                changes.push_back({entry.index, shift});
                left += shift;
            }
        }
        previousRight = left + width;
        first = false;
    }

    float draggedTarget = dragged.rackOffsetX;
    const float width = draggedBox->card.right - draggedBox->card.left;
    const float baseLeft = draggedBox->card.left - dragged.rackOffsetX;
    const float margin = 24.0f * scale_;
    const float maxLeft = view_.layout().width - width - margin;
    const float wantedLeft = baseLeft + draggedTarget;
    draggedTarget += rack::Clamp(wantedLeft, margin, std::max(margin, maxLeft)) - wantedLeft;
    // Only the no-conflict case gets the soft magnetic centre.
    if (changes.empty() && std::abs(draggedTarget) <= 36.0f * scale_) {
        draggedTarget = 0.0f;
    }
    if (std::abs(draggedTarget - dragged.rackOffsetX) > 0.35f) {
        changes.push_back({index, draggedTarget - dragged.rackOffsetX});
    }

    offsetAnims_.clear();
    const uint64_t now = rack::NowMs();
    for (const auto& change : changes) {
        if (change.first < 0 || change.first >= static_cast<int>(plugin_->items().size())) {
            continue;
        }
        Item& item = plugin_->items()[change.first];
        const float target = item.rackOffsetX + change.second;
        if (std::abs(target - item.rackOffsetX) < 0.35f) {
            continue;
        }
        offsetAnims_.push_back({item.id, item.rackOffsetX, target, now});
        item.shakeStartMs = now;
        item.shakeAmp = 2.2f;
    }
    if (!offsetAnims_.empty()) {
        swingUntil_ = std::max(swingUntil_, now + 1250);
        if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

void RackWindow::ToggleVisibility()
{
    if (!plugin_) {
        return;
    }
    if (expanding_ || expanded()) {
        hotkeyPinned_ = false;
        Collapse();
        return;
    }
    EnsureVisible();
    // Keep it down until the pointer actually visits the rack; only then does
    // leaving it start the normal auto-hide countdown.
    hotkeyPinned_ = true;
    Expand(false);
}

void RackWindow::Collapse()
{
    if (plugin_ && plugin_->PreviewPinned()) {
        return;
    }
    if (view_.HasRemoved()) {
        return;
    }
    SetClickThrough(true);
    hotkeyPinned_ = false;
    expanding_ = false;
    peekUntil_ = 0;
    swingUntil_ = 0;
    hovered_ = -1;
    cardHoverTarget_ = 0.0f;
    basketHoverTarget_ = 0.0f;
    envelopeHoverTarget_ = 0.0f;
    lastAnimMs_ = 0;
    DismissPreview();
    StartAnimation(0.0f);
}

void RackWindow::StartAnimation(float target)
{
    if (!hwnd()) {
        expand_ = target;
        return;
    }
    if (animating_ && std::abs(animTo_ - target) < 0.001f) {
        return;
    }
    if (!animating_ && std::abs(expand_ - target) < 0.001f) {
        expand_ = target;
        return;
    }
    animFrom_ = expand_;
    animTo_ = target;
    animStart_ = rack::NowMs();
    animating_ = true;
    SetTimer(hwnd(), kTimerAnim, 16, nullptr);
}

void RackWindow::Animate()
{
    if (!hwnd()) {
        return;
    }
    const uint64_t now = rack::NowMs();
    if (!pendingPinPath_.empty() && now >= pendingPinAt_) {
        const std::wstring path = std::move(pendingPinPath_);
        pendingPinPath_.clear();
        if (plugin_) plugin_->PinImageFile(path);
    }
    if (pendingAdjustId_ >= 0 && now >= pendingAdjustAt_) {
        const int id = pendingAdjustId_;
        pendingAdjustId_ = -1;
        FinishCardDrag(id);
    }
    if (!offsetAnims_.empty() && plugin_) {
        bool finished = false;
        for (auto it = offsetAnims_.begin(); it != offsetAnims_.end();) {
            const int index = plugin_->IndexById(it->id);
            if (index < 0) {
                it = offsetAnims_.erase(it);
                finished = true;
                continue;
            }
            const float t = rack::Clamp(
                static_cast<float>(now - it->start) / 180.0f, 0.0f, 1.0f);
            Item& item = plugin_->items()[index];
            if (t >= 1.0f) {
                item.rackOffsetX = it->to;
                it = offsetAnims_.erase(it);
                finished = true;
            } else {
                item.rackOffsetX =
                    rack::Lerp(it->from, it->to, rack::EaseOutCubic(t));
                ++it;
            }
        }
        if (finished && offsetAnims_.empty()) {
            plugin_->SaveItems();
        }
    }
    if (animating_) {
        const float t = static_cast<float>(now - animStart_) /
                        static_cast<float>(kAnimDurationMs);
        if (t >= 1.0f) {
            expand_ = animTo_;
            animating_ = false;
        } else {
            expand_ = rack::Lerp(animFrom_, animTo_, rack::EaseOutCubic(t));
        }
    }

    float dt = 0.0f;
    if (lastAnimMs_ != 0 && now > lastAnimMs_ && now - lastAnimMs_ < 100) {
        dt = static_cast<float>(now - lastAnimMs_) / 1000.0f;
    }
    lastAnimMs_ = now;

    StepSpring(basketHover_, basketHoverVelocity_, basketHoverTarget_, dt, 0.50f, 0.50f);
    StepSpring(cardHover_, cardHoverVelocity_, cardHoverTarget_, dt, 0.30f, 0.60f);

    bool paperSettled = true;
    for (int i = 0; i < 3; ++i) {
        const float target =
            (envelopeHoverTarget_ > 0.5f &&
             now >= envelopeEnterMs_ + static_cast<uint64_t>(i * 45))
                ? 1.0f
                : 0.0f;
        StepSpring(paperPos_[i], paperVelocity_[i], target, dt, 0.38f, 0.80f);
        if (std::abs(paperPos_[i] - target) >= 0.003f) {
            paperSettled = false;
        }
    }

    Reposition();
    Render();

    const bool hoverSettled =
        std::abs(basketHover_ - basketHoverTarget_) < 0.003f &&
        std::abs(cardHover_ - cardHoverTarget_) < 0.003f && paperSettled;
    if (!animating_ && offsetAnims_.empty() && pendingAdjustId_ < 0 &&
        now >= swingUntil_ && hoverSettled && !view_.HasRemoved() &&
        !view_.HasArrival() && pendingPinPath_.empty()) {
        if (hideWhenRemovalsDone_ && plugin_->items().empty()) {
            hideWhenRemovalsDone_ = false;
            if (plugin_->config().autoRetract) {
                Hide();
                expanding_ = false;
                expand_ = 0.0f;
                KillTimer(hwnd(), kTimerAnim);
                return;
            }
            // 常驻模式：卡片移除动画结束后保留空衣架，不隐藏窗口。
            EnsureVisible();
        }
        KillTimer(hwnd(), kTimerAnim);
    }
    if (!animating_ && expand_ <= 0.001f) {
        hovered_ = -1;
        DismissPreview();
        SetClickThrough(true);
    }
}

void RackWindow::Reposition()
{
    if (!hwnd() || !plugin_) {
        return;
    }
    if (!monitor_) {
        monitor_ = CursorMonitor();
    }
    const RECT work = MonitorWorkArea(monitor_);
    if (work.right <= work.left || work.bottom <= work.top) {
        return;
    }
    workArea_ = work;
    scale_ = util::ScaleForMonitor(monitor_);
    SetDpiScale(scale_);

    const float s = scale_;
    const float maxWidth = static_cast<float>(work.right - work.left);
    const RackLayout layout = view_.Layout(plugin_->items(), maxWidth, s, expand_,
                                           plugin_->config().CardScale());
    const int width = std::max(1, static_cast<int>(std::lround(layout.width)));
    int height = std::max(1, static_cast<int>(std::lround(layout.height)));
    if (view_.HasFalling() || view_.HasArrival()) {
        // Falling/arriving cards use the full work area so the layered window
        // never clips their flight.
        height = std::max(height, static_cast<int>(work.bottom - work.top));
    }
    const int x = work.left;
    const int y = work.top;

    if (width != this->width() || height != this->height()) {
        MoveTo(x, y, width, height);
    } else {
        RECT current{};
        GetWindowRect(hwnd(), &current);
        if (current.left != x || current.top != y) {
            MoveTo(x, y, width, height);
        }
    }
    needsLayout_ = false;
}

void RackWindow::PositionForMonitor(HMONITOR monitor)
{
    if (!monitor) {
        return;
    }
    monitor_ = monitor;
    workArea_ = MonitorWorkArea(monitor);
    scale_ = util::ScaleForMonitor(monitor);
    if (!plugin_) {
        return;
    }
    if (!plugin_->items().empty()) {
        Reposition();
    }
}

void RackWindow::EnsureVisible()
{
    if (!hwnd() || !plugin_ || !plugin_->config().enabled || hiddenForFullscreen_) {
        return;
    }
    if (!visible()) {
        if (!monitor_) {
            monitor_ = CursorMonitor();
        }
        Reposition();
        Render();
        ShowNoActivate();
    }
}

void RackWindow::Tick()
{
    if (!plugin_ || !hwnd()) {
        return;
    }
    plugin_->SyncHostTheme();
    if (menuOpen_) {
        return;
    }
    const bool hostMenuOpen = plugin_->MenuOpen();
    if (hostMenuWasOpen_ && !hostMenuOpen) {
        hostMenuWasOpen_ = false;
        // Also unhooks PinMenu's low-level mouse hook if it outlived the
        // visible popup; otherwise it can keep swallowing desktop clicks.
        plugin_->CloseMenu();
        // 常驻模式不因菜单关闭而自动收回。
        if (plugin_->config().autoRetract) {
            Collapse();
        }
        Render();
        return;
    }
    if (hostMenuOpen) {
        hostMenuWasOpen_ = true;
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        // The popup is its own window. Let clicks outside it pass through to
        // the desktop instead of the full-width rack window swallowing them.
        SetClickThrough(true);
        lastInside_ = rack::NowMs();
        return;
    }
    if (plugin_->ClipboardOpen()) {
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        /* The panel is anchored at the envelope and overlaps the rope strip.
           Let the rack ignore the mouse entirely while it is open, otherwise
           the rope window steals clicks meant for the panel (the search bar
           '+' is right inside the overlap). */
        SetClickThrough(true);
        lastInside_ = rack::NowMs();
        return;
    }

    if (plugin_->PreviewPinned()) {
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        lastInside_ = rack::NowMs();
    }

    if (!plugin_->config().enabled) {
        if (visible()) {
            Hide();
        }
        if (preview_.showing()) {
            DismissPreview();
        }
        expanding_ = false;
        expand_ = 0.0f;
        animating_ = false;
        KillTimer(hwnd(), kTimerAnim);
        hiddenForFullscreen_ = false;
        return;
    }

    const HMONITOR cursorMonitor = CursorMonitor();
    if (cursorMonitor && cursorMonitor != monitor_ && !mouseDown_ && !dragStarted_ &&
        expand_ < 0.01f && !preview_.showing()) {
        monitor_ = cursorMonitor;
        PositionForMonitor(monitor_);
        EnsureVisible();
    }

    if (hiddenForFullscreen_) {
        const bool stillFullscreen = IsFullscreenOn(monitor_);
        if (stillFullscreen) {
            return;
        }
        hiddenForFullscreen_ = false;
        EnsureVisible();
        Reposition();
        if (!plugin_->config().autoRetract && expand_ < 0.9f) {
            Expand(false);
        }
        Render();
    } else if (IsFullscreenOn(monitor_)) {
        DismissPreview();
        Hide();
        hiddenForFullscreen_ = true;
        expanding_ = false;
        expand_ = 0.0f;
        animating_ = false;
        KillTimer(hwnd(), kTimerAnim);
        return;
    }

    if (!visible()) {
        EnsureVisible();
    }

    UpdateHover();
    UpdatePreview();
    CheckEditing();

    if (normalizeSpacingPending_) {
        normalizeSpacingPending_ = false;
        NormalizeLoadedSpacing();
    }

    if (needsLayout_) {
        Reposition();
        Render();
    }
}

bool RackWindow::CursorOverLine() const
{
    if (!hwnd() || !visible()) {
        return false;
    }
    POINT point{};
    if (!GetCursorPos(&point)) {
        return false;
    }
    ScreenToClient(hwnd(), &point);
    const float s = scale_;
    if (expand_ < 0.3f) {
        /* Tendedero uses the menu-bar band, not the rope itself. On Windows
           that is the top edge of the screen, across the full width. */
        RECT client{};
        GetClientRect(hwnd(), &client);
        const float hot = 10.0f * s;
        return point.y >= 0 && point.y <= hot &&
               point.x >= 0 && point.x <= client.right;
    }
    const RackLayout& layout = view_.layout();
    const float lineHitBottom = layout.lineY + layout.sag + 8.0f * s;
    return point.y >= 0 && point.y <= lineHitBottom &&
           point.x >= layout.lineStart - 6.0f * s &&
           point.x <= layout.lineEnd + 6.0f * s;
}

bool RackWindow::CursorOverContent() const
{
    if (!hwnd() || !visible() || expand_ < 0.5f) {
        return false;
    }
    POINT point{};
    if (!GetCursorPos(&point)) {
        return false;
    }
    ScreenToClient(hwnd(), &point);
    return rack::PointIn(view_.layout().content, static_cast<float>(point.x),
                         static_cast<float>(point.y));
}

bool RackWindow::PointOverPreview(const POINT& screen) const
{
    if (!preview_.showing()) {
        return false;
    }
    const RECT rect = preview_.screenRect();
    return screen.x >= rect.left && screen.x < rect.right && screen.y >= rect.top &&
           screen.y < rect.bottom;
}

void RackWindow::UpdateHover()
{
    const uint64_t now = rack::NowMs();
    POINT cursor{};
    GetCursorPos(&cursor);
    const bool overLine = CursorOverLine();
    const bool overContent = CursorOverContent();
    const bool overPreview = PointOverPreview(cursor);
    const bool inside = overLine || overContent || overPreview;

    if (overLine && expand_ < 0.3f) {
        if (!lineHover_) {
            lineHover_ = true;
            hoverLineSince_ = now;
        }
        // A deliberate hover is required so brushing the top edge for the
        // menu bar does not pull the line down.
        const int delay = std::max(600, plugin_->config().hoverDelayMs);
        if (now - hoverLineSince_ >= static_cast<uint64_t>(delay)) {
            Expand(false);
        }
    } else if (!overLine) {
        lineHover_ = false;
    }

    if (inside) {
        lastInside_ = now;
        // The hotkey-opened rack becomes a normal interactive rack once the
        // pointer reaches it; leaving it then retracts as usual.
        hotkeyPinned_ = false;
    }

    if (plugin_->config().autoRetract && expanding_ && !inside && !mouseDown_ &&
        !dragStarted_ && !plugin_->PreviewPinned() && !hotkeyPinned_ &&
        !view_.HasRemoved()) {
        const uint64_t autoHide = static_cast<uint64_t>(std::max(0, plugin_->config().autoHideMs));
        const uint64_t hideAt = std::max(peekUntil_, lastInside_ + autoHide);
        if (now >= hideAt) {
            Collapse();
        }
    }
}

void RackWindow::UpdateAccessoryHover(const RackHit& hit)
{
    if (!hwnd()) {
        return;
    }
    const bool expanded = expand_ > 0.9f;
    const float basketTarget =
        (expanded && hit.part == RackPart::Basket) ? 1.0f : 0.0f;
    const float envelopeTarget =
        (expanded && hit.part == RackPart::Envelope) ? 1.0f : 0.0f;
    if (basketTarget != basketHoverTarget_ || envelopeTarget != envelopeHoverTarget_) {
        if (envelopeTarget > 0.5f && envelopeHoverTarget_ < 0.5f) {
            envelopeEnterMs_ = rack::NowMs();
        }
        basketHoverTarget_ = basketTarget;
        envelopeHoverTarget_ = envelopeTarget;
        lastAnimMs_ = 0;
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

void RackWindow::SetClickThrough(bool enabled)
{
    if (!hwnd() || clickThrough_ == enabled) {
        return;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd(), GWL_EXSTYLE);
    const LONG_PTR next =
        enabled ? (style | WS_EX_TRANSPARENT) : (style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (next != style) {
        SetWindowLongPtrW(hwnd(), GWL_EXSTYLE, next);
    }
    clickThrough_ = enabled;
}

void RackWindow::UpdateClickThrough(const RackHit& hit)
{
    /* Keep cards/notes/accessories interactive, but let the transparent gaps,
       including the empty part of the rope strip, reach the window below. */
    const bool interactive =
        expand_ > 0.9f && hit.part != RackPart::None &&
        (hit.part == RackPart::Card || hit.part == RackPart::Note ||
         hit.part == RackPart::Close || hit.part == RackPart::Lock ||
         hit.part == RackPart::Basket || hit.part == RackPart::Envelope ||
         hit.part == RackPart::Clip);
    const bool keepMouse = mouseDown_ || dragStarted_;
    SetClickThrough(!keepMouse && !interactive);
}

void RackWindow::UpdatePreview()
{
    if (!plugin_ || dragStarted_ || mouseDown_) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);

    RackPart part = RackPart::None;
    RackHit hit;
    hit.index = HitIndexAtCursor(&part);
    hit.part = part;
    const int index = hit.index;
    const bool overPreview = PointOverPreview(cursor);
    const uint64_t now = rack::NowMs();
    UpdateAccessoryHover(hit);
    UpdateClickThrough(hit);
    bool visualChanged = false;

    if (index >= 0 && expand_ > 0.9f) {
        previewHideAt_ = 0;
        if (index != hovered_) {
            hovered_ = index;
            hoveredSince_ = now;
            cardHoverIndex_ = index;
            cardHover_ = 0.0f;
            cardHoverVelocity_ = 0.0f;
            cardHoverTarget_ = 1.0f;
            lastAnimMs_ = 0;
            SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            visualChanged = true;
            if (preview_.showing()) {
                HidePreviewNow();
            }
        }
        if (!preview_.showing() && now - hoveredSince_ >= kPreviewDelayMs &&
            plugin_->HoverPreviewEnabled() && index >= 0 &&
            index < static_cast<int>(plugin_->items().size())) {
            const Item& hoverItem = plugin_->items()[static_cast<size_t>(index)];
            const bool previewable =
                hoverItem.kind == 0 ||
                (hoverItem.kind == 2 && !hoverItem.thumbPath.empty());
            if (previewable) ShowPreviewFor(index);
        } else if (preview_.showing() && previewIndex_ == index) {
            RECT anchor{};
            const POINT origin = screenOrigin();
            const CardBox* box = view_.Card(index);
            if (box) {
                anchor.left = static_cast<LONG>(box->card.left) + origin.x;
                anchor.top = static_cast<LONG>(box->card.top) + origin.y;
                anchor.right = static_cast<LONG>(box->card.right) + origin.x;
                anchor.bottom = static_cast<LONG>(box->card.bottom) + origin.y;
                if (!NearlySameRect(anchor, previewAnchor_)) {
                    ShowPreviewFor(index);
                }
            }
        }
    } else if (overPreview) {
        previewHideAt_ = 0;
    } else {
        if (hovered_ != -1) {
            hovered_ = -1;
            cardHoverTarget_ = 0.0f;
            lastAnimMs_ = 0;
            SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            visualChanged = true;
        }
        if (preview_.showing()) {
            if (previewHideAt_ == 0) {
                previewHideAt_ = now + kPreviewHideDelayMs;
            } else if (now >= previewHideAt_) {
                HidePreviewNow();
            }
        }
    }
    if (visualChanged) {
        Render();
    }
}

void RackWindow::CheckEditing()
{
    if (!plugin_) {
        return;
    }
    bool anyEditing = false;
    bool changed = false;
    const uint64_t now = rack::NowMs();

    for (auto& item : plugin_->items()) {
        if (!item.editing) {
            continue;
        }
        anyEditing = true;
        if (now - item.editStartedMs > 10ull * 60ull * 1000ull) {
            item.editing = false;
            item.editPath.clear();
            continue;
        }
        const uint64_t mtime = util::FileWriteTime(item.editPath);
        if (mtime == 0 || mtime == item.mtime) {
            continue;
        }
        item.mtime = mtime;
        item.path = item.editPath;
        item.ownsFile = true;
        item.thumb.Reset();
        item.thumbAttempted = false;
        item.preview.Reset();
        item.previewAttempted = false;
        UINT w = 0, h = 0;
        if (SUCCEEDED(plugin_->graphics().images().GetSize(item.path, w, h)) && w > 0 && h > 0) {
            item.aspect = static_cast<float>(w) / static_cast<float>(h);
        }
        item.editing = false;
        item.editPath.clear();
        needsLayout_ = true;
        changed = true;
        RackLog(L"RackWindow: edited image refreshed for item %d", item.id);
    }

    if (!anyEditing && editingWatchActive_) {
        KillTimer(hwnd(), kTimerEditWatch);
        editingWatchActive_ = false;
    }
    if (changed) {
        DismissPreview();
        Reposition();
        Render();
    }
}

RackHit RackWindow::HitAtCursor() const
{
    RackHit hit;
    if (!hwnd()) {
        return hit;
    }
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(hwnd(), &point);
    return view_.HitTest(static_cast<float>(point.x), static_cast<float>(point.y), scale_);
}

int RackWindow::HitIndexAtCursor(RackPart* outPart) const
{
    const RackHit hit = HitAtCursor();
    if (outPart) {
        *outPart = hit.part;
    }
    if (hit.part == RackPart::Card || hit.part == RackPart::Note ||
        hit.part == RackPart::Close || hit.part == RackPart::Lock) {
        return hit.index;
    }
    return -1;
}

D2D1_RECT_F RackWindow::CardScreenRect(int index) const
{
    const CardBox* box = view_.Card(index);
    if (!box) {
        return D2D1::RectF();
    }
    const POINT origin = screenOrigin();
    return AddRect(box->card, origin);
}

void RackWindow::ShowPreviewFor(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    const D2D1_RECT_F card = CardScreenRect(index);
    if (card.right <= card.left) {
        return;
    }
    preview_.ShowItem(plugin_->items()[index], card, workArea_, scale_, plugin_->theme());
    previewIndex_ = index;
    previewAnchor_.left = static_cast<LONG>(card.left);
    previewAnchor_.top = static_cast<LONG>(card.top);
    previewAnchor_.right = static_cast<LONG>(card.right);
    previewAnchor_.bottom = static_cast<LONG>(card.bottom);
}

void RackWindow::HidePreviewNow()
{
    preview_.Dismiss();
    previewIndex_ = -1;
    previewHideAt_ = 0;
}

void RackWindow::DismissPreview()
{
    HidePreviewNow();
}

void RackWindow::RefreshPreview()
{
    if (preview_.showing() && previewIndex_ >= 0 &&
        previewIndex_ < static_cast<int>(plugin_->items().size())) {
        ShowPreviewFor(previewIndex_);
    }
}

HMONITOR RackWindow::CursorMonitor() const
{
    POINT point{};
    GetCursorPos(&point);
    return MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
}

RECT RackWindow::MonitorWorkArea(HMONITOR monitor) const
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    RECT fallback{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return fallback;
}

bool RackWindow::IsFullscreenOn(HMONITOR monitor) const
{
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE)) {
        return true;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == hwnd()) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == GetCurrentProcessId()) {
        return false;
    }

    wchar_t className[128]{};
    GetClassNameW(foreground, className, 128);
    if (_wcsicmp(className, L"Progman") == 0 || _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }
    if (MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) != monitor) {
        return false;
    }

    RECT windowRect{};
    RECT monitorRect{};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetWindowRect(foreground, &windowRect) || !GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    monitorRect = info.rcMonitor;
    return windowRect.left <= monitorRect.left && windowRect.top <= monitorRect.top &&
           windowRect.right >= monitorRect.right && windowRect.bottom >= monitorRect.bottom;
}

void RackWindow::CopyItem(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    const Item& copyItem = plugin_->items()[index];
    if (copyItem.kind == 1) {
        clipboard::CopyText(copyItem.note);
    } else if (copyItem.kind == 0) {
        clipboard::CopyImage(copyItem.path);
    } else if (!copyItem.path.empty()) {
        clipboard::CopyFilePath(copyItem.path);
    } else {
        clipboard::CopyText(copyItem.label);
    }
    view_.ShowCopyToast(index, rack::NowMs() + 900);
    if (hwnd()) {
        SetTimer(hwnd(), kTimerCopyToast, 900, nullptr);
    }
    Render();
}

void RackWindow::OpenItem(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    util::OpenInShell(plugin_->items()[index].path);
}

void RackWindow::EditItemInPaint(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    Item& item = plugin_->items()[index];
    if (item.editing) {
        return;
    }
    DismissPreview();
    const std::wstring cacheDir = util::JoinPath(util::GetDataDir(), L"cache");
    util::EnsureDir(cacheDir);
    const std::wstring editPath = util::JoinPath(
        cacheDir, util::Format(L"edit-%s-%d.png", util::TimestampName().c_str(), item.id));
    if (!util::CopyFileTo(item.path, editPath)) {
        RackLog(L"RackWindow: failed to create edit copy for %s", item.path.c_str());
        return;
    }
    item.editPath = editPath;
    item.editing = true;
    item.editStartedMs = rack::NowMs();
    item.mtime = util::FileWriteTime(editPath);
    if (!util::OpenInPaint(editPath)) {
        item.editing = false;
        item.editPath.clear();
        return;
    }
    if (!editingWatchActive_) {
        editingWatchActive_ = true;
        SetTimer(hwnd(), kTimerEditWatch, 500, nullptr);
    }
}

void RackWindow::SaveItemAs(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    DismissPreview();
    const Item& item = plugin_->items()[index];
    if (item.path.empty() && !item.note.empty()) {
        wchar_t txtFile[MAX_PATH]{};
        const std::wstring txtSuggested = util::Format(L"SnowAir-%s.txt", util::TimestampName().c_str());
        wcsncpy_s(txtFile, txtSuggested.c_str(), _TRUNCATE);
        OPENFILENAMEW txtOfn{};
        txtOfn.lStructSize = sizeof(txtOfn);
        txtOfn.hwndOwner = hwnd();
        txtOfn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件\0*.*\0\0";
        txtOfn.lpstrFile = txtFile;
        txtOfn.nMaxFile = MAX_PATH;
        txtOfn.lpstrDefExt = L"txt";
        txtOfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetSaveFileNameW(&txtOfn)) {
            std::string txt = "\xEF\xBB\xBF";
            txt += util::WideToUtf8(item.note);
            util::WriteFileBytes(txtFile, txt.data(), txt.size());
        }
        SetTopmost();
        return;
    }
    wchar_t file[MAX_PATH]{};
    const std::wstring suggested = util::Format(L"SnowAir-%s.png", util::TimestampName().c_str());
    wcsncpy_s(file, suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd();
    ofn.lpstrFilter = L"PNG 图片\0*.png\0所有文件\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) {
        util::CopyFileTo(item.path, file);
    }
    SetTopmost();
}

void RackWindow::RemoveItem(int index, bool toBasket)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    view_.BeginRemove(index, plugin_->items()[index], toBasket);
    SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    plugin_->RemoveItemAt(index);
}

void RackWindow::EditNote(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    DismissPreview();
    std::wstring text = plugin_->items()[index].note;
    if (NoteEditor::Edit(hwnd(), util::GetDllModule(), text, plugin_->theme().dark, text)) {
        plugin_->SetItemNote(index, text, plugin_->items()[index].noteColor);
        hovered_ = -1;
        lastInside_ = rack::NowMs();
        Expand(true);
        Render();
    }
    SetTopmost();
}

void RackWindow::SetNoteColor(int index, int color)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    plugin_->SetItemNote(index, plugin_->items()[index].note, rack::ClampInt(color, 0, 2));
}

void RackWindow::ShowItemMenu(int index, POINT screen)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    batchMenu_.Close();
    const bool noteItem = plugin_->items()[index].path.empty() && !plugin_->items()[index].note.empty();

    if (plugin_->host() && plugin_->host()->show_menu) {
        SnowAirMenuItem batch[4]{};
        batch[0].label = L"取下左侧";
        batch[0].id = kMenuBatchLeft;
        batch[1].label = L"取下右侧";
        batch[1].id = kMenuBatchRight;
        batch[2].label = L"保留当前";
        batch[2].id = kMenuBatchKeep;
        batch[3].label = L"全部取下";
        batch[3].id = kMenuBatchAll;

        SnowAirMenuItem main[8]{};
        int n = 0;
        auto add = [&](const wchar_t* label, int id, int flags = 0) {
            main[n].label = label;
            main[n].id = id;
            main[n].flags = flags;
            ++n;
        };
        add(L"打开", kMenuOpen);
        add(L"另存为…", kMenuSaveAs);
        if (!noteItem) add(L"打开位置", kMenuShowInFolder);
        add(L"", 0, SNOWAIR_MENU_SEPARATOR);
        add(L"取下内容", kMenuRemove);
        add(L"批量取下", kMenuBatch);
        main[n - 1].submenu = batch;
        main[n - 1].submenu_count = 4;
        add(L"", 0, SNOWAIR_MENU_SEPARATOR);
        add(L"移至回收站", kMenuRecycle);
        if (noteItem) main[n - 1].label = L"销毁";

        menuIndex_ = index;
        plugin_->host()->show_menu(plugin_->host()->user, main, n, screen.x, screen.y,
                                   this, &RackWindow::MenuSelected);
        return;
    }

    const Item& item = plugin_->items()[index];

    std::vector<MenuItem> items;
    auto action = [&](const wchar_t* label, int id) {
        MenuItem entry;
        entry.kind = MenuItem::Kind::Action;
        entry.label = label;
        entry.id = id;
        items.push_back(std::move(entry));
    };
    auto separator = [&]() {
        MenuItem entry;
        entry.kind = MenuItem::Kind::Separator;
        items.push_back(std::move(entry));
    };

    action(L"打开", kMenuOpen);
    action(L"另存为…", kMenuSaveAs);
    if (!noteItem) action(L"打开位置", kMenuShowInFolder);
    separator();
    action(L"取下内容", kMenuRemove);
    action(L"批量取下  ›", kMenuBatch);
    separator();
    if (noteItem) action(L"销毁", kMenuRecycle); else action(L"移至回收站", kMenuRecycle);

    const bool menuDark = plugin_->host() && plugin_->host()->is_dark
                              ? plugin_->host()->is_dark(plugin_->host()->user) != 0
                              : plugin_->theme().dark;
    menuOpen_ = true;
    const POINT menuPoint = screen;
    menu_.Show(items, screen, workArea_, scale_, Theme::MenuStyle(menuDark),
               [this, index, menuPoint](int command) {
                   if (command == kMenuBatch) {
                       this->menu_.Close();
                       this->ShowBatchMenu(index, menuPoint);
                   } else {
                       this->HandleMenuCommand(index, command);
                   }
               },
               [this]() { menuOpen_ = false; });
    if (!menu_.showing()) {
        menuOpen_ = false;
    }
}

void RackWindow::MenuSelected(void* user, int id)
{
    auto* window = static_cast<RackWindow*>(user);
    if (window) {
        window->HandleMenuCommand(window->menuIndex_, id);
    }
}

void RackWindow::ShowBatchMenu(int index, POINT screen)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        menuOpen_ = false;
        return;
    }
    std::vector<MenuItem> items;
    auto action = [&](const wchar_t* label, int id) {
        MenuItem entry;
        entry.kind = MenuItem::Kind::Action;
        entry.label = label;
        entry.id = id;
        items.push_back(std::move(entry));
    };
    action(L"取下左侧", kMenuBatchLeft);
    action(L"取下右侧", kMenuBatchRight);
    action(L"保留当前", kMenuBatchKeep);
    action(L"全部取下", kMenuBatchAll);

    const bool menuDark = plugin_->host() && plugin_->host()->is_dark
                              ? plugin_->host()->is_dark(plugin_->host()->user) != 0
                              : plugin_->theme().dark;
    menuOpen_ = true;
    batchMenu_.Show(items, screen, workArea_, scale_, Theme::MenuStyle(menuDark),
                    [this, index](int command) { HandleMenuCommand(index, command); },
                    [this]() { menuOpen_ = false; });
    if (!batchMenu_.showing()) {
        menuOpen_ = false;
    }
}

void RackWindow::ShowGeneralMenu(POINT screen)
{
    std::vector<MenuItem> items;
    MenuItem toggle;
    toggle.kind = MenuItem::Kind::Action;
    toggle.label = expanded() ? L"收起晾衣架" : L"展开晾衣架";
    toggle.id = kMenuCollapse;
    items.push_back(std::move(toggle));

    MenuItem separator;
    separator.kind = MenuItem::Kind::Separator;
    items.push_back(std::move(separator));

    MenuItem settings;
    settings.kind = MenuItem::Kind::Action;
    settings.label = L"设置";
    settings.id = kMenuSettings;
    items.push_back(std::move(settings));

    menuOpen_ = true;
    const bool menuDark = plugin_ && plugin_->host() && plugin_->host()->is_dark
                              ? plugin_->host()->is_dark(plugin_->host()->user) != 0
                              : (plugin_ ? plugin_->theme().dark : false);
    menu_.Show(items, screen, workArea_, scale_, Theme::MenuStyle(menuDark),
               [this](int command) { HandleMenuCommand(-1, command); },
               [this]() { menuOpen_ = false; });
    if (!menu_.showing()) {
        menuOpen_ = false;
    }
}

void RackWindow::HandleMenuCommand(int index, int command)
{
    if (!plugin_) {
        return;
    }
    if (command == kMenuSettings) {
        if (plugin_->host() && plugin_->host()->open_settings) {
            plugin_->host()->open_settings(plugin_->host()->user);
        }
        return;
    }
    if (command == kMenuCollapse) {
        if (expanded()) {
            Collapse();
        } else {
            Expand(false);
        }
        return;
    }
    if (index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }

    const Item& menuItem = plugin_->items()[index];
    const bool noteItem = menuItem.path.empty() && !menuItem.note.empty();
    if (command == kMenuOpen) {
        if (noteItem) EditNote(index);
        else OpenItem(index);
    } else if (command == kMenuSaveAs) {
        SaveItemAs(index);
    } else if (command == kMenuShowInFolder) {
        if (!noteItem) {
            util::ShowInExplorer(menuItem.sourcePath.empty() ? menuItem.path : menuItem.sourcePath);
        }
    } else if (command == kMenuRemove) {
        RemoveItem(index);
    } else if (command == kMenuRecycle) {
        if (noteItem) {
            plugin_->DestroyNoteText(menuItem.note);
            RemoveItem(index, false);
        } else if (RecyclePath(menuItem.path)) {
            RemoveItem(index, false);
        }
    
    } else if (command == kMenuBatchLeft || command == kMenuBatchRight ||
               command == kMenuBatchKeep || command == kMenuBatchAll) {
        const int count = static_cast<int>(plugin_->items().size());
        std::vector<int> remove;
        if (command == kMenuBatchLeft) {
            for (int i = 0; i < index; ++i) remove.push_back(i);
        } else if (command == kMenuBatchRight) {
            for (int i = index + 1; i < count; ++i) remove.push_back(i);
        } else if (command == kMenuBatchKeep) {
            for (int i = 0; i < count; ++i) {
                if (i != index) remove.push_back(i);
            }
        } else {
            for (int i = 0; i < count; ++i) remove.push_back(i);
        }
        for (auto it = remove.rbegin(); it != remove.rend(); ++it) {
            RemoveItem(*it);
        }
    } else if (command == kMenuCopy) {
        CopyItem(index);
    } else if (command == kMenuEditPaint) {
        EditItemInPaint(index);
    } else if (command == kMenuNote) {
        EditNote(index);
    } else if (command >= kMenuColorBase && command <= kMenuColorBase + 2) {
        SetNoteColor(index, static_cast<int>(command - kMenuColorBase));
    }
}

void RackWindow::BeforeDraw(ID2D1RenderTarget* target)
{
    if (!plugin_ || !graphics_) {
        return;
    }
    view_.LoadAssets(target);
    for (auto& item : plugin_->items()) {
        if (item.thumb.valid() || item.thumbAttempted) {
            continue;
        }
        item.thumbAttempted = true;
        if (item.kind == 1 || item.kind == 3) {
            continue;   /* 便签 / 文件卡片没有图片 */
        }
        if (item.kind == 2 && item.thumbPath.empty()) {
            continue;   /* 录屏没有封面：退化成纯文字卡片 */
        }
        const std::wstring& source =
            (item.kind == 2 && !item.thumbPath.empty()) ? item.thumbPath : item.path;
        UINT w = 0, h = 0;
        if (SUCCEEDED(graphics_->images().GetSize(source, w, h)) && w > 0 && h > 0) {
            const float aspect = static_cast<float>(w) / static_cast<float>(h);
            if (std::abs(aspect - item.aspect) > 0.01f) {
                item.aspect = aspect;
                needsLayout_ = true;
            }
        }
        /* The card image is only ~140 DIP wide. Downscale once with WIC Fant
           close to its display size instead of letting D2D minify a 640px
           thumbnail at draw time, which turns thin UI lines into mush. */
        graphics_->images().Load(source, 192.0f * scale_, target, item.thumb);
    }
}

void RackWindow::Paint(ID2D1RenderTarget* target)
{
    if (!plugin_) {
        return;
    }
    bool darkUi = plugin_->theme().dark;
    if (plugin_->host()) {
        if (plugin_->host()->is_dark) {
            darkUi = plugin_->host()->is_dark(plugin_->host()->user) != 0;
        }
        // The background role is the most reliable signal when the host's
        // explicit dark flag is stale during startup/theme transitions.
        if (plugin_->host()->theme_color) {
            const uint32_t bg = plugin_->host()->theme_color(
                plugin_->host()->user, SA_THEME_BG);
            const float luma = 0.2126f * ((bg >> 24) & 0xFF) +
                               0.7152f * ((bg >> 16) & 0xFF) +
                               0.0722f * ((bg >> 8) & 0xFF);
            darkUi = luma < 128.0f;
        }
    }
    view_.SetUiDark(darkUi);
    const float swaySeconds =
        revealStart_ ? static_cast<float>(rack::NowMs() - revealStart_) / 1000.0f : 10.0f;
    view_.Draw(target, plugin_->items(), plugin_->theme(), expand_, swaySeconds, basketHover_,
               paperPos_[0], paperPos_[1], paperPos_[2], cardHover_, cardHoverIndex_,
               hovered_, mouseDown_ && !dragStarted_ ? pressed_ : -1,
               plugin_->HistoryCount(), plugin_->ClipboardCount());
}

LRESULT RackWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_TIMER:
        switch (wParam) {
        case kTimerPoll:
            Tick();
            return 0;
        case kTimerAnim:
            Animate();
            return 0;
        case kTimerClick: {
            KillTimer(hwnd(), kTimerClick);
            const int id = clickItemId_;
            clickItemId_ = -1;
            if (id >= 0 && plugin_) {
                const int index = plugin_->IndexById(id);
                if (index >= 0) {
                    CopyItem(index);
                }
            }
            return 0;
        }
        case kTimerCopyToast:
            KillTimer(hwnd(), kTimerCopyToast);
            view_.ClearCopyToast();
            Render();
            return 0;
        case kTimerLongPress: {
            KillTimer(hwnd(), kTimerLongPress);
            if (mouseDown_ && !dragStarted_ && plugin_) {
                longPressFired_ = true;
                mouseDown_ = false;
                ReleaseCapture();
                const int index = plugin_->IndexById(pressItemId_);
                pressed_ = -1;
                pressedPart_ = RackPart::None;
                if (index >= 0) {
                    EditItemInPaint(index);
                }
                Render();
            }
            return 0;
        }
        case kTimerEditWatch:
            CheckEditing();
            return 0;
        default:
            break;
        }
        break;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR: {
        const RackHit hit = HitAtCursor();
        if (hit.part != RackPart::None) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        if (!plugin_) {
            return 0;
        }
        if (CursorOverLine() && expand_ < 0.3f) {
            Expand(false);
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RackHit hit = view_.HitTest(static_cast<float>(point.x),
                                          static_cast<float>(point.y), scale_);
        if (hit.part == RackPart::Close) {
            RemoveItem(hit.index, false);
            return 0;
        }
        if (hit.part == RackPart::Lock) {
            if (hit.index >= 0 && hit.index < static_cast<int>(plugin_->items().size())) {
                plugin_->items()[hit.index].locked =
                    !plugin_->items()[hit.index].locked;
                plugin_->SaveItems();
                Render();
            }
            return 0;
        }
        if (hit.part == RackPart::Basket) {
            plugin_->OpenHistory();
            return 0;
        }
        if (hit.part == RackPart::Envelope) {
            const D2D1_POINT_2F anchor = view_.EnvelopeBadgeCenter();
            POINT screen{static_cast<LONG>(std::lround(anchor.x)),
                         static_cast<LONG>(std::lround(anchor.y))};
            ClientToScreen(hwnd(), &screen);
            plugin_->OpenClipboard(screen.x, screen.y);
            return 0;
        }
        if (hit.part == RackPart::Card || hit.part == RackPart::Note) {
            if (hit.index < 0 || hit.index >= static_cast<int>(plugin_->items().size())) {
                return 0;
            }
            pressed_ = hit.index;
            pressItemId_ = plugin_->items()[hit.index].id;
            pressedPart_ = hit.part;
            mouseDown_ = true;
            dragStarted_ = false;
            longPressFired_ = false;
            pressPoint_ = point;
            cardDragStartOffset_ = plugin_->items()[hit.index].rackOffsetX;
            cardDragging_ = false;
            lastCardDragPaint_ = 0;
            pendingAdjustId_ = -1;
            offsetAnims_.clear();
            SetCapture(hwnd());
            SetTimer(hwnd(), kTimerLongPress, 560, nullptr);
            Render();
        } else if (hit.part == RackPart::Clip) {
            if (hit.index < 0 || hit.index >= static_cast<int>(plugin_->items().size())) {
                return 0;
            }
            pressed_ = hit.index;
            pressItemId_ = plugin_->items()[hit.index].id;
            pressedPart_ = RackPart::Clip;
            mouseDown_ = true;
            dragStarted_ = false;
            pressPoint_ = point;
            SetCapture(hwnd());
            Render();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!mouseDown_ || dragStarted_ || !plugin_) {
            return 0;
        }
        if (pressedPart_ == RackPart::Clip) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int dx = std::abs(point.x - pressPoint_.x);
        const int dy = std::abs(point.y - pressPoint_.y);
        if (dx <= 6 * scale_ && dy <= 6 * scale_) {
            return 0;
        }

        const Item* heldItem = plugin_->ItemById(pressItemId_);
        const bool locked = heldItem && heldItem->locked;
        if (pressedPart_ == RackPart::Card && !cardDragging_ &&
            (locked || dy <= 24 * scale_) &&
            (dx > 6 * scale_ || (locked && dy > 6 * scale_))) {
            cardDragging_ = true;
            KillTimer(hwnd(), kTimerLongPress);
        }
        if (cardDragging_) {
            if (Item* item = plugin_->ItemById(pressItemId_)) {
                const int index = plugin_->IndexById(pressItemId_);
                if (const CardBox* box = view_.Card(index)) {
                    const float width = box->card.right - box->card.left;
                    const float baseLeft = box->card.left - item->rackOffsetX;
                    float desired = cardDragStartOffset_ + (point.x - pressPoint_.x);
                    const float minLeft = 24.0f * scale_;
                    const float maxLeft = view_.layout().width - width - 24.0f * scale_;
                    const float left = baseLeft + desired;
                    desired += rack::Clamp(left, minLeft, std::max(minLeft, maxLeft)) - left;
                    item->rackOffsetX = desired;
                }
            }
            // Coalesce mouse-move paints. Painting the full-width layered
            // window at raw 125-1000 Hz was what made the card stutter near
            // the rope; 12 ms still tracks the pointer closely.
            const uint64_t paintNow = rack::NowMs();
            if (lastCardDragPaint_ == 0 || paintNow - lastCardDragPaint_ >= 12) {
                lastCardDragPaint_ = paintNow;
                Reposition();
                Render();
            }
            return 0;
        }

        KillTimer(hwnd(), kTimerLongPress);
        const int index = plugin_->IndexById(pressItemId_);
        mouseDown_ = false;
        dragStarted_ = true;
        pressed_ = -1;
        pressedPart_ = RackPart::None;
        ReleaseCapture();

        if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
            bool dropped = false;
            view_.SetDragging(index);
            Render();
            const CardBox* dragBox = view_.Card(index);
            const int dragW = dragBox ? static_cast<int>(dragBox->card.right -
                                                         dragBox->card.left)
                                      : static_cast<int>(154.0f * scale_);
            const int dragH = dragBox ? static_cast<int>(dragBox->card.bottom -
                                                         dragBox->card.top)
                                      : static_cast<int>(119.0f * scale_);
            const DWORD effect = dragdrop::BeginDrag(plugin_->items()[index].path, dragW,
                                                     dragH, 4.0f * scale_, &dropped);
            view_.SetDragging(-1);
            if ((effect & DROPEFFECT_MOVE) && dropped) {
                view_.BeginRemove(index, plugin_->items()[index]);
                SetTimer(hwnd(), kTimerAnim, 16, nullptr);
                plugin_->RemoveItemAt(index);
            }
        }
        dragStarted_ = false;
        Render();
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!mouseDown_) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        mouseDown_ = false;
        KillTimer(hwnd(), kTimerLongPress);
        ReleaseCapture();
        if (pressedPart_ == RackPart::Clip) {
            const int dx = std::abs(point.x - pressPoint_.x);
            const int dy = std::abs(point.y - pressPoint_.y);
            if (dx <= 6 * scale_ && dy <= 6 * scale_) {
                const int index = plugin_->IndexById(pressItemId_);
                if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
                    const Item& item = plugin_->items()[static_cast<size_t>(index)];
                    if (item.kind == 0 && !item.path.empty()) {
                        /* 卡片完全落下去之后再贴到桌面。 */
                        pendingPinPath_ = item.path;
                        pendingPinAt_ = rack::NowMs() + 560;
                        RemoveItem(index, false);
                    }
                }
            }
            pressed_ = -1;
            pressedPart_ = RackPart::None;
            Render();
            return 0;
        }
        if (cardDragging_) {
            cardDragging_ = false;
            pressed_ = -1;
            pressedPart_ = RackPart::None;
            pendingAdjustId_ = pressItemId_;
            pendingAdjustAt_ = rack::NowMs() + 320;
            if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            Reposition();
            plugin_->SaveItems();
            Render();
            return 0;
        }
        const bool wasLong = longPressFired_;
        longPressFired_ = false;

        if (!wasLong && plugin_) {
            const int dx = std::abs(point.x - pressPoint_.x);
            const int dy = std::abs(point.y - pressPoint_.y);
            if (dx <= 6 * scale_ && dy <= 6 * scale_) {
                const RackHit hit = view_.HitTest(static_cast<float>(point.x),
                                                  static_cast<float>(point.y), scale_);
                if (hit.part == RackPart::Basket) {
                    plugin_->OpenHistory();
                }
                const int index = plugin_->IndexById(pressItemId_);
                if (index >= 0 && hit.index == index) {
                    if (hit.part == RackPart::Note) {
                        EditNote(index);
                    } else if (hit.part == RackPart::Card) {
                        clickItemId_ = pressItemId_;
                        SetTimer(hwnd(), kTimerClick, GetDoubleClickTime() + 30, nullptr);
                    }
                }
            }
        }
        pressed_ = -1;
        pressedPart_ = RackPart::None;
        Render();
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        KillTimer(hwnd(), kTimerClick);
        clickItemId_ = -1;
        if (!plugin_) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RackHit hit = view_.HitTest(static_cast<float>(point.x),
                                          static_cast<float>(point.y), scale_);
        if (hit.part == RackPart::Card && hit.index >= 0 &&
            hit.index < static_cast<int>(plugin_->items().size())) {
            OpenItem(hit.index);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        if (!plugin_) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const RackHit hit = view_.HitTest(static_cast<float>(point.x),
                                          static_cast<float>(point.y), scale_);
        if (hit.part == RackPart::Card && hit.index >= 0 &&
            hit.index < static_cast<int>(plugin_->items().size())) {
            POINT screen = point;
            ClientToScreen(hwnd(), &screen);
            ShowItemMenu(hit.index, screen);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (preview_.showing() && preview_.hwnd()) {
            SendMessageW(preview_.hwnd(), WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (mouseDown_ && GetCapture() != hwnd()) {
            mouseDown_ = false;
            pressItemId_ = -1;
            pressed_ = -1;
            pressedPart_ = RackPart::None;
            KillTimer(hwnd(), kTimerLongPress);
        }
        return 0;

    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
        SetDpiScale(scale_);
        monitor_ = MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST);
        PositionForMonitor(monitor_);
        Render();
        return 0;
    }

    case WM_DISPLAYCHANGE:
        monitor_ = CursorMonitor();
        PositionForMonitor(monitor_);
        EnsureVisible();
        Render();
        return 0;

    case WM_SETTINGCHANGE:
        if (lParam && _wcsicmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            if (plugin_) {
                plugin_->RefreshTheme();
            }
            Render();
        } else if (plugin_) {
            plugin_->RefreshTheme();
            PositionForMonitor(monitor_ ? monitor_ : CursorMonitor());
            Render();
        }
        return 0;

    case WM_THEMECHANGED:
        if (plugin_) {
            plugin_->RefreshTheme();
        }
        Render();
        return 0;

    default:
        break;
    }
    return LayeredWindow::HandleMessage(message, wParam, lParam);
}


