#include "RackWindow.h"
#include <fstream>
#include <unordered_map>

#include "Clipboard.h"
#include "Config.h"
#include "DragDrop.h"
#include "NoteEditor.h"
#include "NoteStore.h"
#include "Plugin.h"
#include "RackLayoutMath.h"
#include "Util.h"

#include <commdlg.h>
#include <shellapi.h>

namespace {

constexpr int kPollIntervalMs = 50;
constexpr int kAnimDurationMs = 170;
constexpr int kPreviewDelayMs = 320;
constexpr int kPreviewHideDelayMs = 180;
constexpr int64_t kPeekDurationMs = 3000;
/* 移除卡片后先延迟该时长再开始靠拢，避免瞬间贴合的僵硬感。 */
constexpr uint64_t kCloseDelayMs = 130;
/* 屏幕最上面这一条：点一下就收回，和「停在这里拉下来」用的是同一块区域。 */
constexpr float kTopBarBand = 24.0f;
constexpr UINT kMsgTopBarClick = WM_APP + 2;
constexpr UINT kMsgColorWheel = WM_APP + 3;
constexpr UINT kMsgRackRightClick = WM_APP + 4;
constexpr UINT kMsgRackScroll = WM_APP + 5;

bool CanPinItem(const Item& item)
{
    if (item.kind == 0) return !item.path.empty();
    if (!item.paths.empty() || !item.path.empty()) return true;
    return !item.note.empty() || !item.label.empty();
}

bool PinAsFiles(const Item& item)
{
    return !item.paths.empty() || item.kind == 2 || item.kind == 3;
}

std::wstring PinTextForItem(const Item& item)
{
    if (!item.note.empty()) return item.note;
    if (!item.label.empty()) return item.label;
    return item.path;
}

void SendPasteShortcut()
{
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

/* 有些输入框要先点一下才拿到焦点/插入符，粘贴前在落点补一次左键单击。 */
void ClickAt(POINT point)
{
    SetCursorPos(point.x, point.y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(20);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

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

/* 弹簧积分器：用当前位置和速度步进，和常见的 SpringMotion 形式一致。 */
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

D2D1_POINT_2F RotateAroundPoint(D2D1_POINT_2F point, D2D1_POINT_2F center, float degrees)
{
    const float radians = degrees * 3.1415926535f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return D2D1::Point2F(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
}

/* 悬停时卡片最多放大到这个倍数，命中形状和绘制保持一致。 */
constexpr float kCardHoverScale = 1.05f;

bool RoundedRectContains(float x, float y, float left, float top, float right, float bottom,
                         float radius)
{
    if (x < left || x > right || y < top || y > bottom) {
        return false;
    }
    radius = std::max(0.0f, std::min(radius, std::min(right - left, bottom - top) * 0.5f));
    const float cx = std::clamp(x, left + radius, right - radius);
    const float cy = std::clamp(y, top + radius, bottom - radius);
    const float dx = x - cx;
    const float dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
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
    historyDisabled_ = plugin_->HistoryDisabled();
    view_.SetHistoryDisabled(historyDisabled_);
    lastCardSize_ = plugin_->config().size;
    cardScaleValue_ = plugin_->config().CardScale();
    cardScaleFrom_ = cardScaleValue_;
    cardScaleTarget_ = cardScaleValue_;
    lastDisplayMode_ = plugin_->config().displayMode;
    manuallyHidden_ = lastDisplayMode_ == kDisplayHotkey;
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
    /* 2x 超采样后线性缩回：卡片圆角/20% 外圈/文字更平滑。 */
    SetSupersample(true);
    lastInside_ = rack::NowMs();
    /* 一次性迁移：清掉旧版本留下的偏移（旧偏移会让普通卡交叉）。
       迁移过后，偏移就是用户的自定义位置，跨启动保留。 */
    const std::wstring migrateFlag =
        util::JoinPath(util::GetDataDir(), L"row_offset_model2.flag");
    if (!util::FileExists(migrateFlag)) {
        bool clearedOffsets = false;
        for (auto& item : plugin_->items()) {
            if (item.rackOffsetX != 0.0f) {
                item.rackOffsetX = 0.0f;
                clearedOffsets = true;
            }
        }
        if (clearedOffsets) {
            plugin_->SaveItems();
        }
        std::ofstream flag(migrateFlag.c_str());
        flag << 2;
    }
    SetTimer(hwnd(), kTimerPoll, kPollIntervalMs, nullptr);
    return true;
}

void RackWindow::Shutdown()
{
    if (hwnd()) {
        KillTimer(hwnd(), kTimerPoll);
        KillTimer(hwnd(), kTimerAnim);
        KillTimer(hwnd(), kTimerClick);
        KillTimer(hwnd(), kTimerEditStart);
        KillTimer(hwnd(), kTimerFakeCursor);
    }
    SetTopBarWatch(false);
    menu_.Close();
    batchMenu_.Close();
    menu_.Destroy();
    batchMenu_.Destroy();
    preview_.Destroy();
    view_.Shutdown();
    Destroy();
    plugin_ = nullptr;
}

void RackWindow::OnItemsChanged(bool added, int addedId)
{
    DismissPreview();
    if (!plugin_ || plugin_->items().empty()) {
        menu_.Close();
        batchMenu_.Close();
        menuOpen_ = false;
        if (view_.HasRemoved()) {
            // 等最后一张卡片移入篮子再收起。
            hideWhenRemovalsDone_ = true;
            if (!(manuallyHidden_ &&
                  plugin_->config().displayMode != kDisplayHover)) {
                EnsureVisible();
                if (expand_ < 0.9f) {
                    expanding_ = true;
                    StartAnimation(1.0f);
                }
            }
            if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            Render();
            return;
        }
        if (plugin_->config().displayMode != kDisplayHover) {
            // 快捷键/常驻模式：没有卡片也保留衣架本体；手动隐藏时不要自动拉起。
            hideWhenRemovalsDone_ = false;
            EnsureVisible();
            if (plugin_->config().displayMode == kDisplayAlways && !manuallyHidden_ &&
                expand_ < 0.9f) {
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
    /* 新卡片按 id 找位置：自定义卡排在吸附组后面，items.back() 不一定是新卡。 */
    int arrivalIndex = -1;
    if (added) {
        arrivalIndex = addedId >= 0 ? plugin_->IndexById(addedId) : -1;
        if (arrivalIndex < 0) {
            arrivalIndex = static_cast<int>(plugin_->items().size()) - 1;
        }
        if (view_.AtScrollEnd()) {
            view_.ScrollToEnd();
        }
        view_.BeginArrival(arrivalIndex);
    }
    PositionForMonitor(monitor_);
    if (added) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd(), &cursor);
        const Item& incoming = plugin_->items()[static_cast<size_t>(arrivalIndex)];
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
        RackLog(L"新卡片入场：index=%d 起点=(%ld,%ld) 目标=(%.0f,%.0f)", arrivalIndex,
                cursor.x, cursor.y,
                view_.Card(arrivalIndex)
                    ? (view_.Card(arrivalIndex)->card.left + view_.Card(arrivalIndex)->card.right) * 0.5f
                    : -1.0f,
                view_.Card(arrivalIndex)
                    ? (view_.Card(arrivalIndex)->card.top + view_.Card(arrivalIndex)->card.bottom) * 0.5f
                    : -1.0f);
        if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
        /* 快捷键/常驻模式被手动隐藏时，新卡片只更新数据，不主动把绳子拉出来；
           等用户按快捷键显示。悬停模式保持原来的「新卡片 peek」。 */
        if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover) {
            Render();
        } else {
            Expand(true);
        }
    } else {
        EnsureVisible();
        if (plugin_->config().displayMode == kDisplayAlways && !manuallyHidden_ &&
            expand_ < 0.9f) {
            Expand(false);
        }
        Render();
    }
    /* 卡片增删后吸附组槽位会变，检查自定义卡有没有真的被压住。 */
    ResolveCustomOverlaps();
    if (needsLayout_ && hwnd()) {
        Reposition();
        Render();
    }
    RelaxGroupOffsetsIfNoCustoms();
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
    RefreshHistoryDisabled();
    /* 显示方式切到快捷键/常驻时立刻按新模式摆好：
       - 快捷键：先藏起来，等用户按快捷键
       - 常驻：直接展开
       - 悬停：显示绳子，等指针靠近 */
    const int newDisplayMode = plugin_->config().displayMode;
    if (newDisplayMode != lastDisplayMode_) {
        lastDisplayMode_ = newDisplayMode;
        manuallyHidden_ = newDisplayMode == kDisplayHotkey;
        if (newDisplayMode != kDisplayHotkey) {
            /* 切回悬停/常驻时，之前为快捷键模式排队的收起不应再生效。 */
            collapseAfterPreview_ = false;
        }
        if (newDisplayMode == kDisplayHotkey) {
            Collapse();
        } else {
            EnsureVisible();
            if (newDisplayMode == kDisplayAlways) {
                Expand(false);
            }
        }
    }
    /* 尺寸档位变化：只记录目标档位，卡片宽度和槽位在 Animate 里逐帧缓动。
       不在这里清偏移/槽位，否则会跳变。 */
    const int newCardSize = plugin_->config().size;
    const bool sizeChanged = lastCardSize_ >= 0 && newCardSize != lastCardSize_;
    lastCardSize_ = newCardSize;
    if (sizeChanged) {
        /* 不在配置回调里直接跳到新尺寸：让卡片宽度和槽位一起缓动，
           放大时把间距挤开，缩小时把间距收拢。 */
        cardScaleFrom_ = cardScaleValue_;
        cardScaleTarget_ = plugin_->config().CardScale();
        cardScaleAnimStart_ = rack::NowMs();
        cardScaleAnimating_ = true;
        needsLayout_ = true;
        if (hwnd()) SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
    if (!monitor_) {
        monitor_ = CursorMonitor();
    }
    PositionForMonitor(monitor_);
    /* 尺寸档位从「小」变「大」时窗口和分层表面都要长大；强制丢掉旧的
       DIB/D2D 表面再重排，避免只有缩小方向即时生效。 */
    InvalidateSurface();
    needsLayout_ = true;
    Reposition();
    EnsureVisible();
    /* 常驻模式：配置变化后保持展开；快捷键/悬停模式不在这里主动拉开。 */
    if (newDisplayMode == kDisplayAlways && !manuallyHidden_ && expand_ < 0.9f) {
        Expand(false);
    }
    /* 启动后按真实布局收一次：旧版本/迁移过来的自定义卡可能正好压在组上。 */
    if (!startupResolveDone_ && hwnd() && !plugin_->items().empty()) {
        startupResolveDone_ = true;
        ResolveCustomOverlaps();
        Reposition();
        RelaxGroupOffsetsIfNoCustoms();
    }
    Render();
}

/* 轻雪的「停用历史记录」不在插件设置里，宿主切换时也不会回调插件；
   每 50 ms 的轮询里跟一下。变了就重新布局：篮子隐藏，信封右移到它的槽位。 */
void RackWindow::RefreshHistoryDisabled()
{
    if (!plugin_) {
        return;
    }
    const bool disabled = plugin_->HistoryDisabled();
    if (disabled == historyDisabled_) {
        return;
    }
    historyDisabled_ = disabled;
    view_.SetHistoryDisabled(disabled);
    basketHover_ = 0.0f;
    basketHoverVelocity_ = 0.0f;
    basketHoverTarget_ = 0.0f;
    if (hwnd()) {
        Reposition();
        Render();
        /* 信封从旧槽位滑到新槽位：和卡片一样由 16 ms 的动画定时器推进。 */
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
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
    /* 手动隐藏状态下，除了快捷键路径（会先清掉 manuallyHidden_），
       任何自动展开都不能把绳子拉出来。设置页在预览晾衣架时例外：
       用户正在调它，必须把架子显示出来。 */
    const bool settingsPreview = plugin_->PreviewPinned();
    if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover && !settingsPreview) {
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
    lastExpandedMs_ = now;
    idleMemoryReleased_ = false;
    swingUntil_ = now + 2200;
    expanding_ = true;
    if (!hiddenForFullscreen_) {
        EnsureVisible();
        /* 窗口一出现就恢复可命中，不依赖下一次轮询；否则刚展开时右键
           仍可能沿用旧的 WS_EX_TRANSPARENT 状态。 */
        SetClickThrough(false);
    }
    StartAnimation(1.0f);
    if (hwnd()) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

RackWindow* RackWindow::topBarOwner_ = nullptr;

void RackWindow::SetTopBarWatch(bool enabled)
{
    if (!hwnd()) {
        return;
    }
    if (enabled == (topBarHook_ != nullptr)) {
        return;
    }
    if (enabled) {
        topBarOwner_ = this;
        topBarHook_ = SetWindowsHookExW(WH_MOUSE_LL, &RackWindow::TopBarHookProc,
                                        util::GetDllModule(), 0);
        if (!topBarHook_) {
            topBarOwner_ = nullptr;
            RackLog(L"RackWindow: 顶栏点击监测装不上（%lu）", GetLastError());
        }
    } else {
        rightCardDown_ = false;
        if (topBarHook_) {
            UnhookWindowsHookEx(topBarHook_);
            topBarHook_ = nullptr;
        }
        if (topBarOwner_ == this) {
            topBarOwner_ = nullptr;
        }
    }
}

bool RackWindow::IsTopBarPoint(POINT screen) const
{
    if (expand_ <= 0.5f) {
        return false;
    }
    const HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    const LONG band = static_cast<LONG>(std::lround(kTopBarBand * scale_));
    return screen.y >= info.rcMonitor.top && screen.y < info.rcMonitor.top + band &&
           screen.x >= info.rcMonitor.left && screen.x < info.rcMonitor.right;
}

int RackWindow::CardIndexAtScreen(POINT screen) const
{
    if (!hwnd()) return -1;
    POINT client = screen;
    ScreenToClient(hwnd(), &client);
    const float px = static_cast<float>(client.x);
    const float py = static_cast<float>(client.y);
    const RackHit hit = view_.HitTest(px, py, scale_);
    if (hit.index >= 0 && hit.part != RackPart::None && hit.part != RackPart::Background &&
        hit.part != RackPart::Line && hit.part != RackPart::Basket &&
        hit.part != RackPart::Envelope) {
        return hit.index;
    }
    /* 旋转/悬停放大后的卡片边缘用和绘制完全一致的形状再兜一次，
       不再用 10px 的轴对齐包围盒——那会把卡片旁边的空白也吃进来。 */
    const float extra = 2.5f * scale_;
    for (int i = 0;; ++i) {
        const CardBox* box = view_.CardAt(i);
        if (!box) break;
        if (PointInCardShape(*box, px, py, extra) ||
            (box->hasNote && PointInNoteShape(*box, px, py, extra))) {
            return i;
        }
    }
    return -1;
}

bool RackWindow::IsEnvelopeScreenPoint(POINT screen) const
{
    if (!hwnd()) return false;
    POINT client = screen;
    ScreenToClient(hwnd(), &client);
    const RackHit hit = view_.HitTest(static_cast<float>(client.x),
                                      static_cast<float>(client.y), scale_);
    return hit.part == RackPart::Envelope;
}

void RackWindow::ToggleClipboardFromEnvelope()
{
    if (!plugin_ || !hwnd()) return;
    /* 面板的锚点跟着信封走：信封在换槽位的动画里会滑动，所以每次都用
       当前布局重新算，而不是缓存下来的坐标。 */
    const D2D1_POINT_2F anchor = view_.EnvelopeBadgeCenter();
    POINT screen{static_cast<LONG>(std::lround(anchor.x)),
                 static_cast<LONG>(std::lround(anchor.y))};
    ClientToScreen(hwnd(), &screen);
    plugin_->OpenClipboard(screen.x, screen.y);
}

bool RackWindow::PointInCardShape(const CardBox& box, float x, float y, float extra) const
{
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    D2D1_POINT_2F local = RotateAroundPoint(D2D1::Point2F(x, y), pivot, -box.angle);
    /* 反向抵消悬停放大：绘制时卡片以夹子为原点放大到 1.05 倍。 */
    local.x = pivot.x + (local.x - pivot.x) / kCardHoverScale;
    local.y = pivot.y + (local.y - pivot.y) / kCardHoverScale;

    const float left = box.card.left - extra;
    const float top = box.card.top - extra;
    const float right = box.card.right + extra;
    const float bottom = box.card.bottom + extra;
    if (right <= left || bottom <= top) {
        return false;
    }
    float radius = 0.0f;
    if (box.cardType == card::Type::Note) {
        radius = 0.0f; /* 便签是方角纸卡 */
    } else if (box.flat) {
        radius = (box.card.right - box.card.left) / 10.0f;
    } else {
        radius = 12.0f * scale_;
    }
    return RoundedRectContains(local.x, local.y, left, top, right, bottom, radius);
}

bool RackWindow::PointInNoteShape(const CardBox& box, float x, float y, float extra) const
{
    if (!box.hasNote || box.note.right <= box.note.left || box.note.bottom <= box.note.top) {
        return false;
    }
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    const D2D1_POINT_2F local = RotateAroundPoint(D2D1::Point2F(x, y), pivot, -box.angle);
    return local.x >= box.note.left - extra && local.x <= box.note.right + extra &&
           local.y >= box.note.top - extra && local.y <= box.note.bottom + extra;
}

bool RackWindow::PointHittable(POINT screen) const
{
    if (!hwnd()) {
        return false;
    }
    POINT client = screen;
    ScreenToClient(hwnd(), &client);
    if (!AlphaAt(client.x, client.y)) {
        return false;
    }
    /* 堆叠后层、数字胶囊等装饰像素 alpha>0，但不是可操作区域：
       必须再走一次精确命中，避免低级鼠标钩子把它们当成卡片。 */
    const RackHit hit = view_.HitTest(static_cast<float>(client.x),
                                      static_cast<float>(client.y), scale_);
    return hit.part != RackPart::None && hit.part != RackPart::Background &&
           hit.part != RackPart::Line;
}

void RackWindow::CheckMissingFiles()
{
    if (!plugin_ || !hwnd() || dragStarted_ || mouseDown_ || menuOpen_ || editingNote_ ||
        plugin_->MenuOpen()) {
        return;
    }

    std::vector<int> missing;
    const auto& items = plugin_->items();
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const Item& item = items[static_cast<size_t>(i)];
        if (item.kind == 1) {
            continue; /* 便签/正在编辑的不看文件 */
        }
        std::vector<std::wstring> paths = item.paths;
        if (paths.empty() && !item.path.empty()) {
            paths.push_back(item.path);
        }
        if (paths.empty()) {
            continue;
        }
        bool anyExists = false;
        for (const auto& path : paths) {
            if (!path.empty() &&
                GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                anyExists = true;
                break;
            }
        }
        if (!anyExists) {
            missing.push_back(i);
        }
    }

    /* 从后往前移除，前面的索引不受影响；每张卡都走正常的掉落动画。 */
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (*it >= 0 && *it < static_cast<int>(plugin_->items().size())) {
            RackLog(L"RackWindow: 文件已不存在，卡片自动落下: %s",
                    plugin_->items()[static_cast<size_t>(*it)].path.c_str());
            RemoveItem(*it, false);
        }
    }
}

LRESULT CALLBACK RackWindow::TopBarHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    /* 右键在按下阶段就截住：分层窗口偶发命中失败时，这次右键不会先落到桌面。
       判断依据改成「指针下最顶层的窗口是不是晾衣架」：宿主菜单、剪贴板面板、
       预览窗或别的程序盖在卡片上时放行，剩下卡片上的右键一律由晾衣架打开菜单。 */
    if (code == HC_ACTION && (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) &&
        topBarOwner_ && topBarOwner_->hwnd() && topBarOwner_->plugin_) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (!mouse) return CallNextHookEx(nullptr, code, wParam, lParam);
        if (wParam == WM_RBUTTONUP) {
            if (topBarOwner_->rightCardDown_) {
                const bool recent = rack::NowMs() - topBarOwner_->rightCardDownAt_ < 1000;
                topBarOwner_->rightCardDown_ = false;
                topBarOwner_->rightCardDownAt_ = 0;
                if (recent) {
                    return 1;
                }
            }
        } else {
            /* 新的一次右键按下永远重新开始计数。上一轮如果只收到 down
               没收到 up（宿主菜单抢走/钩子重装），不能让标志卡住，
               否则下一次右键的 up 会被吞掉，卡片上的右键就变成死区。 */
            topBarOwner_->rightCardDown_ = false;
            topBarOwner_->rightCardDownAt_ = 0;
            if (!topBarOwner_->menuOpen_ && !topBarOwner_->dragStarted_ &&
                !topBarOwner_->editingNote_) {
                const int index = topBarOwner_->CardIndexAtScreen(mouse->pt);
                if (index >= 0) {
                    /* 只处理真正落在晾衣架上的点：
                       - WindowFromPoint 确认没有菜单/面板/别的窗口压在卡片上；
                       - PointHittable 看该像素 alpha 是否 > 0，空白区域（alpha=0）
                         一律放行，交给下面真正的前台程序。 */
                    const HWND under = WindowFromPoint(mouse->pt);
                    if (under != topBarOwner_->hwnd() ||
                        !topBarOwner_->PointHittable(mouse->pt)) {
                        return CallNextHookEx(nullptr, code, wParam, lParam);
                    }
                    topBarOwner_->rightCardDown_ = true;
                    topBarOwner_->rightCardDownAt_ = rack::NowMs();
                    PostMessageW(topBarOwner_->hwnd(), kMsgRackRightClick,
                                 static_cast<WPARAM>(index),
                                 MAKELPARAM(mouse->pt.x, mouse->pt.y));
                    return 1;
                }
            }
        }
    }
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL && topBarOwner_ &&
        topBarOwner_->hwnd()) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (mouse) {
            POINT client = mouse->pt;
            ScreenToClient(topBarOwner_->hwnd(), &client);
            const RackHit hit = topBarOwner_->view_.HitTest(
                static_cast<float>(client.x), static_cast<float>(client.y),
                topBarOwner_->scale_);
            if ((hit.part == RackPart::Card || hit.part == RackPart::CardColor) &&
                hit.index >= 0 &&
                hit.index < static_cast<int>(topBarOwner_->plugin_->items().size()) &&
                topBarOwner_->plugin_->items()[static_cast<size_t>(hit.index)].cardType ==
                    card::Type::Color) {
                PostMessageW(topBarOwner_->hwnd(), kMsgColorWheel,
                             static_cast<WPARAM>(GET_WHEEL_DELTA_WPARAM(mouse->mouseData)), 0);
                /* 颜色卡吃掉的滚轮不要再传给下面窗口。 */
                return 1;
            }
            /* 其它位置：鼠标在绳子/卡片/堆叠上时，滚轮切换可见窗口。 */
            if (topBarOwner_->view_.RackAreaContains(static_cast<float>(client.x),
                                                     static_cast<float>(client.y))) {
                PostMessageW(topBarOwner_->hwnd(), kMsgRackScroll,
                             static_cast<WPARAM>(GET_WHEEL_DELTA_WPARAM(mouse->mouseData)), 0);
                return 1;
            }
        }
    }
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && topBarOwner_ &&
        topBarOwner_->hwnd()) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (mouse && topBarOwner_->IsTopBarPoint(mouse->pt)) {
            PostMessageW(topBarOwner_->hwnd(), kMsgTopBarClick, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void RackWindow::ResolveCustomOverlaps(int ignoreId)
{
    if (!plugin_ || !hwnd()) {
        return;
    }
    auto& items = plugin_->items();
    /* 只把「真的压住了」的自定义卡并回槽位。落点规则允许卡片停在两卡之间
       （两侧各留不到 2G 也合法），这里再用 2G 判定会把刚落下的卡又弹回槽位，
       看起来就是“松手后突然位移”。 */
    constexpr float kOverlapTolerance = 1.0f;
    /* 在“内容坐标”里检查：槽位目标和自定义卡的 customX 是同一套坐标，
       和居中/滚动/弹簧无关，所以这个判断是稳定的、不依赖当前画面。 */
    for (int pass = 0; pass < 8; ++pass) {
        int groupCount = 0;
        for (const auto& item : items) {
            if (!item.custom) {
                ++groupCount;
            }
        }
        if (groupCount >= static_cast<int>(items.size())) {
            return;
        }
        std::vector<Item> group;
        group.reserve(static_cast<size_t>(groupCount));
        for (int g = 0; g < groupCount; ++g) {
            group.push_back(items[static_cast<size_t>(g)]);
        }
        const std::vector<float> targets = view_.TargetLefts(group);
        bool joined = false;
        for (int i = groupCount; i < static_cast<int>(items.size()) && !joined; ++i) {
            if (!items[static_cast<size_t>(i)].custom) {
                continue;
            }
            if (items[static_cast<size_t>(i)].id == ignoreId) {
                continue;   /* 刚放下的卡：它就是要停在那里 */
            }
            const float customLeft = items[static_cast<size_t>(i)].customX;
            const float customWidth = view_.CardWidth(items[static_cast<size_t>(i)]);
            bool conflict = false;
            for (int g = 0; g < groupCount && !conflict; ++g) {
                /* 叠组里的卡画在固定叠位、不按槽位；自定义卡又画在叠组上面，
                   拿它们的槽位判重叠会把卡误收进组里。 */
                if (view_.IsStackIndex(g)) {
                    continue;
                }
                /* 吸附组可能为了让路而带着偏移：按（含缓动目标的）实际位置判断。 */
                const float gLeft = targets[static_cast<size_t>(g)] +
                                    PendingGroupOffset(group[static_cast<size_t>(g)].id,
                                                       group[static_cast<size_t>(g)].rackOffsetX);
                const float gRight = gLeft + view_.CardWidth(group[static_cast<size_t>(g)]);
                const float gapBetween = std::max(customLeft, gLeft) -
                                         std::min(customLeft + customWidth, gRight);
                if (gapBetween < kOverlapTolerance) {
                    conflict = true;
                }
            }
            for (int j = groupCount; j < i && !conflict; ++j) {
                if (!items[static_cast<size_t>(j)].custom) {
                    continue;
                }
                const float oLeft = items[static_cast<size_t>(j)].customX;
                const float oRight = oLeft + view_.CardWidth(items[static_cast<size_t>(j)]);
                const float gapBetween = std::max(customLeft, oLeft) -
                                         std::min(customLeft + customWidth, oRight);
                if (gapBetween < kOverlapTolerance) {
                    conflict = true;
                }
            }
            if (!conflict) {
                continue;
            }
            const float center = customLeft + customWidth * 0.5f;
            int at = 0;
            for (int k = 0; k < groupCount; ++k) {
                const float otherCenter =
                    targets[static_cast<size_t>(k)] +
                    view_.CardWidth(group[static_cast<size_t>(k)]) * 0.5f;
                if (otherCenter < center) {
                    ++at;
                }
            }
            if (at < 0) {
                at = 0;
            }
            if (at > groupCount) {
                at = groupCount;
            }
            const CardBox* before = view_.Card(i);
            const float visual = before ? before->card.left : 0.0f;
            Item moved = items[static_cast<size_t>(i)];
            moved.custom = false;
            moved.rackOffsetX = 0.0f;
            items.erase(items.begin() + i);
            items.insert(items.begin() + at, moved);
            std::vector<Item> groupAfter;
            groupAfter.reserve(static_cast<size_t>(groupCount + 1));
            for (int g = 0; g <= groupCount && g < static_cast<int>(items.size()); ++g) {
                groupAfter.push_back(items[static_cast<size_t>(g)]);
            }
            const std::vector<float> after = view_.TargetLefts(groupAfter);
            const float slotScreen = view_.ScreenLeftForContentLeft(after[static_cast<size_t>(at)]);
            if (Item* item = plugin_->ItemById(moved.id)) {
                item->rackOffsetX = visual - slotScreen;
                view_.SnapSlotForId(item->id, after[static_cast<size_t>(at)]);
                offsetAnims_.push_back(
                    {item->id, item->rackOffsetX, 0.0f, rack::NowMs(), 260.0f});
            }
            joined = true;
        }
        if (!joined) {
            return;
        }
    }
}

void RackWindow::FinishCardDrag(int id)
{
    if (!plugin_) {
        return;
    }
    auto& items = plugin_->items();
    const int dragIndex = plugin_->IndexById(id);
    Item* dragged = plugin_->ItemById(id);
    if (!dragged || dragIndex < 0) {
        return;
    }
    const float gap = view_.DefaultGap();
    const float width = view_.CardWidth(*dragged);
    const bool wasCustom = dragged->custom;
    /* 当前吸附组（不含自定义卡）和每张卡的屏幕左边缘。 */
    std::vector<Item> group;
    for (const auto& item : items) {
        if (!item.custom) {
            group.push_back(item);
        }
    }
    const std::vector<float> groupTargets = view_.TargetLefts(group);
    std::vector<float> groupLefts(group.size(), 0.0f);
    for (size_t g = 0; g < group.size(); ++g) {
        groupLefts[g] = view_.ScreenLeftForContentLeft(groupTargets[g]) + group[g].rackOffsetX;
    }
    const int dragGroupPos = [&]() -> int {
        if (wasCustom) {
            return -1;
        }
        for (size_t g = 0; g < group.size(); ++g) {
            if (group[g].id == id) {
                return static_cast<int>(g);
            }
        }
        return -1;
    }();
    float dropLeft = 0.0f;
    if (wasCustom) {
        dropLeft = view_.ScreenLeftForContentLeft(dragged->customX);
    } else if (dragGroupPos >= 0 &&
               dragGroupPos < static_cast<int>(groupLefts.size())) {
        dropLeft = groupLefts[static_cast<size_t>(dragGroupPos)];
    } else {
        return;
    }
    const float dropCenter = dropLeft + width * 0.5f;
    struct Visual {
        int groupPos = -1;
        int itemIndex = -1;
        bool custom = false;
        float left = 0.0f;
        float w = 0.0f;
    };
    std::vector<Visual> others;
    for (size_t g = 0; g < group.size(); ++g) {
        if (!wasCustom && static_cast<int>(g) == dragGroupPos) {
            continue;
        }
        others.push_back({static_cast<int>(g), static_cast<int>(g), false, groupLefts[g],
                          view_.CardWidth(group[g])});
    }
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (!items[static_cast<size_t>(i)].custom || items[static_cast<size_t>(i)].id == id) {
            continue;
        }
        others.push_back({-1, i, true, view_.ScreenLeftForContentLeft(items[static_cast<size_t>(i)].customX),
                          view_.CardWidth(items[static_cast<size_t>(i)])});
    }
    /* 落点中心压在谁身上。叠组里的卡片按实际绘制位置计算，不按槽位：
       它们的槽位在屏幕外，按槽位判断会并入不可见的位置。 */
    int coveredGroupPos = -1;
    int coveredItemIndex = -1;
    for (const Visual& v : others) {
        if (dropCenter > v.left && dropCenter < v.left + v.w) {
            if (v.custom) {
                coveredItemIndex = v.itemIndex;
                break;
            }
            if (view_.IsStackIndex(v.groupPos)) {
                continue;
            }
            coveredGroupPos = v.groupPos;
            break;
        }
    }
    /* 落点中心压在谁身上：压住卡片 => 并回吸附组（按落点重排）；
       落在空档/绳子空白处 => 就停在落点当自定义位置，两侧不够放时让开。 */
    int centerInsert = 0;
    int splitIndex = 0;
    for (const Visual& v : others) {
        const float c = v.left + v.w * 0.5f;
        if (c < dropCenter && !v.custom) {
            /* centerInsert 用于组内插入下标（含叠组）；splitIndex 只数可见行的卡，
               叠组本来就不跟着 rackOffsetX 动。 */
            ++centerInsert;
            if (!view_.IsStackIndex(v.groupPos)) {
                ++splitIndex;
            }
        }
    }
    const bool onCard = coveredGroupPos >= 0 || coveredItemIndex >= 0;
    /* 只挪开一点点就松手：回自己拖拽前的位置（可见卡按槽位、叠卡按叠组位置）。 */
    const bool backToOwnSlot = !onCard && !wasCustom && dragGroupPos >= 0 &&
                               std::abs(dropLeft - cardDragStartLeft_) <= gap * 0.75f;
    bool joinGroup = false;
    int insertAt = -1;
    if (onCard) {
        joinGroup = true;
        if (coveredGroupPos >= 0) {
            const float coveredCenter =
                groupLefts[static_cast<size_t>(coveredGroupPos)] +
                view_.CardWidth(group[static_cast<size_t>(coveredGroupPos)]) * 0.5f;
            insertAt = coveredGroupPos + (dropCenter > coveredCenter ? 1 : 0);
            if (!wasCustom && dragGroupPos >= 0 && dragGroupPos < coveredGroupPos) {
                --insertAt;
            }
        } else {
            /* 压在一张自定义卡上：插到落点左右最近的组内位置（others 已经不含被拖卡）。 */
            insertAt = centerInsert;
        }
    } else if (backToOwnSlot) {
        joinGroup = true;
        insertAt = dragGroupPos;
    }
    /* 先让其他卡片未完成的偏移缓动到达终点，再执行重排，避免停在中间位置。 */
    SettleOffsetAnims();
    const uint64_t now = rack::NowMs();
    if (!joinGroup) {
        /* 自定义位置：留在落点，不占槽位；不能停到信封下面。 */
        float customLeft = dropLeft;
        const float maxScreen = view_.AccessoryLeft() - gap - width;
        if (customLeft > maxScreen) {
            customLeft = maxScreen;
        }
        Item* item = plugin_->ItemById(id);
        if (!item) {
            return;
        }
        item->custom = true;
        item->rackOffsetX = 0.0f;
        /* 落点按这张卡离开吸附组后的居中基准换算：下一次 Layout 会按新内容重新居中，
           同一帧按新基准绘制，松手位置不会偏移。 */
        std::vector<Item> remaining;
        remaining.reserve(items.size());
        for (const auto& other : items) {
            if (!other.custom && other.id != id) {
                remaining.push_back(other);
            }
        }
        const float remainingWidth = view_.ContentWidthForItems(remaining);
        const float rowWidthNow = view_.layout().rowRight - view_.layout().rowLeft;
        const float baseNow = view_.ProjectedContentBase(remainingWidth);
        const float maxScrollNow = std::max(0.0f, remainingWidth - rowWidthNow);
        float scrollNow = std::min(view_.ScrollTargetX(), maxScrollNow);
        if (view_.AtScrollEnd()) {
            scrollNow = maxScrollNow;
        }
        item->customX = customLeft - baseNow + scrollNow;
        if (!wasCustom) {
            Item moved = *item;
            items.erase(items.begin() + dragIndex);
            items.push_back(moved);
        }
        /* 落点两侧空间不足时，吸附组向两侧让开，卡片才能停在落点。 */
        PushGroupAside(id, splitIndex, customLeft, customLeft + width);
    } else {
        std::vector<Item> newGroup;
        newGroup.reserve(group.size());
        for (size_t g = 0; g < group.size(); ++g) {
            if (!wasCustom && static_cast<int>(g) == dragGroupPos) {
                continue;
            }
            newGroup.push_back(group[g]);
        }
        if (insertAt < 0) {
            insertAt = 0;
        }
        if (insertAt > static_cast<int>(newGroup.size())) {
            insertAt = static_cast<int>(newGroup.size());
        }
        Item moved = *plugin_->ItemById(id);
        moved.custom = false;
        moved.rackOffsetX = 0.0f;
        newGroup.insert(newGroup.begin() + insertAt, moved);
        std::vector<Item> suffix;
        for (const auto& item : items) {
            if (item.custom && item.id != id) {
                suffix.push_back(item);
            }
        }
        items.clear();
        items.insert(items.end(), newGroup.begin(), newGroup.end());
        items.insert(items.end(), suffix.begin(), suffix.end());
        const std::vector<float> newTargets = view_.TargetLefts(newGroup);
        /* 用当前基准算槽位屏幕位置：Layout 会为居中基准变化补偿 slot.pos，
           这里再按新基准算会把补偿算两遍，动画起点会偏一截。 */
        const float slotScreen = view_.ScreenLeftForContentLeft(newTargets[static_cast<size_t>(insertAt)]);
        if (Item* item = plugin_->ItemById(id)) {
            item->rackOffsetX = dropLeft - slotScreen;
            view_.SnapSlotForId(id, newTargets[static_cast<size_t>(insertAt)]);
            /* 拖动距离较小时回到拖拽前的位置（保留原有偏移）；
               并回吸附组时偏移归零（落在槽位）。
               不延迟：偏移与槽位弹簧同时开始，卡片从落点直接回到目标位置。 */
            const float settleOffset = backToOwnSlot ? cardDragStartOffset_ : 0.0f;
            offsetAnims_.push_back({id, item->rackOffsetX, settleOffset, now, 260.0f});
        }
    }
    needsLayout_ = true;
    if (hwnd()) {
        Reposition();
        /* 按最新的目标布局检查自定义卡：真叠住了才并回槽位（刚放下的这张除外）。 */
        ResolveCustomOverlaps(id);
        Reposition();
        Render();
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
    /* 一张自定义卡都不剩时，把让位偏移收回槽位，绳子恢复成一排。 */
    RelaxGroupOffsetsIfNoCustoms();
    /* 拿到最新的 customX/偏移后再存：重启后自定义位置才对得上。 */
    plugin_->SaveItems();
    if (const CardBox* box = view_.Card(plugin_->IndexById(id))) {
        RackLog(L"拖拽落位：id=%d 松手左缘=%.0f 布局左缘=%.0f custom=%d", id,
                static_cast<double>(dropLeft), static_cast<double>(box->card.left),
                plugin_->ItemById(id) ? plugin_->ItemById(id)->custom : 0);
    }
}

void RackWindow::ToggleVisibility()
{
    if (!plugin_) {
        return;
    }
    const int mode = plugin_->config().displayMode;
    if (mode != kDisplayHover) {
        /* 快捷键/常驻模式：纯手动开关，不受悬停和自动收回影响。 */
        if (!manuallyHidden_ && (expanding_ || expanded())) {
            manuallyHidden_ = true;
            Collapse();
        } else {
            manuallyHidden_ = false;
            collapseAfterPreview_ = false;
            EnsureVisible();
            Expand(false);
        }
        return;
    }
    if (expanding_ || expanded()) {
        hotkeyPinned_ = false;
        Collapse();
        return;
    }
    EnsureVisible();
    // 在指针真正到达晾衣架之前保持收起；之后离开才按常规计时隐藏。
    hotkeyPinned_ = true;
    Expand(false);
}

void RackWindow::SetDragTransparent(bool on)
{
    if (!hwnd()) {
        return;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd(), GWL_EXSTYLE);
    const LONG_PTR updated = on ? (style | WS_EX_TRANSPARENT) : (style & ~WS_EX_TRANSPARENT);
    if (updated != style) {
        SetWindowLongPtrW(hwnd(), GWL_EXSTYLE, updated);
        SetWindowPos(hwnd(), nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
    }
}

void RackWindow::ShakeCard(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    Item& item = plugin_->items()[static_cast<size_t>(index)];
    const uint64_t now = rack::NowMs();
    item.shakeStartMs = now;
    item.shakeAmp = 4.5f; /* 比「让位」时的轻晃明显，明确提示「已经挂上了」 */
    shakeUntil_ = std::max(shakeUntil_, now + 1200);
    if (hwnd()) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

void RackWindow::Collapse()
{
    if (plugin_ && plugin_->PreviewPinned()) {
        /* 设置页的「晾衣架外观」正在 pin 住预览：现在收会被宿主立刻拉开。
           记下来，等预览结束（rack.preview 变 false）再补一次收起。 */
        collapseAfterPreview_ = true;
        return;
    }
    collapseAfterPreview_ = false;
    if (view_.HasRemoved()) {
        return;
    }
    /* 收起动画期间卡片还在画面上，仍然要能接住右键；
       整窗穿透只留到完全收起之后（见 Animate）。 */
    missingChecked_ = false;
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
    /* 拖拽期间画面完全冻结：OLE 的拖拽循环就跑在这个 UI 线程上，
       任何重绘/改窗口位置都会让光标和落点发涩，甚至把拖拽打断。 */
    if (dragStarted_) {
        return;
    }
    const uint64_t now = rack::NowMs();
    if (pendingPinKind_ != PendingPin::None && now >= pendingPinAt_) {
        const PendingPin kind = pendingPinKind_;
        pendingPinKind_ = PendingPin::None;
        const std::wstring path = std::move(pendingPinPath_);
        const std::wstring text = std::move(pendingPinText_);
        const std::vector<std::wstring> paths = std::move(pendingPinPaths_);
        pendingPinPath_.clear();
        pendingPinText_.clear();
        pendingPinPaths_.clear();
        if (plugin_) {
            if (kind == PendingPin::Image) {
                plugin_->PinImageFileAt(path, pendingPinAnchor_.x, pendingPinAnchor_.y);
            } else if (kind == PendingPin::Text) {
                plugin_->PinTextToDesktopAt(text, pendingPinAnchor_.x, pendingPinAnchor_.y);
            } else if (kind == PendingPin::Files) {
                plugin_->PinFilesToDesktopAt(paths, pendingPinAnchor_.x, pendingPinAnchor_.y);
            }
        }
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
            const uint64_t elapsed = now > it->start ? now - it->start : 0;
            const float t = rack::Clamp(
                static_cast<float>(elapsed) / std::max(1.0f, it->ms), 0.0f, 1.0f);
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
    if (normalizeAt_ != 0 && now >= normalizeAt_) {
        normalizeAt_ = 0;
        NormalizeSpacing(240.0f);
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
    StepSpring(cardHover_, cardHoverVelocity_, cardHoverTarget_, dt, 0.18f, 0.72f);

    /* 信封里那张纸只有一层，弹簧仅跟踪一个值。 */
    const float paperTarget = envelopeHoverTarget_ > 0.5f ? 1.0f : 0.0f;
    StepSpring(paperPos_, paperVelocity_, paperTarget, dt, 0.38f, 0.80f);
    const bool paperSettled = std::abs(paperPos_ - paperTarget) < 0.003f;

    if (cardScaleAnimating_) {
        const float t = rack::Clamp(
            static_cast<float>(now - cardScaleAnimStart_) / 220.0f, 0.0f, 1.0f);
        if (t >= 1.0f) {
            cardScaleValue_ = cardScaleTarget_;
            cardScaleAnimating_ = false;
        } else {
            cardScaleValue_ =
                rack::Lerp(cardScaleFrom_, cardScaleTarget_, rack::EaseOutCubic(t));
        }
    }

    Reposition();
    Render();

    /* 展开、收起动画期间都挂着：卡片还在画面上时，右键都必须先被这里接住。
       完全收起后由 Animate 摘掉，顶边点击的「拉下来」不需要钩子。 */
    SetTopBarWatch(expand_ > 0.05f);

    const bool hoverSettled =
        std::abs(basketHover_ - basketHoverTarget_) < 0.003f &&
        std::abs(cardHover_ - cardHoverTarget_) < 0.003f && paperSettled;
    if (!animating_ && !cardScaleAnimating_ && offsetAnims_.empty() && pendingAdjustId_ < 0 &&
        now >= swingUntil_ && now >= shakeUntil_ && hoverSettled && !view_.HasRemoved() &&
        !view_.HasSliding() && normalizeAt_ == 0 &&
        !view_.HasArrival() && pendingPinKind_ == PendingPin::None) {
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
        missingChecked_ = false;
        /* 快捷键/常驻模式被手动关掉：收起动画结束后把窗口一起隐藏，
           下次只能靠快捷键重新显示。 */
        if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover) {
            Hide();
        }
    }
    /* 每次完全展开后只查一次文件是否还在；不在就按正常掉落动画取下。
       不做定时轮询：用户要求「下次展开时检查」。 */
    if (!animating_ && expand_ >= 0.99f && !missingChecked_) {
        missingChecked_ = true;
        CheckMissingFiles();
    }
}

void RackWindow::SettleOffsetAnims()
{
    const bool had = !offsetAnims_.empty();
    if (plugin_) {
        for (const auto& anim : offsetAnims_) {
            if (Item* item = plugin_->ItemById(anim.id)) {
                item->rackOffsetX = anim.to;
            }
        }
    }
    offsetAnims_.clear();
    /* 动画被提前取消时，偏移已到达最终值；此时保存，重启后卡片不会停在中间位置。 */
    if (had && plugin_) {
        plugin_->SaveItems();
    }
}

float RackWindow::PendingGroupOffset(int id, float fallback) const
{
    for (const auto& anim : offsetAnims_) {
        if (anim.id == id) {
            return anim.to;
        }
    }
    return fallback;
}

void RackWindow::RelaxGroupOffsetsIfNoCustoms()
{
    if (!plugin_ || !hwnd()) {
        return;
    }
    for (const auto& item : plugin_->items()) {
        if (item.custom) {
            return;   /* 还有自定义卡：让位偏移继续保留 */
        }
    }
    const uint64_t now = rack::NowMs();
    bool animated = false;
    for (auto& item : plugin_->items()) {
        if (std::abs(item.rackOffsetX) < 0.5f) {
            continue;
        }
        bool pending = false;
        for (const auto& anim : offsetAnims_) {
            if (anim.id == item.id) {
                pending = true;
                break;
            }
        }
        if (pending) {
            continue;
        }
        offsetAnims_.push_back({item.id, item.rackOffsetX, 0.0f, now + 40, 260.0f});
        animated = true;
    }
    if (animated) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
}

void RackWindow::PushGroupAside(int skipId, int splitIndex, float obstacleLeft,
                                float obstacleRight)
{
    if (!plugin_ || !hwnd()) {
        return;
    }
    auto& items = plugin_->items();
    std::vector<Item> pushGroup;
    pushGroup.reserve(items.size());
    for (int index = 0; index < static_cast<int>(items.size()); ++index) {
        const Item& item = items[static_cast<size_t>(index)];
        if (item.custom || item.id == skipId) {
            continue;
        }
        /* 叠组绘制在固定位置，不跟随 rackOffsetX：为它们计算让位没有意义，
           还会影响可见行的让位量。 */
        if (view_.IsStackIndex(index)) {
            continue;
        }
        pushGroup.push_back(item);
    }
    if (pushGroup.empty()) {
        return;
    }
    const int total = static_cast<int>(pushGroup.size());
    const int split = rack::ClampInt(splitIndex, 0, total);
    const float gap = view_.DefaultGap();
    const std::vector<float> targets = view_.TargetLefts(pushGroup);
    const float contentWidth = view_.ContentWidthForItems(pushGroup);
    const float rowWidth = view_.layout().rowRight - view_.layout().rowLeft;
    /* 内容变窄时布局会锚在旧基准上，让位必须按同一个基准算。 */
    const float base = view_.ProjectedContentBase(contentWidth);
    /* 卡片离开吸附组后内容变窄：滚动会回到新的末端（和平时删卡一致）。
       让位按最终位置算，松手后卡片才不会先让开再被卷动带回去。 */
    const float maxScroll = std::max(0.0f, contentWidth - rowWidth);
    float scroll = std::min(view_.ScrollTargetX(), maxScroll);
    if (view_.AtScrollEnd()) {
        scroll = maxScroll;
    }
    std::vector<float> natural(pushGroup.size(), 0.0f);
    std::vector<float> widths(pushGroup.size(), 0.0f);
    std::vector<float> existing(pushGroup.size(), 0.0f);
    for (size_t g = 0; g < pushGroup.size(); ++g) {
        existing[g] = pushGroup[g].rackOffsetX;
        natural[g] = base + targets[g] - scroll + existing[g];
        widths[g] = view_.CardWidth(pushGroup[g]);
    }
    /* 左边一组从落点往左依次排开，右边一组往右，保证相邻至少一个 gap。 */
    const std::vector<float> offsets =
        rack::PushApartOffsets(natural, widths, split, obstacleLeft, obstacleRight, gap);
    const uint64_t now = rack::NowMs();
    bool animated = false;
    for (size_t g = 0; g < pushGroup.size() && g < offsets.size(); ++g) {
        const float targetOffset = existing[g] + offsets[g];
        Item* item = plugin_->ItemById(pushGroup[g].id);
        if (!item) {
            continue;
        }
        if (std::abs(targetOffset - item->rackOffsetX) < 0.5f) {
            continue;
        }
        offsetAnims_.push_back({item->id, item->rackOffsetX, targetOffset, now + 40, 260.0f});
        animated = true;
    }
    if (animated) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
        RackLog(L"拖拽让位：落点=%.0f..%.0f 两侧让开", static_cast<double>(obstacleLeft),
                static_cast<double>(obstacleRight));
    }
}

void RackWindow::NormalizeSpacing(float ms)
{
    if (!plugin_ || !hwnd()) {
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
    const uint64_t now = rack::NowMs();
    float previousRight = 0.0f;
    bool first = true;
    bool changed = false;
    for (const auto& entry : order) {
        const CardBox* box = view_.Card(entry.index);
        if (!box) continue;
        float left = box->card.left;
        const float width = box->card.right - box->card.left;
        if (!first && left - previousRight < gap) {
            const float shift = gap - (left - previousRight);
            Item& item = plugin_->items()[static_cast<size_t>(entry.index)];
            item.rackOffsetX += shift;
            offsetAnims_.push_back(
                {item.id, item.rackOffsetX - shift, item.rackOffsetX, now, ms});
            left += shift;
            changed = true;
        }
        previousRight = left + width;
        first = false;
    }
    if (changed) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
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
                                           cardScaleValue_);
    const int width = std::max(1, static_cast<int>(std::lround(layout.width)));
    int height = std::max(1, static_cast<int>(std::lround(layout.height)));
    if (view_.HasFalling() || view_.HasArrival()) {
        /* 只加高到动画真正会用到的位置：整个工作区作为窗口时，每帧要上传
           2560x1032 的位图，拖动时会明显卡顿。 */
        const int needed = static_cast<int>(std::ceil(view_.AnimationHeight()));
        const int limit = static_cast<int>(work.bottom - work.top);
        height = std::max(height, std::min(needed, limit));
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
    UpdateMaxTotal();
}

void RackWindow::UpdateMaxTotal()
{
    if (!plugin_ || !hwnd() || updatingMaxTotal_) {
        return;
    }
    const int capacity = std::max(1, view_.Capacity());
    const int maxTotal = plugin_->config().maxMode == 0 ? capacity : 0;
    updatingMaxTotal_ = true;
    plugin_->SetMaxTotal(maxTotal);
    updatingMaxTotal_ = false;
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
    /* 快捷键/常驻模式下被快捷键隐藏时，任何自动路径都不能再把窗口拉出来。
       设置页正在预览晾衣架时例外，否则用户打开设置看不到架子。 */
    if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover &&
        !plugin_->PreviewPinned()) {
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
    /* 拖拽期间不做悬停判定、不重绘：这段时间 CPU 应该全留给 OLE 拖拽。 */
    if (dragStarted_) {
        lastInside_ = rack::NowMs();
        return;
    }
    /* 菜单命令可能打开模态编辑器/取色器；在命令真正跑完前
       不要让自动收回先把绳子拉上去。 */
    if (menuActionPending_) {
        lastInside_ = rack::NowMs();
        return;
    }
    RefreshHistoryDisabled();
    plugin_->SyncHostTheme();
    if (menuOpen_) {
        return;
    }
    const bool hostMenuOpen = plugin_->MenuOpen();
    if (hostMenuWasOpen_ && !hostMenuOpen) {
        hostMenuWasOpen_ = false;
        // 同时把 PinMenu 的低级鼠标钩子摘掉（如果它比弹窗活得久的话），
        // 否则它会一直吞掉桌面上的点击。
        plugin_->CloseMenu();
        /* 菜单关闭和命令回调之间可能有几十毫秒空档。这里不能立刻
           Collapse，否则点“编辑”会先看到卡片收回、编辑器再展开。
           给命令一个宽限期，真正的收起交给 autoHide。 */
        lastInside_ = rack::NowMs();
        menuCloseGraceUntil_ = lastInside_ + 500;
        /* 菜单开着的那段时间如果窗口被设成穿透，这里立刻恢复；
           否则紧接着的右键会直接落到桌面。 */
        UpdateClickThrough();
        Render();
        return;
    }
    if (hostMenuOpen) {
        hostMenuWasOpen_ = true;
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        /* 菜单是独立的置顶窗口，不需要把晾衣架整窗变穿透。
           以前这里 SetClickThrough(true)，菜单一开卡片就真的点不到了：
           紧接着右键另一张卡时，这次点击会穿透到桌面，弹出系统右键菜单。 */
        SetClickThrough(false);
        lastInside_ = rack::NowMs();
        return;
    }
    if (plugin_->ClipboardOpen()) {
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        lastInside_ = rack::NowMs();
        /* 面板只在面板窗口自己的矩形里优先（面板是置顶窗口，盖住的地方
           本来就轮不到晾衣架；万一被晾衣架盖住，WM_NCHITTEST 会在那一块
           返回 HTTRANSPARENT）。面板之外的卡片仍然要能正常右键，
           所以这里不能再把整个晾衣架窗口设成穿透。 */
        POINT cursor{};
        GetCursorPos(&cursor);
        if (plugin_->ClipboardContains(cursor)) {
            return;
        }
    }

    if (editingNote_) {
        /* 便签编辑器开着：绳子一直展开，编辑卡片从夹子上变出来。 */
        if (!visible()) {
            EnsureVisible();
        }
        if (expand_ < 0.9f) {
            Expand(false);
        }
        lastInside_ = rack::NowMs();
        return;
    }

    if (plugin_->PreviewPinned()) {
        /* 设置页正在预览晾衣架：快捷键/常驻模式下即使手动隐藏过，也要拉出来；
           用户正在调它，看不到架子就没法调。 */
        settingsPreviewShown_ = true;
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        lastInside_ = rack::NowMs();
    } else {
        if (settingsPreviewShown_) {
            settingsPreviewShown_ = false;
            /* 预览结束：之前如果是快捷键/常驻模式下手动隐藏的，回到隐藏状态。 */
            if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover) {
                collapseAfterPreview_ = true;
            }
        }
        if (collapseAfterPreview_) {
            /* 设置页的预览刚结束：把之前因为 pin 住而没执行的那次收起补上。 */
            collapseAfterPreview_ = false;
            Collapse();
        }
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

    /* 收起满 20 s 就把「展开才用得到」的素材位图和 2x 表面放掉，并 trim 一次工作集：
       常驻内存能回落到接近刚启动的水平；下次展开由 LoadAssets 重新加载（几十毫秒）。 */
    if (!idleMemoryReleased_ && lastExpandedMs_ != 0 && expand_ <= 0.001f && !animating_ &&
        !mouseDown_ && !dragStarted_ && !preview_.showing() &&
        rack::NowMs() - lastExpandedMs_ > 20000) {
        idleMemoryReleased_ = true;
        view_.ReleaseCachedImages();
        /* 卡片缩略图同理：下次展开由 BeforeDraw 重新加载。 */
        if (plugin_) {
            for (auto& item : plugin_->items()) {
                item.thumb.Reset();
                item.thumbAttempted = false;
            }
        }
        InvalidateSurface();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                 static_cast<SIZE_T>(-1));
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
        const int mode = plugin_->config().displayMode;
        if (!manuallyHidden_ &&
            (mode == kDisplayAlways || (mode == kDisplayHotkey && shownBeforeFullscreen_))) {
            Expand(false);
        }
        shownBeforeFullscreen_ = false;
        Render();
    } else if (IsFullscreenOn(monitor_)) {
        DismissPreview();
        SetTopBarWatch(false);
        shownBeforeFullscreen_ = !manuallyHidden_ && (expanding_ || expand_ > 0.5f);
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
    const RackLayout& layout = view_.layout();
    /* 热区横向只到绳子两端（含 6px 余量），和展开后的判定保持一致。
       原来收起时按整屏宽度算，屏幕左上/右上角也算热区：在角落展开后
       又因为不在绳子范围内被判成「离开」，收起来再弹，来回抖。 */
    const float lineLeft = layout.lineStart - 6.0f * s;
    const float lineRight = layout.lineEnd + 6.0f * s;
    if (point.x < lineLeft || point.x > lineRight) {
        return false;
    }
    if (expand_ < 0.3f) {
        /* 收起时热区是一条贴着屏幕顶边、和菜单栏同高的窄条。 */
        const float hot = 10.0f * s;
        return point.y >= 0 && point.y <= hot;
    }
    const float lineHitBottom = layout.lineY + layout.sag + 8.0f * s;
    return point.y >= 0 && point.y <= lineHitBottom;
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
    /* 顶栏点击收回之后，指针往往还停在顶边上：在它离开之前不要再自动拉下来，
       否则一松手就又弹回来了。 */
    if (!overLine) {
        topBarSuppressed_ = false;
    }
    const bool overContent = CursorOverContent();
    const bool overPreview = PointOverPreview(cursor);
    const bool inside = overLine || overContent || overPreview;
    const bool hoverMode = plugin_->config().displayMode == kDisplayHover;

    if (hoverMode && overLine && !topBarSuppressed_ && expand_ < 0.3f) {
        if (!lineHover_) {
            lineHover_ = true;
            hoverLineSince_ = now;
        }
        // 必须有明确的停留才把绳子拉下来；否则只是去点菜单栏时蹭一下顶边就触发了。
        const int delay = std::max(600, plugin_->config().hoverDelayMs);
        if (now - hoverLineSince_ >= static_cast<uint64_t>(delay)) {
            Expand(false);
        }
    } else if (!overLine || !hoverMode) {
        lineHover_ = false;
    }

    if (inside) {
        lastInside_ = now;
        // 快捷键打开的晾衣架，在指针真正进入后就变成普通可交互的晾衣架，
        // 之后离开时按常规收起。
        hotkeyPinned_ = false;
    }

    if (plugin_->config().autoRetract && expanding_ && !inside && !mouseDown_ &&
        !dragStarted_ && !plugin_->PreviewPinned() && !hotkeyPinned_ &&
        !view_.HasRemoved()) {
        const uint64_t autoHide = static_cast<uint64_t>(std::max(0, plugin_->config().autoHideMs));
        const uint64_t hideAt =
            std::max(std::max(peekUntil_, lastInside_ + autoHide), menuCloseGraceUntil_);
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
    if (!hwnd()) {
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd(), GWL_EXSTYLE);
    const bool actual = (style & WS_EX_TRANSPARENT) != 0;
    if (actual == enabled && clickThrough_ == enabled) {
        return;
    }
    const LONG_PTR next =
        enabled ? (style | WS_EX_TRANSPARENT) : (style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (next != style) {
        SetWindowLongPtrW(hwnd(), GWL_EXSTYLE, next);
    }
    /* WS_EX_TRANSPARENT 在分层窗口上偶尔要重新算一次非客户区才生效。 */
    SetWindowPos(hwnd(), nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    clickThrough_ = enabled;
}

void RackWindow::UpdateClickThrough()
{
    /* 只有完全收起、画面上什么都不剩时才整窗穿透；展开/收起动画期间
       卡片仍然看得见，必须留给命中测试，否则右键会掉到桌面。
       透明缝隙由逐像素 alpha + WM_NCHITTEST 负责。 */
    const bool keepMouse = mouseDown_ || dragStarted_;
    SetClickThrough(!keepMouse && expand_ <= 0.001f);
}

void RackWindow::UpdateHoverFromMouse()
{
    if (!plugin_ || dragStarted_ || mouseDown_) {
        return;
    }
    const uint64_t now = rack::NowMs();
    if (lastHoverPollMs_ != 0 && now - lastHoverPollMs_ < 8) {
        return;
    }
    lastHoverPollMs_ = now;
    UpdatePreview();
}

void RackWindow::UpdatePreview()
{
    if (!plugin_ || dragStarted_ || mouseDown_) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);

    RackHit hit = HitAtCursor();
    /* 鼠标停在叠组上滚轮时，不实时把某张叠卡提出来（悬停/预览都先按背景处理）。 */
    if (hit.index >= 0 && view_.IsStackIndex(hit.index) && view_.IsScrolling()) {
        hit.part = RackPart::Background;
        hit.index = -1;
        hit.colorIndex = -1;
    }
    const RackPart part = hit.part;
    const int index = hit.index;
    const int colorIndex = part == RackPart::CardColor ? hit.colorIndex : -1;
    const int colorPart = part == RackPart::CardColor ? hit.colorPart : -1;
    const bool overPreview = PointOverPreview(cursor);
    const uint64_t now = rack::NowMs();
    UpdateAccessoryHover(hit);
    UpdateClickThrough();
    bool visualChanged = false;

    /* 卡片上的锁定/关闭小控件：把光标压在哪个上告诉 view，做悬停高亮。 */
    const bool hoverChanged =
        static_cast<int>(part) != lastHoverPart_ || index != lastHoverPartIndex_ ||
        colorIndex != lastHoverColorIndex_ || colorPart != lastHoverColorPart_;
    if (hoverChanged) {
        lastHoverPart_ = static_cast<int>(part);
        lastHoverPartIndex_ = index;
        lastHoverColorIndex_ = colorIndex;
        lastHoverColorPart_ = colorPart;
        view_.SetHoverPart(lastHoverPart_, lastHoverPartIndex_);
        view_.SetHoverColor(lastHoverColorIndex_, lastHoverColorPart_);
        visualChanged = true;
    }

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
            plugin_->HoverPreviewEnabled() && !plugin_->ClipboardOpen() && index >= 0 &&
            index < static_cast<int>(plugin_->items().size())) {
            const Item& hoverItem = plugin_->items()[static_cast<size_t>(index)];
            const bool previewable =
                hoverItem.kind == 0 ||
                (hoverItem.kind == 2 && !hoverItem.thumbPath.empty());
            if (previewable) ShowPreviewFor(index);
        } else if (preview_.showing() && previewIndex_ == index) {
            RECT anchor{};
            const POINT origin = screenOrigin();
            const CardBox* box = view_.DisplayCard(index);
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
        hit.part == RackPart::Close || hit.part == RackPart::Lock ||
        hit.part == RackPart::CardLink || hit.part == RackPart::CardColor) {
        return hit.index;
    }
    return -1;
}

D2D1_RECT_F RackWindow::CardScreenRect(int index) const
{
    const CardBox* box = view_.DisplayCard(index);
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
    /* 单色颜色卡：整卡单击默认复制 hex（带 alpha 时是 8 位）。多色卡整卡复制原文。 */
    if (copyItem.cardType == card::Type::Color) {
        std::vector<card::Rgba> colors;
        if (card::ParseColors(copyItem.note, colors) && colors.size() == 1) {
            clipboard::CopyText(card::HexText(colors.front()));
            view_.ShowCopyToast(index, rack::NowMs() + 900);
            if (hwnd()) SetTimer(hwnd(), kTimerCopyToast, 900, nullptr);
            Render();
            return;
        }
    }
    /* SVG 卡片复制的是 SVG 代码本身，粘贴到支持矢量代码的设计软件里仍是矢量；
       不另存 .svg 文件。 */
    const std::wstring svgText = plugin_->SvgText(index);
    if (!svgText.empty()) {
        clipboard::CopyText(svgText);
    } else if (copyItem.kind == 1) {
        clipboard::CopyText(copyItem.note);
    } else if (copyItem.kind == 0) {
        // 和拖拽保持同一条路径：只给 CF_HDROP，目标软件按「粘贴文件」导入 PNG，
        // 透明通道由文件本身带；位图剪贴板可能被优先取走并洗成白底。
        clipboard::CopyFilePath(copyItem.path);
    } else if (!copyItem.paths.empty()) {
        if (copyItem.paths.size() > 1) {
            clipboard::CopyFilePaths(copyItem.paths);
        } else {
            clipboard::CopyFilePath(copyItem.paths.front());
        }
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

void RackWindow::OpenCardLink(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) return;
    const Item& item = plugin_->items()[static_cast<size_t>(index)];
    if (item.cardType != card::Type::Link || item.note.empty()) return;
    /* 单击已经打开过，双击不再重复开第二个标签页。 */
    const uint64_t now = rack::NowMs();
    if (lastLinkOpenIndex_ == index &&
        now - lastLinkOpenMs_ < static_cast<uint64_t>(GetDoubleClickTime())) {
        return;
    }
    lastLinkOpenIndex_ = index;
    lastLinkOpenMs_ = now;
    ShellExecuteW(hwnd(), L"open", item.note.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RackWindow::CopyCardColor(int index, int colorIndex, int part)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) return;
    const Item& item = plugin_->items()[static_cast<size_t>(index)];
    std::vector<card::Rgba> colors;
    if (!card::ParseColors(item.note, colors) || colorIndex < 0 ||
        colorIndex >= static_cast<int>(colors.size())) {
        return;
    }
    const card::Rgba& color = colors[static_cast<size_t>(colorIndex)];
    const std::wstring text = part == 2 ? card::RgbText(color) : card::HexText(color);
    clipboard::CopyText(text);
    const uint64_t now = rack::NowMs();
    view_.ShowCopyToast(index, now + 900);
    view_.ShowColorFlash(item.id, colorIndex, part == 2 ? 2 : 1, now + 900);
    if (hwnd()) SetTimer(hwnd(), kTimerCopyToast, 900, nullptr);
    Render();
}

bool RackWindow::ScrollColorCard(int index, int delta)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return false;
    }
    Item& item = plugin_->items()[static_cast<size_t>(index)];
    if (item.cardType != card::Type::Color) return false;
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(hwnd(), &cursor);
    if (!view_.ColorContentContains(index, static_cast<float>(cursor.x),
                                    static_cast<float>(cursor.y))) {
        return false;
    }
    std::vector<card::Rgba> colors;
    if (!card::ParseColors(item.note, colors) || colors.empty()) return false;
    const float cardScale = plugin_->config().CardScale();
    const float rowHeight = 36.0f * scale_ * cardScale;
    const float bodyHeight = (150.0f - 33.0f) * scale_ * cardScale;
    const float maxScroll = std::max(
        0.0f, static_cast<float>(colors.size()) * rowHeight + 4.0f * scale_ * cardScale -
                  bodyHeight);
    if (maxScroll <= 0.0f) return false;
    const int steps = delta / WHEEL_DELTA;
    if (steps == 0) return false;
    item.colorScroll =
        rack::Clamp(item.colorScroll - static_cast<float>(steps) * rowHeight, 0.0f, maxScroll);
    Reposition();
    Render();
    return true;
}

void RackWindow::PickCardColor(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) return;
    Item& item = plugin_->items()[static_cast<size_t>(index)];
    if (!card::HasAccent(item.cardType)) return;
    const uint32_t current = item.accentSet ? item.accent : card::DefaultAccent(item.cardType);
    const std::wstring initial = card::HexText(card::Rgba{
        static_cast<uint8_t>((current >> 16) & 0xFF),
        static_cast<uint8_t>((current >> 8) & 0xFF),
        static_cast<uint8_t>(current & 0xFF), 255});
    std::wstring text = initial;
    NoteEditor::CardLook look;
    look.type = item.cardType;
    look.accent = current;
    look.accentSet = true;
    look.cardScale = plugin_->config().CardScale();
    look.colorMode = true;
    Theme uiTheme = plugin_->host()
                        ? Theme::HostUi(plugin_->host(), plugin_->host()->user)
                        : Theme::MenuStyle(plugin_->theme().dark);
    const D2D1_RECT_F card = CardScreenRect(index);
    const RECT anchor{ static_cast<LONG>(std::lround(card.left)),
                       static_cast<LONG>(std::lround(card.top)),
                       static_cast<LONG>(std::lround(card.right)),
                       static_cast<LONG>(std::lround(card.bottom)) };
    if (!NoteEditor::Edit(hwnd(), util::GetDllModule(), &plugin_->graphics(), uiTheme,
                          L"颜色", anchor, initial, text, nullptr, nullptr, nullptr, look)) {
        return;
    }
    std::vector<card::Rgba> colors;
    if (!card::ParseColors(text, colors) || colors.empty()) {
        return;
    }
    const card::Rgba& picked = colors.front();
    item.accent = (static_cast<uint32_t>(picked.r) << 16) |
                  (static_cast<uint32_t>(picked.g) << 8) |
                  static_cast<uint32_t>(picked.b);
    item.accentSet = true;
    plugin_->SaveItems();
    Render();
}

void RackWindow::OpenItem(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    /* 便签（只有文字、没有文件路径）没有可交给 shell 打开的对象；
       ShellExecuteW 收到空路径会退化成打开资源管理器。 */
    const std::wstring& path = plugin_->items()[index].path;
    if (path.empty()) {
        return;
    }
    util::OpenInShell(path);
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
    /* 篮子被「停用历史记录」藏起来时没有可落的地方，卡片改成从绳上掉落。 */
    if (toBasket && (historyDisabled_ || plugin_->HistoryDisabled())) {
        toBasket = false;
    }
    view_.BeginRemove(index, plugin_->items()[index], toBasket);
    SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    /* 自定义位置保留：删掉一张卡后不要把其他卡弹回槽位。 */
    SettleOffsetAnims();
    plugin_->RemoveItemAt(index);
    /* 位置动画由 RackView 的弹簧负责（目标位置按索引算，卡片通过弹簧平滑过渡），
       这里只要保证让位动画在跑。 */
    Reposition();
    ResolveCustomOverlaps();
    Reposition();
    Render();
    RelaxGroupOffsetsIfNoCustoms();
}
void RackWindow::QueueEditNote(int index)
{
    pendingEditIndex_ = index;
    if (hwnd()) {
        SetTimer(hwnd(), kTimerEditStart, 30, nullptr);
    }
}

void RackWindow::EditNote(int index)
{
    if (!plugin_ || index < 0 || index >= static_cast<int>(plugin_->items().size())) {
        return;
    }
    DismissPreview();
    const Item& item = plugin_->items()[index];
    const int noteColor = item.noteColor;
    const bool textLike = item.cardType == card::Type::Note ||
                          item.cardType == card::Type::Text ||
                          item.cardType == card::Type::Link ||
                          item.cardType == card::Type::Color;
    const bool fileLike = item.cardType == card::Type::File ||
                          item.cardType == card::Type::Video ||
                          item.cardType == card::Type::Music;
    const bool readOnly = fileLike;
    const bool hideCard = textLike || fileLike;
    std::vector<std::wstring> filePaths = item.paths;
    if (filePaths.empty() && !item.path.empty()) {
        filePaths.push_back(item.path);
    }
    const bool foldersOnly = card::AllPathsAreFolders(filePaths);
    std::wstring title = item.cardType == card::Type::Image
                             ? L"给这张截图写一句用途"
                             : card::TypeLabel(item.cardType);
    if (item.cardType == card::Type::File) {
        title = card::FileLabel(filePaths);
    }
    RECT anchor{};
    const D2D1_RECT_F card = CardScreenRect(index);
    if (card.right > card.left && card.bottom > card.top) {
        anchor.left = static_cast<LONG>(std::lround(card.left));
        anchor.top = static_cast<LONG>(std::lround(card.top));
        anchor.right = static_cast<LONG>(std::lround(card.right));
        anchor.bottom = static_cast<LONG>(std::lround(card.bottom));
    }
    std::wstring text = item.note;
    if (fileLike) {
        /* 全是文件夹时显示完整地址；文件/混搭只显示名字。 */
        text.clear();
        if (!filePaths.empty()) {
            for (const auto& path : filePaths) {
                if (!text.empty()) text += L'\n';
                text += foldersOnly ? path : card::FileName(path);
            }
        } else if (!item.label.empty()) {
            text = item.label;
        }
    }
    if (hideCard) {
        /* 文本和文件都用同一套编辑卡片：第一帧出来后才把绳子上的卡片藏掉。 */
        editingNote_ = true;
        Expand(false);
    }
    NoteEditor::CardLook look;
    look.type = item.cardType;
    look.accent = item.accentSet ? item.accent : card::DefaultAccent(item.cardType);
    look.accentSet = true;
    look.createdAt = item.createdAt;
    look.cardScale =
        item.cardType == card::Type::Image ? 1.0f : plugin_->config().CardScale();
    look.colorMode = card::HasAccent(item.cardType);
    look.readOnly = readOnly;
    Theme uiTheme = plugin_->host()
                        ? Theme::HostUi(plugin_->host(), plugin_->host()->user)
                        : Theme::MenuStyle(plugin_->theme().dark);
    const bool saved = NoteEditor::Edit(
        hwnd(), util::GetDllModule(), &plugin_->graphics(), uiTheme, title, anchor, text, text,
        [this, index, hideCard]() {
            if (!hideCard) {
                return;
            }
            view_.SetEditingCard(index);
            Render();
        },
        nullptr,
        /* 卡片已经返回夹子：先把下面这张卡片画出来，编辑器再淡出。 */
        [this, hideCard]() {
            if (!hideCard) {
                return;
            }
            view_.SetEditingCard(-1);
            Render();
        },
        look,
        [this, index](uint32_t rgb) {
            if (!plugin_ || index < 0 ||
                index >= static_cast<int>(plugin_->items().size())) {
                return;
            }
            Item& edited = plugin_->items()[static_cast<size_t>(index)];
            edited.accent = rgb & 0xFFFFFFu;
            edited.accentSet = true;
            plugin_->SaveItems();
            Render();
        });
    if (hideCard) {
        view_.SetEditingCard(-1);
        editingNote_ = false;
    }
    if (saved) {
        if (!fileLike && index >= 0 &&
            index < static_cast<int>(plugin_->items().size())) {
            plugin_->SetItemNote(index, text, noteColor);
        }
        hovered_ = -1;
        lastInside_ = rack::NowMs();
        Expand(true);
    }
    Render();
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
    const Item& menuEntry = plugin_->items()[index];
    const bool noteItem = menuEntry.path.empty() && !menuEntry.note.empty();
    const bool textLike = menuEntry.cardType == card::Type::Note ||
                          menuEntry.cardType == card::Type::Text ||
                          menuEntry.cardType == card::Type::Link ||
                          menuEntry.cardType == card::Type::Color;
    const bool accentable = card::HasAccent(menuEntry.cardType);
    const bool canEditImage = plugin_->host() && plugin_->host()->api_version >= 6 &&
                              plugin_->host()->open_image_editor;
    const bool imageItem = (menuEntry.cardType == card::Type::Image ||
                            menuEntry.cardType == card::Type::Svg) && canEditImage;

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

        SnowAirMenuItem main[14]{};
        int n = 0;
        auto add = [&](const wchar_t* label, int id, int flags = 0) {
            main[n].label = label;
            main[n].id = id;
            main[n].flags = flags;
            ++n;
        };
        add(L"打开", kMenuOpen);
        if (imageItem) add(L"图片编辑", kMenuImageEdit);
        else if (textLike || accentable) add(L"编辑", kMenuEdit);
        add(imageItem ? L"图片另存" : L"另存为…", kMenuSaveAs);
        if (!noteItem) add(L"打开位置", kMenuShowInFolder);
        add(L"", 0, SNOWAIR_MENU_SEPARATOR);
        add(L"取下内容", kMenuRemove);
        add(L"批量取下", kMenuBatch);
        main[n - 1].submenu = batch;
        main[n - 1].submenu_count = 4;
        /* 便签记录归轻雪：没有「销毁」这一项；没有这一项时也不要留一条
           多余的分隔线。 */
        if (!noteItem) {
            add(L"", 0, SNOWAIR_MENU_SEPARATOR);
            add(L"移至回收站", kMenuRecycle);
        }

        menuIndex_ = index;
        plugin_->host()->show_menu(plugin_->host()->user, main, n, screen.x, screen.y,
                                   this, &RackWindow::MenuSelected);
        /* 从这一刻起就知道是宿主菜单。菜单弹在自己窗口上面时，
           第一次落在卡片上的左键只负责把菜单关掉，不要再触发复制。 */
        hostMenuWasOpen_ = true;
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
    auto separator = [&]() {
        MenuItem entry;
        entry.kind = MenuItem::Kind::Separator;
        items.push_back(std::move(entry));
    };

    action(L"打开", kMenuOpen);
    if (imageItem) action(L"图片编辑", kMenuImageEdit);
    else if (textLike || accentable) action(L"编辑", kMenuEdit);
    action(imageItem ? L"图片另存" : L"另存为…", kMenuSaveAs);
    if (!noteItem) action(L"打开位置", kMenuShowInFolder);
    separator();
    action(L"取下内容", kMenuRemove);
    action(L"批量取下  ›", kMenuBatch);
    if (!noteItem) {
        separator();
        action(L"移至回收站", kMenuRecycle);
    }

    const bool menuDark = plugin_->host() && plugin_->host()->is_dark
                              ? plugin_->host()->is_dark(plugin_->host()->user) != 0
                              : plugin_->theme().dark;
    menuOpen_ = true;
    const POINT menuPoint = screen;
    menu_.Show(items, screen, workArea_, scale_, Theme::MenuStyle(menuDark),
               [this, index, menuPoint](int command) {
                   menuActionPending_ = true;
                   if (command == kMenuBatch) {
                       this->menu_.Close();
                       this->ShowBatchMenu(index, menuPoint);
                   } else {
                       this->HandleMenuCommand(index, command);
                   }
                   menuActionPending_ = false;
               },
               [this]() {
                   menuOpen_ = false;
                   lastInside_ = rack::NowMs();
                   menuCloseGraceUntil_ = lastInside_ + 500;
               });
    if (!menu_.showing()) {
        menuOpen_ = false;
    }
}

void RackWindow::MenuSelected(void* user, int id)
{
    auto* window = static_cast<RackWindow*>(user);
    if (window) {
        window->menuActionPending_ = true;
        window->HandleMenuCommand(window->menuIndex_, id);
        window->menuActionPending_ = false;
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
                    [this, index](int command) {
                        menuActionPending_ = true;
                        HandleMenuCommand(index, command);
                        menuActionPending_ = false;
                    },
                    [this]() {
                        menuOpen_ = false;
                        lastInside_ = rack::NowMs();
                        menuCloseGraceUntil_ = lastInside_ + 500;
                    });
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
               [this](int command) {
                   menuActionPending_ = true;
                   HandleMenuCommand(-1, command);
                   menuActionPending_ = false;
               },
               [this]() {
                   menuOpen_ = false;
                   lastInside_ = rack::NowMs();
                   menuCloseGraceUntil_ = lastInside_ + 500;
               });
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
    const bool textLike = menuItem.cardType == card::Type::Note ||
                          menuItem.cardType == card::Type::Text ||
                          menuItem.cardType == card::Type::Link ||
                          menuItem.cardType == card::Type::Color;
    if (command == kMenuImageEdit) {
        if (!menuItem.path.empty() && plugin_->host() &&
            plugin_->host()->api_version >= 6 &&
            plugin_->host()->open_image_editor) {
            plugin_->host()->open_image_editor(plugin_->host()->user, menuItem.path.c_str());
        }
        return;
    }
    if (command == kMenuOpen) {
        if (menuItem.cardType == card::Type::Link) {
            OpenCardLink(index);
        } else if (textLike) {
            QueueEditNote(index);
        } else {
            OpenItem(index);
        }
    } else if (command == kMenuEdit) {
        QueueEditNote(index);
    } else if (command == kMenuEditColor) {
        PickCardColor(index);
    } else if (command == kMenuSaveAs) {
        SaveItemAs(index);
    } else if (command == kMenuShowInFolder) {
        if (!noteItem) {
            util::ShowInExplorer(menuItem.sourcePath.empty() ? menuItem.path : menuItem.sourcePath);
        }
    } else if (command == kMenuRemove) {
        RemoveItem(index);
    } else if (command == kMenuRecycle) {
        if (!noteItem && RecyclePath(menuItem.path)) {
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
    } else if (command == kMenuNote) {
        QueueEditNote(index);
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
    /* 卡片缩略图缓存放在轻雪的缓存目录（<Cache>\Misc\Rack），跟着「缓存位置」设置走；
       RackThumbDir 还会把旧位置的缩略图搬过来。 */
    const std::wstring thumbDir = util::RackThumbDir();
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
        std::wstring source = !item.thumbPath.empty() ? item.thumbPath : item.path;
        if (!item.thumbPath.empty() && !util::FileExists(source)) {
            source = item.path;   /* 宿主缩略图被清掉了：退回原图 */
        }
        if (source.empty()) {
            continue;
        }
        UINT w = 0, h = 0;
        if (SUCCEEDED(graphics_->images().GetSize(source, w, h)) && w > 0 && h > 0) {
            const float aspect = static_cast<float>(w) / static_cast<float>(h);
            if (std::abs(aspect - item.aspect) > 0.01f) {
                item.aspect = aspect;
                needsLayout_ = true;
            }
        }
        /* 卡片只画小图：第一次从原图缩出一份 512px 的 PNG 缓存，之后（包括下次
           启动）直接读缓存，卡片多时不再反复解 4K 原图；悬停预览仍然读原图。
           如果轻雪给了 thumb（录屏封面/宿主缩略图），source 就是它。 */
        std::wstring cardSource = source;
        if (!thumbDir.empty()) {
            const std::wstring cacheFile =
                util::JoinPath(thumbDir, ThumbCacheKey(item) + L".png");
            const std::wstring cached =
                graphics_->images().EnsureThumbnail(source, cacheFile, 512);
            if (!cached.empty()) {
                cardSource = cached;
            }
        }
        /* 卡片图只有约 140 DIP 宽。用 WIC Fant 先按接近显示尺寸缩小，
           比绘制时让 D2D 缩小 640px 的缩略图更干净——那样会
           把细线糊成一团。 */
        graphics_->images().Load(cardSource, 192.0f * scale_, target, item.thumb);
    }
}

void RackWindow::AfterDraw(HDC dc, void* bits, int width, int height)
{
    (void)dc;
    if (!plugin_ || !bits || width <= 0 || height <= 0) {
        return;
    }
    auto* pixels = static_cast<BYTE*>(bits);
    /* 给卡片本身补一层「能看见但看不见」的 alpha=1 底：
       分层窗口的命中测试只看 alpha，圆角/贴边/锁定按钮附近的 AA 像素
       可能是 alpha=0，右键会直接穿过窗口落到桌面。
       只补卡片真正画出来的形状（含旋转和悬停放大）外加 2.5px，
       卡片旁边的空白不补，仍然 alpha=0 交给下面的程序。 */
    const float extra = 2.5f * scale_;
    const int margin = static_cast<int>(std::ceil(26.0f * scale_));
    auto setOpaque = [&](int x, int y) {
        BYTE* pixel = pixels + (static_cast<size_t>(y) * static_cast<size_t>(width) +
                                static_cast<size_t>(x)) *
                                   4u;
        if (pixel[3] == 0) {
            pixel[3] = 1;
        }
    };

    auto fillBox = [&](const CardBox& box) {
        const int x0 = std::max(0, static_cast<int>(std::floor(box.card.left)) - margin);
        const int y0 = std::max(0, static_cast<int>(std::floor(box.card.top)) - margin);
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(box.card.right)) + margin);
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(box.card.bottom)) + margin);

        const D2D1_POINT_2F pivot =
            D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
        const float radians = box.angle * 3.1415926535f / 180.0f;
        const float ca = std::cos(radians);
        const float sa = std::sin(radians);
        const float left = box.card.left - extra;
        const float top = box.card.top - extra;
        const float right = box.card.right + extra;
        const float bottom = box.card.bottom + extra;
        float radius = 0.0f;
        if (box.cardType == card::Type::Note) {
            radius = 0.0f;
        } else if (box.flat) {
            radius = (box.card.right - box.card.left) / 10.0f;
        } else {
            radius = 12.0f * scale_;
        }

        for (int y = y0; y <= y1; ++y) {
            const float dy = static_cast<float>(y) + 0.5f - pivot.y;
            for (int x = x0; x <= x1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - pivot.x;
                /* 反向旋转（-angle）+ 反向悬停放大，得到卡片本地坐标。
                   每张卡只算一次三角函数，逐像素只做乘加。 */
                const float lx = pivot.x + dx * ca + dy * sa;
                const float ly = pivot.y - dx * sa + dy * ca;
                if (RoundedRectContains(pivot.x + (lx - pivot.x) / kCardHoverScale,
                                        pivot.y + (ly - pivot.y) / kCardHoverScale, left, top,
                                        right, bottom, radius) ||
                    (box.hasNote && lx >= box.note.left - extra &&
                     lx <= box.note.right + extra && ly >= box.note.top - extra &&
                     ly <= box.note.bottom + extra)) {
                    setOpaque(x, y);
                }
            }
        }
    };

    /* 只给可见行和堆叠顶层补 alpha：堆叠后层只画，不参与鼠标命中。 */
    const RackLayout& layoutNow = view_.layout();
    const int first = rack::ClampInt(layoutNow.visibleFirst, 0,
                                     static_cast<int>(plugin_->items().size()));
    const int last = std::min(static_cast<int>(plugin_->items().size()),
                              first + layoutNow.visibleCount);
    for (int i = first; i < last; ++i) {
        if (const CardBox* box = view_.CardAt(i)) {
            fillBox(*box);
        }
    }
    /* 自定义位置的卡片也要参与命中测试。 */
    for (int i = std::max(0, layoutNow.groupCount); i < static_cast<int>(plugin_->items().size());
         ++i) {
        if (const CardBox* box = view_.CardAt(i)) {
            fillBox(*box);
        }
    }
    /* 叠层也要参与命中测试（悬停其中一张会临时提到前面），
       所以每一层的形状都补 alpha；后层被前层盖住的部分仍由 HitTest 先判前层。 */
    for (const CardBox& box : view_.StackBoxes(true)) {
        fillBox(box);
    }
    for (const CardBox& box : view_.StackBoxes(false)) {
        fillBox(box);
    }

    /* 绳子本身只有 1.6~2.3px 宽，AA 之后真正 alpha=255 的像素更少。
       沿绳子的下垂曲线补 ±2px 的命中带；带子以外的空白仍然是 alpha=0。 */
    const RackLayout& layout = view_.layout();
    if (layout.expanded && layout.lineEnd > layout.lineStart) {
        const float span = layout.lineEnd - layout.lineStart;
        /* 展开动画里绳子整体带着 retract 偏移，命中带也要跟着走，
           否则动画途中右键会从绳子上漏下去。 */
        const float retract = (1.0f - rack::Clamp(expand_, 0.0f, 1.0f)) *
                              -(layout.lineY + layout.sag + 24.0f * scale_);
        const int x0 = std::max(0, static_cast<int>(std::floor(layout.lineStart - 6.0f * scale_)));
        const int x1 = std::min(width - 1,
                                static_cast<int>(std::ceil(layout.lineEnd + 6.0f * scale_)));
        const int band = std::max(1, static_cast<int>(std::lround(2.0f * scale_)));
        for (int x = x0; x <= x1; ++x) {
            const float t = rack::Clamp((static_cast<float>(x) - layout.lineStart) / span, 0.0f,
                                        1.0f);
            const float y = layout.lineY + retract + 4.0f * layout.sag * t * (1.0f - t);
            const int y0 = std::max(0, static_cast<int>(std::floor(y)) - band);
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil(y)) + band);
            for (int row = y0; row <= y1; ++row) {
                setOpaque(x, row);
            }
        }
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
        // 宿主提供的 dark 标记在启动或切换主题时可能尚未更新，
        // 这时「背景色角色」才是最可靠的判断依据。
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
    const int historyCount = historyDisabled_ ? 0 : plugin_->HistoryCount();
    view_.Draw(target, plugin_->items(), plugin_->theme(), expand_, swaySeconds, basketHover_,
               paperPos_, cardHover_, cardHoverIndex_,
               hovered_, mouseDown_ && !dragStarted_ ? pressed_ : -1,
               historyCount, plugin_->ClipboardCount());
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
            view_.ClearColorFlash();
            Render();
            return 0;
        case kTimerFakeCursor:
            dragdrop::SetDragCursor();
            return 0;
        case kTimerEditStart: {
            KillTimer(hwnd(), kTimerEditStart);
            const int index = pendingEditIndex_;
            pendingEditIndex_ = -1;
            if (index >= 0) {
                EditNote(index);
            }
            return 0;
        }
        default:
            break;
        }
        break;

