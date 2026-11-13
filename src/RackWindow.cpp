#include "RackWindow.h"
#include <fstream>

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
/* 移除卡片后先延迟该时长再开始靠拢，避免瞬间贴合的僵硬感。 */
constexpr uint64_t kCloseDelayMs = 130;
/* 屏幕最上面这一条：点一下就收回，和「停在这里拉下来」用的是同一块区域。 */
constexpr float kTopBarBand = 24.0f;
constexpr UINT kMsgTopBarClick = WM_APP + 2;
constexpr UINT kMsgColorWheel = WM_APP + 3;
constexpr UINT kMsgRackRightClick = WM_APP + 4;

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

void RackWindow::OnItemsChanged(bool added)
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
       任何自动展开都不能把绳子拉出来。 */
    if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover) {
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
    return AlphaAt(client.x, client.y);
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

void RackWindow::NormalizeLoadedSpacing()
{
    if (!plugin_) {
        return;
    }
    /* 0.1.1 的做法：按画面上的左右顺序补最小间距，偏移保留下来，
       视觉顺序才不会被启动时清零。 */
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
    struct Entry {
        float center = 0.0f;
        float left = 0.0f;
        float width = 0.0f;
        int index = -1;
    };
    std::vector<Entry> order;
    for (int i = 0; i < static_cast<int>(plugin_->items().size()); ++i) {
        if (const CardBox* box = view_.Card(i)) {
            order.push_back({(box->card.left + box->card.right) * 0.5f, box->card.left,
                             box->card.right - box->card.left, i});
        }
    }
    if (order.empty()) {
        return;
    }
    std::sort(order.begin(), order.end(),
              [](const Entry& a, const Entry& b) { return a.center < b.center; });

    std::vector<Item> reordered;
    reordered.reserve(order.size());
    for (const Entry& entry : order) {
        reordered.push_back(plugin_->items()[static_cast<size_t>(entry.index)]);
    }
    if (reordered.size() != plugin_->items().size()) {
        return;
    }

    const std::vector<float> targets = view_.TargetLefts(reordered);
    std::vector<float> oldOffsets(reordered.size(), 0.0f);
    std::vector<float> offsets(reordered.size(), 0.0f);
    for (size_t i = 0; i < reordered.size(); ++i) {
        oldOffsets[i] = reordered[i].rackOffsetX;
    }

    /* 拖到「两倍槽位间距」以内就吸附回槽位；超过才保留落点。
       之前是没有任何阈值，拖多远都会弹回。 */
    const float snapDistance = 2.0f * view_.DefaultGap();
    for (size_t i = 0; i < reordered.size(); ++i) {
        const Entry& entry = order[i];
        const float desired = entry.left - targets[i];
        const bool dragged = reordered[i].id == id;
        const bool wasFree = std::abs(oldOffsets[i]) > snapDistance;
        if (dragged) {
            offsets[i] = std::abs(desired) <= snapDistance ? 0.0f : desired;
        } else if (wasFree) {
            // 之前已经自由摆放的卡：保持它现在看到的绝对位置，不跟着重排弹走。
            offsets[i] = desired;
        } else {
            offsets[i] = 0.0f;
        }
    }

    /* 自由位置导致贴得太近/重叠时，整排回到槽位（用户说的“图片太多挤到一起才吸附”）。 */
    struct FinalBox {
        float left = 0.0f;
        float right = 0.0f;
    };
    std::vector<FinalBox> finals;
    finals.reserve(reordered.size());
    for (size_t i = 0; i < reordered.size(); ++i) {
        const float left = targets[i] + offsets[i];
        finals.push_back({left, left + order[i].width});
    }
    std::sort(finals.begin(), finals.end(),
              [](const FinalBox& a, const FinalBox& b) { return a.left < b.left; });
    bool crowded = false;
    const float minGap = 6.0f * scale_;
    for (size_t i = 1; i < finals.size(); ++i) {
        if (finals[i].left - finals[i - 1].right < minGap) {
            crowded = true;
            break;
        }
    }
    if (crowded) {
        std::fill(offsets.begin(), offsets.end(), 0.0f);
    }

    view_.SnapSlotsTo(reordered, targets);
    plugin_->items() = std::move(reordered);
    offsetAnims_.clear();
    const uint64_t now = rack::NowMs();
    for (size_t i = 0; i < plugin_->items().size(); ++i) {
        Item& item = plugin_->items()[i];
        const float oldOffset = oldOffsets[i];
        const float newOffset = offsets[i];
        if (std::abs(newOffset) > 0.35f) {
            // 保留位置：直接落到最终视觉位置，不再缓动回 0。
            item.rackOffsetX = newOffset;
        } else if (std::abs(oldOffset) > 0.35f) {
            // 吸附回槽位：从当前偏移缓动到 0。
            item.rackOffsetX = oldOffset;
            offsetAnims_.push_back({item.id, oldOffset, 0.0f, now, 180.0f});
            item.shakeStartMs = now;
            item.shakeAmp = 2.2f;
        } else {
            item.rackOffsetX = 0.0f;
        }
    }
    plugin_->SaveItems();
    needsLayout_ = true;
    if (hwnd()) {
        Reposition();
        Render();
        if (!offsetAnims_.empty()) {
            swingUntil_ = std::max(swingUntil_, now + 1250);
            SetTimer(hwnd(), kTimerAnim, 16, nullptr);
        }
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
            /* 动画收尾后还有可能挨在一起（比如左边那张被用户往右拖过），
               再按最小间距推开一次，仍然是缓动。 */
            NormalizeSpacing(220.0f);
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
        !view_.HasSliding() &&
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
    /* 快捷键/常驻模式下被快捷键隐藏时，任何自动路径都不能再把窗口拉出来。 */
    if (manuallyHidden_ && plugin_->config().displayMode != kDisplayHover) {
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
        if (!visible()) EnsureVisible();
        if (expand_ < 0.9f) Expand(false);
        lastInside_ = rack::NowMs();
    } else if (collapseAfterPreview_) {
        /* 设置页的预览刚结束：把之前因为 pin 住而没执行的那次收起补上。 */
        collapseAfterPreview_ = false;
        Collapse();
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

void RackWindow::UpdatePreview()
{
    if (!plugin_ || dragStarted_ || mouseDown_) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);

    const RackHit hit = HitAtCursor();
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
    /* SVG 卡片复制的是 SVG 代码本身，粘贴到 Figma / Illustrator 里是矢量；
       不另存 .svg 文件。 */
    const std::wstring svgText = plugin_->SvgText(index);
    if (!svgText.empty()) {
        clipboard::CopyText(svgText);
    } else if (copyItem.kind == 1) {
        clipboard::CopyText(copyItem.note);
    } else if (copyItem.kind == 0) {
        // 和拖拽保持同一条路径：只给 CF_HDROP，Adobe 按「粘贴文件」导入 PNG，
        // 透明通道由文件本身带；位图剪贴板会被 Adobe 取走并洗成白底。
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
    plugin_->RemoveItemAt(index);
    /* 移除一张卡片后，剩下的卡片回到布局的均匀槽位；否则旧的手动 offset 会让
       两边卡片看起来反而向两边分开/继续叠着。 */
    offsetAnims_.clear();
    const uint64_t now = rack::NowMs();
    bool hadOffset = false;
    for (auto& item : plugin_->items()) {
        if (std::abs(item.rackOffsetX) > 0.35f) {
            offsetAnims_.push_back({item.id, item.rackOffsetX, 0.0f, now, 180.0f});
            hadOffset = true;
        }
    }
    if (hadOffset) {
        SetTimer(hwnd(), kTimerAnim, 16, nullptr);
    }
    /* 位置动画由 RackView 的弹簧负责（目标位置按索引算，卡片通过弹簧平滑过渡），
       这里只要保证让位动画在跑。 */
    Reposition();
    Render();
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
        /* 卡片图只有约 140 DIP 宽。用 WIC Fant 先按接近显示尺寸缩小，
           比绘制时让 D2D 缩小 640px 的缩略图更干净——那样会
           把细线糊成一团。 */
        graphics_->images().Load(source, 192.0f * scale_, target, item.thumb);
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

    for (int i = 0;; ++i) {
        const CardBox* box = view_.CardAt(i);
        if (!box) {
            break;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(box->card.left)) - margin);
        const int y0 = std::max(0, static_cast<int>(std::floor(box->card.top)) - margin);
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(box->card.right)) + margin);
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(box->card.bottom)) + margin);

        const D2D1_POINT_2F pivot =
            D2D1::Point2F((box->card.left + box->card.right) * 0.5f, box->card.top);
        const float radians = box->angle * 3.1415926535f / 180.0f;
        const float ca = std::cos(radians);
        const float sa = std::sin(radians);
        const float left = box->card.left - extra;
        const float top = box->card.top - extra;
        const float right = box->card.right + extra;
        const float bottom = box->card.bottom + extra;
        float radius = 0.0f;
        if (box->cardType == card::Type::Note) {
            radius = 0.0f;
        } else if (box->flat) {
            radius = (box->card.right - box->card.left) / 10.0f;
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
                    (box->hasNote && lx >= box->note.left - extra &&
                     lx <= box->note.right + extra && ly >= box->note.top - extra &&
                     ly <= box->note.bottom + extra)) {
                    setOpaque(x, y);
                }
            }
        }
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
        for (int i = 0;; ++i) {
            const CardBox* box = view_.CardAt(i);
            if (!box) break;
            if (PointInCardShape(*box, px, py, extra) ||
                (box->hasNote && PointInNoteShape(*box, px, py, extra))) {
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
            const D2D1_POINT_2F anchor = view_.EnvelopeBadgeCenter();
            POINT screen{static_cast<LONG>(std::lround(anchor.x)),
                         static_cast<LONG>(std::lround(anchor.y))};
            ClientToScreen(hwnd(), &screen);
            plugin_->OpenClipboard(screen.x, screen.y);
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
            cardDragging_ = false;
            lastCardDragPaint_ = 0;
            pendingAdjustId_ = -1;
            offsetAnims_.clear();
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

        /* 横向为主 = 沿绳子移动并重排；纵向为主 = 拖出去。
           颜色行（色块/hex/rgb）单独处理成单色拖出，不参与整卡重排。 */
        const float horizontalRun = static_cast<float>(dx);
        const float verticalRun = static_cast<float>(dy);
        const bool horizontalDominant =
            horizontalRun > 12.0f * scale_ && horizontalRun >= verticalRun * 1.3f;
        const bool cardPress = pressedPart_ == RackPart::Card;
        if (cardPress && horizontalDominant) {
            cardDragging_ = true;
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
                        if (const CardBox* box = view_.Card(index)) {
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
                    const CardBox* dragBox = view_.Card(index);
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
            if (const CardBox* box = view_.Card(index)) {
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
            const CardBox* dragBox = view_.Card(index);
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
               不依赖目标软件接不接拖放，也不区分 Figma/Pixso/MasterGo 等。 */
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