    case kMsgRackRightClick: {
        /* 低层鼠标钩子在右键落到桌面之前先截住，避免偶发穿透。 */
        const int index = static_cast<int>(wParam);
        if (plugin_ && !menuOpen_ && index >= 0 &&
            index < static_cast<int>(plugin_->items().size())) {
            const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ShowItemMenu(index, screen);
        }
        return 0;
    }

    case kMsgTopBarClick:
        /* 顶栏被点了一下：用户已经用完，立即收回。
           便签编辑器开着时不动——那时候绳子本来就要一直展开。 */
        topBarSuppressed_ = true;
        if (expand_ > 0.5f && !editingNote_) {
            if (plugin_ && plugin_->config().displayMode != kDisplayHover) {
                /* 快捷键/常驻模式：顶栏点击也是一次手动隐藏。 */
                manuallyHidden_ = true;
            }
            Collapse();
        }
        return 0;

    case kMsgColorWheel: {
        /* 低层鼠标钩子把滚轮从「前台窗口」抢过来：颜色卡数量超过 3 个时，
           指针放在卡上就能滚动查看。 */
        const RackHit hit = HitAtCursor();
        if ((hit.part == RackPart::Card || hit.part == RackPart::CardColor) &&
            ScrollColorCard(hit.index, static_cast<int>(wParam))) {
            return 0;
        }
        return 0;
    }

    case kMsgRackScroll: {
        /* 滚轮 = 拉绳子：连续像素移动，一格不再固定切一张卡。 */
        DismissPreview();
        const int delta = static_cast<int>(wParam);
        const float pixels = -static_cast<float>(delta) / 120.0f * 52.0f * scale_;
        if (pixels != 0.0f) {
            view_.ScrollByPixels(pixels);
            if (hwnd()) {
                SetTimer(hwnd(), kTimerAnim, 16, nullptr);
            }
        }
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCHITTEST: {
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT point = screen;
        ScreenToClient(hwnd(), &point);
        if (expand_ <= 0.001f) {
            return HTTRANSPARENT;
        }
        /* 剪贴板面板是独立置顶窗口。如果它被晾衣架压住（某些操作会把
           晾衣架重新提到最上面），在面板矩形里让出点击，避免面板被
           「全宽晾衣架」挡住；不能用 WS_EX_TRANSPARENT 整窗让，
           那会把卡片上的右键一起送进桌面。 */
        if (plugin_ && plugin_->ClipboardOpen() && plugin_->ClipboardContains(screen)) {
            return HTTRANSPARENT;
        }
        /* 卡片边缘用和绘制一致的形状命中（含旋转/悬停放大），
           不再用 10px 轴对齐包围盒——卡片旁边的空白要留给下面的程序。 */
        const float px = static_cast<float>(point.x);
        const float py = static_cast<float>(point.y);
        const float extra = 2.5f * scale_;
        const RackLayout& layoutNow = view_.layout();
        const int shapeFirst = rack::ClampInt(layoutNow.visibleFirst, 0,
                                              static_cast<int>(plugin_->items().size()));
        const int shapeLast = std::min(static_cast<int>(plugin_->items().size()),
                                       shapeFirst + layoutNow.visibleCount);
        for (int i = shapeFirst; i < shapeLast; ++i) {
            const CardBox* box = view_.CardAt(i);
            if (!box) break;
            if (PointInCardShape(*box, px, py, extra) ||
                (box->hasNote && PointInNoteShape(*box, px, py, extra))) {
                return HTCLIENT;
            }
        }
        for (bool left : {true, false}) {
            const CardBox* top = view_.StackTop(left);
            if (top && (PointInCardShape(*top, px, py, extra) ||
                        (top->hasNote && PointInNoteShape(*top, px, py, extra)))) {
                return HTCLIENT;
            }
        }
        const RackHit hit = view_.HitTest(px, py, scale_);
        if (hit.part == RackPart::None || hit.part == RackPart::Background) {
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }

    case WM_SETCURSOR: {
        const RackHit hit = HitAtCursor();
        /* 只有真正能点的东西才给手型：绳子本身只是装饰。
           便签的夹子点了没任何效果，也不给点击提示（光标保持箭头）。 */
        /* 卡片本体用普通箭头；只有链接正文、颜色块和真正的按钮给手型。 */
        bool clickable = hit.part == RackPart::Close || hit.part == RackPart::Lock ||
                         hit.part == RackPart::Basket || hit.part == RackPart::Envelope ||
                         hit.part == RackPart::CardLink || hit.part == RackPart::CardColor;
        if (hit.part == RackPart::Clip && plugin_ && hit.index >= 0 &&
            hit.index < static_cast<int>(plugin_->items().size())) {
            const Item& item = plugin_->items()[static_cast<size_t>(hit.index)];
            clickable = CanPinItem(item);
        }
        if (clickable) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        /* 落在窗口上但不可点：明确返回箭头，避免下层窗口的光标透出。 */
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }

    case WM_LBUTTONDOWN: {
        if (!plugin_) {
            return 0;
        }
        /* 宿主的右键菜单刚被这一下左键关掉：这次点击只用来收菜单，
           不要再顺手复制/拖动卡片。 */
        if (hostMenuWasOpen_ && !plugin_->MenuOpen()) {
            hostMenuWasOpen_ = false;
            UpdateClickThrough();
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
            ToggleClipboardFromEnvelope();
            return 0;
        }
        if (hit.part == RackPart::Card || hit.part == RackPart::Note ||
            hit.part == RackPart::CardLink || hit.part == RackPart::CardColor) {
            if (hit.index < 0 || hit.index >= static_cast<int>(plugin_->items().size())) {
                return 0;
            }
            pressed_ = hit.index;
            pressItemId_ = plugin_->items()[hit.index].id;
            pressedPart_ = hit.part;
            pressColorIndex_ = hit.colorIndex;
            pressColorPart_ = hit.colorPart;
            mouseDown_ = true;
            dragStarted_ = false;
            pressPoint_ = point;
            cardDragStartOffset_ = plugin_->items()[hit.index].rackOffsetX;
            if (const CardBox* display = view_.DisplayCard(hit.index)) {
                cardDragStartLeft_ = display->card.left;
            }
            /* 一开始拖就收掉悬停预览和高亮：否则预览框会挡在拖动路径上。 */
            DismissPreview();
            hovered_ = -1;
            cardHoverTarget_ = 0.0f;
            cardHoverVelocity_ = 0.0f;
            lastHoverPart_ = static_cast<int>(RackPart::None);
            lastHoverPartIndex_ = -1;
            view_.SetHoverPart(lastHoverPart_, lastHoverPartIndex_);
            cardDragging_ = false;
            view_.SetFreeDragging(-1);
            lastCardDragPaint_ = 0;
            pendingAdjustId_ = -1;
            /* 上一次松手的偏移缓动可能还没跑完：先落到终点再开始新拖动，
               否则取消 timer 后那张卡会永远停在半路。 */
            SettleOffsetAnims();
            SetCapture(hwnd());
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
        if (!plugin_) return 0;
        if (!mouseDown_ || dragStarted_) {
            /* hover 跟鼠标走：不再等 50ms 轮询。 */
            UpdateHoverFromMouse();
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

        /* 横向为主 = 沿绳子移动并重排；纵向为主 = 拖出去。
           颜色行（色块/hex/rgb）单独处理成单色拖出，不参与整卡重排。 */
        const float horizontalRun = static_cast<float>(dx);
        const float verticalRun = static_cast<float>(dy);
        const bool horizontalDominant =
            horizontalRun > 12.0f * scale_ && horizontalRun >= verticalRun * 1.3f;
        const bool cardPress = pressedPart_ == RackPart::Card;
        if (cardPress && horizontalDominant && !cardDragging_) {
            const int index = plugin_->IndexById(pressItemId_);
            if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
                cardDragging_ = true;
                /* 叠层里的卡也能直接拖：从它现在露在叠组里的位置起手，
                   用 rackOffsetX 把它抬到可见行这一层，之后跟着指针走。 */
                Item& item = plugin_->items()[index];
                const std::vector<float> targets = view_.TargetLefts(plugin_->items());
                if (index < static_cast<int>(targets.size())) {
                    item.rackOffsetX =
                        cardDragStartLeft_ - view_.ScreenLeftForContentLeft(targets[index]);
                }
                view_.SetFreeDragging(index);
            }
        }
        if (cardDragging_) {
            const int index = plugin_->IndexById(pressItemId_);
            if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
                if (const CardBox* box = view_.Card(index)) {
                    const float width = box->card.right - box->card.left;
                    float desiredLeft = cardDragStartLeft_ + (point.x - pressPoint_.x);
                    const float minLeft = 24.0f * scale_;
                    const float maxLeft = view_.layout().width - width - 24.0f * scale_;
                    desiredLeft = rack::Clamp(desiredLeft, minLeft, std::max(minLeft, maxLeft));
                    /* 拖动中只让卡片跟着指针走，先不挤邻居。 */
                    if (Item* moved = plugin_->ItemById(pressItemId_)) {
                        if (moved->custom) {
                            moved->customX = view_.ContentXForScreenLeft(desiredLeft);
                        } else {
                            const int groupCount = view_.GroupCount();
                            std::vector<Item> group;
                            for (int g = 0; g < groupCount; ++g) {
                                group.push_back(plugin_->items()[static_cast<size_t>(g)]);
                            }
                            const std::vector<float> targets = view_.TargetLefts(group);
                            if (index < static_cast<int>(targets.size())) {
                                const float slotLeft = view_.ScreenLeftForContentLeft(
                                    targets[static_cast<size_t>(index)]);
                                moved->rackOffsetX = desiredLeft - slotLeft;
                            }
                        }
                    }
                }
            }
            const uint64_t paintNow = rack::NowMs();
            if (lastCardDragPaint_ == 0 || paintNow - lastCardDragPaint_ >= 12) {
                lastCardDragPaint_ = paintNow;
                Reposition();
                Render();
            }
            return 0;
        }
        if (cardPress && !horizontalDominant) {
            /* 纵向为主：走下面的拖出流程；方向未定时先等。 */
            const bool verticalDominant =
                verticalRun > 20.0f * scale_ && verticalRun > horizontalRun * 1.3f;
            if (!verticalDominant) return 0;
        }

        /* 从颜色行的色块/hex/rgb 拖出：只带这一个颜色，ghost 是纯色块卡。 */
        if (pressedPart_ == RackPart::CardColor && pressColorIndex_ >= 0) {
            const int index = plugin_->IndexById(pressItemId_);
            if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
                const Item& item = plugin_->items()[static_cast<size_t>(index)];
                std::vector<card::Rgba> colors;
                if (card::ParseColors(item.note, colors) &&
                    pressColorIndex_ < static_cast<int>(colors.size())) {
                    const card::Rgba color = colors[static_cast<size_t>(pressColorIndex_)];
                    const std::wstring text = card::ColorDragText(color);
                    mouseDown_ = false;
                    dragStarted_ = true;
                    SetDragTransparent(true);
                    RECT returnBand{};
                    if (const RECT work = MonitorWorkArea(monitor_); work.right > work.left) {
                        float bottom = 200.0f * scale_;
                        if (const CardBox* box = view_.DisplayCard(index)) {
                            bottom = std::max(bottom, box->card.bottom + 24.0f * scale_);
                        }
                        returnBand.left = work.left;
                        returnBand.top = work.top;
                        returnBand.right = work.right;
                        returnBand.bottom = work.top + static_cast<LONG>(std::lround(bottom));
                    }
                    pressed_ = -1;
                    pressedPart_ = RackPart::None;
                    ReleaseCapture();

                    bool dropped = false;
                    dragdrop::DragOutcome outcome;
                    view_.SetDragging(index);
                    Render();
                    const CardBox* dragBox = view_.DisplayCard(index);
                    const int dragW = dragBox ? static_cast<int>(dragBox->card.right -
                                                                 dragBox->card.left)
                                              : static_cast<int>(150.0f * scale_);
                    const int dragH = dragBox ? static_cast<int>(dragBox->card.bottom -
                                                                 dragBox->card.top)
                                              : static_cast<int>(150.0f * scale_);
                    SIZE ghostSize{};
                    HBITMAP ghost = view_.MakeColorGhost(color, dragW, dragH, &ghostSize);
                    SetTimer(hwnd(), kTimerFakeCursor, 10, nullptr);
                    const DWORD effect = dragdrop::BeginTextDrag(
                        text, hwnd(), returnBand, ghost, ghostSize, dragW, dragH,
                        4.0f * scale_, &dropped, &outcome, {}, true);
                    KillTimer(hwnd(), kTimerFakeCursor);
                    view_.SetDragging(-1);
                    if (!outcome.cancelled && !outcome.returnedToRack) {
                        clipboard::CopyText(text);
                        POINT dropPoint{};
                        GetCursorPos(&dropPoint);
                        if (dragdrop::EditableAtPoint(dropPoint)) {
                            ClickAt(dropPoint);
                            Sleep(90);
                        }
                        SendPasteShortcut();
                    } else if (outcome.returnedToRack) {
                        ShakeCard(index);
                    }
                    if (effect == DROPEFFECT_NONE) Render();
                    dragStarted_ = false;
                    SetDragTransparent(false);
                    Render();
                    return 0;
                }
            }
        }
        const int index = plugin_->IndexById(pressItemId_);
        mouseDown_ = false;
        dragStarted_ = true;
        SetDragTransparent(true);
        /* 取消区域：屏幕顶部一直到卡片底部（整宽），比窗口矩形宽松，
           只要松手时还在这一条里就算「拖回来了」。 */
        RECT returnBand{};
        if (const RECT work = MonitorWorkArea(monitor_); work.right > work.left) {
            float bottom = 200.0f * scale_;
            if (const CardBox* box = view_.DisplayCard(index)) {
                bottom = std::max(bottom, box->card.bottom + 24.0f * scale_);
            }
            returnBand.left = work.left;
            returnBand.top = work.top;
            returnBand.right = work.right;
            returnBand.bottom = work.top + static_cast<LONG>(std::lround(bottom));
        }
        pressed_ = -1;
        pressedPart_ = RackPart::None;
        ReleaseCapture();

        if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
            bool dropped = false;
            dragdrop::DragOutcome outcome;
            view_.SetDragging(index);
            Render();
            const CardBox* dragBox = view_.DisplayCard(index);
            const int dragW = dragBox ? static_cast<int>(dragBox->card.right -
                                                         dragBox->card.left)
                                      : static_cast<int>(154.0f * scale_);
            const int dragH = dragBox ? static_cast<int>(dragBox->card.bottom -
                                                         dragBox->card.top)
                                      : static_cast<int>(119.0f * scale_);
            const Item& dragItem = plugin_->items()[index];
            /* 文字类卡片（便签 / SVG 代码）统一按文本拖拽；图片/文件仍拖文件。
               SVG 的 ghost 仍是图片卡片，便签是便签卡片。 */
            const std::wstring svgText =
                !dragItem.svgId.empty() ? plugin_->SvgText(index) : std::wstring();
            std::wstring dragText = svgText.empty() ? dragItem.note : svgText;
            /* 单色卡拖整卡 = 该颜色；多色卡拖整卡仍是原文。 */
            if (dragItem.cardType == card::Type::Color) {
                std::vector<card::Rgba> colors;
                if (card::ParseColors(dragItem.note, colors) && colors.size() == 1) {
                    dragText = card::ColorDragText(colors.front());
                }
            }
            DWORD effect = DROPEFFECT_NONE;
            const bool fileLike = dragItem.cardType == card::Type::File ||
                                  dragItem.cardType == card::Type::Video ||
                                  dragItem.cardType == card::Type::Music ||
                                  dragItem.cardType == card::Type::Recording;
            std::vector<std::wstring> dragFiles;
            std::wstring fileDragText;
            if (fileLike) {
                dragFiles = dragItem.paths;
                if (dragFiles.empty() && !dragItem.path.empty()) {
                    dragFiles.push_back(dragItem.path);
                }
                for (size_t fi = 0; fi < dragFiles.size(); ++fi) {
                    if (fi > 0) fileDragText += L'\n';
                    fileDragText += dragFiles[fi];
                }
                if (fileDragText.empty()) fileDragText = dragItem.label;
            }
            if (!dragText.empty()) {
                SIZE ghostSize{};
                /* 先让 view 画和绳子上完全一致的卡片 ghost；失败再用 PNG 预览兜底。 */
                HBITMAP ghost = view_.MakeDragGhost(dragItem, dragW, dragH, &ghostSize);
                const std::wstring previewPath = dragItem.svgId.empty() ? std::wstring() : dragItem.path;
                /* OLE 空数据拖拽时目标会画禁止光标；用 10ms 定时器把箭头压住。 */
                SetTimer(hwnd(), kTimerFakeCursor, 10, nullptr);
                effect = dragdrop::BeginTextDrag(dragText, hwnd(), returnBand, ghost, ghostSize, dragW,
                                                 dragH, 4.0f * scale_, &dropped, &outcome, previewPath,
                                                 true);
                KillTimer(hwnd(), kTimerFakeCursor);
            } else if (fileLike && (!dragFiles.empty() || !fileDragText.empty())) {
                /* 文件类：同时给 CF_HDROP 和完整路径文本，目标自己挑。
                   纯文本框拿文本，上传区拿文件。 */
                SIZE ghostSize{};
                HBITMAP ghost = view_.MakeDragGhost(dragItem, dragW, dragH, &ghostSize);
                effect = dragdrop::BeginTextDrag(
                    fileDragText, hwnd(), returnBand, ghost, ghostSize, dragW, dragH,
                    4.0f * scale_, &dropped, &outcome, {}, false, dragFiles);
            } else {
                effect = dragdrop::BeginDrag(dragItem.path, hwnd(), returnBand, dragW, dragH, 4.0f * scale_,
                                            &dropped, &outcome);
            }
            view_.SetDragging(-1);
            /* 文字类拖拽：松手后静默复制同一段文本，再替用户按 Ctrl+V。
               不依赖目标软件接不接拖放，也不区分具体是哪种设计软件。 */
            if (!dragText.empty() && !outcome.cancelled && !outcome.returnedToRack &&
                plugin_->CopyCardText(index)) {
                POINT dropPoint{};
                GetCursorPos(&dropPoint);
                /* 所有窗口统一用 UIA/MSAA 查元素：确认下面是可输入元素才先点一下；
                   没识别到就保持正常落点（直接粘贴），不再限定浏览器。 */
                if (dragdrop::EditableAtPoint(dropPoint)) {
                    ClickAt(dropPoint);
                    Sleep(90);
                }
                SendPasteShortcut();
                effect = DROPEFFECT_COPY;
                dropped = true;
            }
            /* 拖出去只是「把内容交给别的程序」，绝不从绳子上取下、也不动文件。
               拖回绳子区域则视为「取消」：这次拖拽被取消，卡片留在原地轻微晃动，作为提示。 */
            (void)dropped;
            if (outcome.returnedToRack) {
                ShakeCard(index);
            } else if (outcome.sameFolder) {
                /* 落在源文件所在的同一个文件夹：不复制；桌面则同时将图标移动到落点。 */
                if (outcome.desktop && !dragItem.path.empty()) {
                    dragdrop::RepositionDesktopIcon(dragItem.path, outcome.point);
                }
            } else if (outcome.duplicate) {
                /* 桌面已有同名同内容的那份：把桌面图标移到落点，而不是复制一份。 */
                if (outcome.desktop && !outcome.duplicatePath.empty()) {
                    dragdrop::RepositionDesktopIcon(outcome.duplicatePath, outcome.point);
                } else {
                    ShakeCard(index);
                }
            }
            if (effect == DROPEFFECT_NONE) {
                Render();
            }
        }
        dragStarted_ = false;
        SetDragTransparent(false);
        Render();
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!mouseDown_) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        mouseDown_ = false;
        ReleaseCapture();
        if (pressedPart_ == RackPart::Clip) {
            const int dx = std::abs(point.x - pressPoint_.x);
            const int dy = std::abs(point.y - pressPoint_.y);
            if (dx <= 6 * scale_ && dy <= 6 * scale_) {
                const int index = plugin_->IndexById(pressItemId_);
                if (index >= 0 && index < static_cast<int>(plugin_->items().size())) {
                    const Item& item = plugin_->items()[static_cast<size_t>(index)];
                    if (CanPinItem(item)) {
                        PendingPin kind = PendingPin::None;
                        pendingPinPath_.clear();
                        pendingPinText_.clear();
                        pendingPinPaths_.clear();
                        if (const CardBox* box = view_.CardAt(index)) {
                            POINT anchor{
                                static_cast<LONG>((box->card.left + box->card.right) * 0.5f),
                                static_cast<LONG>(box->card.bottom)};
                            ClientToScreen(hwnd(), &anchor);
                            pendingPinAnchor_ = anchor;
                        } else {
                            GetCursorPos(&pendingPinAnchor_);
                            pendingPinAnchor_.y += 16;
                        }
                        if (item.kind == 0 && !item.path.empty()) {
                            kind = PendingPin::Image;
                            pendingPinPath_ = item.path;
                        } else if (PinAsFiles(item)) {
                            kind = PendingPin::Files;
                            pendingPinPaths_ = item.paths;
                            if (pendingPinPaths_.empty() && !item.path.empty()) {
                                pendingPinPaths_.push_back(item.path);
                            }
                            if (pendingPinPaths_.empty()) kind = PendingPin::None;
                        } else {
                            pendingPinText_ = PinTextForItem(item);
                            if (!pendingPinText_.empty()) {
                                kind = PendingPin::Text;
                            } else if (!item.path.empty()) {
                                kind = PendingPin::Files;
                                pendingPinPaths_.push_back(item.path);
                            }
                        }
                        if (kind != PendingPin::None) {
                            /* 图片离卡片近一点，文字/文件保持更远的正下方。 */
                            const float gap = kind == PendingPin::Image ? 130.0f : 200.0f;
                            pendingPinAnchor_.y += static_cast<LONG>(gap * scale_);
                            /* 卡片完全落下去之后再贴到桌面；和图片夹子同一套节奏。 */
                            pendingPinKind_ = kind;
                            pendingPinAt_ = rack::NowMs() + 560;
                            RemoveItem(index, false);
                        }
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
            view_.SetFreeDragging(-1);
            pressed_ = -1;
            pressedPart_ = RackPart::None;
            FinishCardDrag(pressItemId_);
            return 0;
        }
        if (plugin_) {
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
                        QueueEditNote(index);
                    } else if (hit.part == RackPart::CardLink) {
                        OpenCardLink(index);
                    } else if (hit.part == RackPart::CardColor) {
                        CopyCardColor(index, hit.colorIndex, hit.colorPart);
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
        /* 快速连点信封时，第二次按下是 WM_LBUTTONDBLCLK，不是
           WM_LBUTTONDOWN。它仍然是用户对开关按钮的一次完整按下，
           必须和单击走同一条切换链路；否则这一下会被系统吞掉。 */
        if (hit.part == RackPart::Envelope) {
            ToggleClipboardFromEnvelope();
            return 0;
        }
        if (hit.index >= 0 && hit.index < static_cast<int>(plugin_->items().size())) {
            if (hit.part == RackPart::CardLink) {
                OpenCardLink(hit.index);
            } else if (hit.part == RackPart::Card || hit.part == RackPart::CardColor) {
                /* 双击与右键“打开”走同一条命令：文字类进编辑器，链接开浏览器，
                   图片/文件交给 shell。 */
                HandleMenuCommand(hit.index, kMenuOpen);
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (hwnd()) SetCapture(hwnd());
        return 0;

    case WM_RBUTTONDBLCLK:
        return 0;

    /* 右键在卡片上打开自己的菜单后，不要让同一次右键继续落到桌面，
       否则会出现桌面的右键菜单。 */
    case WM_CONTEXTMENU:
        return 0;

    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
        return 0;

    case WM_RBUTTONUP: {
        if (hwnd()) ReleaseCapture();
        if (!plugin_) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT screen = point;
        if (hwnd()) ClientToScreen(hwnd(), &screen);
        /* 用和钩子完全相同的卡片命中（含旋转/悬停边缘、夹子、便签纸），
           卡片上任何位置右键都能开菜单，但空白区域不会走到这里。 */
        const int index = CardIndexAtScreen(screen);
        if (index >= 0 && index < static_cast<int>(plugin_->items().size()) && !menuOpen_) {
            ShowItemMenu(index, screen);
        } else if (!menuOpen_) {
            /* 右键点在绳子/空白处：打开通用菜单（收起、设置）。
               卡片数量只在轻雪的设置页里改，不在右键菜单里重复一份。 */
            POINT client = screen;
            ScreenToClient(hwnd(), &client);
            const RackHit hit = view_.HitTest(static_cast<float>(client.x),
                                              static_cast<float>(client.y), scale_);
            if (hit.part == RackPart::Line || hit.part == RackPart::Background) {
                ShowGeneralMenu(screen);
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const RackHit hit = HitAtCursor();
        if ((hit.part == RackPart::Card || hit.part == RackPart::CardColor) &&
            ScrollColorCard(hit.index, GET_WHEEL_DELTA_WPARAM(wParam))) {
            return 0;
        }
        if (preview_.showing() && preview_.hwnd()) {
            SendMessageW(preview_.hwnd(), WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (mouseDown_ && GetCapture() != hwnd()) {
            mouseDown_ = false;
            cardDragging_ = false;
            view_.SetFreeDragging(-1);
            pressItemId_ = -1;
            pressed_ = -1;
            pressedPart_ = RackPart::None;
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


