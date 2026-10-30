#include "PinWindow.h"

#include "Clipboard.h"
#include "Graphics.h"
#include "Util.h"

namespace {

RECT WorkAreaFor(POINT point)
{
    RECT work{};
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    return work;
}

} /* namespace */

bool PinWindow::Open(Graphics* graphics, const std::wstring& path, POINT screenPos)
{
    if (!graphics || path.empty()) {
        return false;
    }
    graphics_ = graphics;
    if (!hwnd() && !Create(graphics, util::GetDllModule(), L"SnowAirRackPin", L"Pin")) {
        return false;
    }
    const bool sameSource = util::SamePath(path_, path) && sourceW_ > 0 && sourceH_ > 0;
    path_ = path;
    if (!sameSource) {
        image_.Reset();
        if (FAILED(graphics->images().GetSize(path, sourceW_, sourceH_)) || sourceW_ == 0 ||
            sourceH_ == 0) {
            return false;
        }
        const HMONITOR monitor = MonitorFromPoint(screenPos, MONITOR_DEFAULTTONEAREST);
        dpi_ = util::ScaleForMonitor(monitor);
        margin_ = static_cast<int>(std::lround(12.0f * dpi_));
        const RECT work = WorkAreaFor(screenPos);
        const float workW = static_cast<float>(work.right - work.left);
        const float workH = static_cast<float>(work.bottom - work.top);
        zoom_ = std::min(1.0f, std::min(workW * 0.9f / static_cast<float>(sourceW_),
                                       workH * 0.9f / static_cast<float>(sourceH_)));
        zoom_ = std::max(0.2f, zoom_);
    }
    ApplyLayout(screenPos, true);
    closed_ = false;
    ShowNoActivate();
    Render();
    return true;
}

void PinWindow::ClosePin()
{
    closed_ = true;
    dragging_ = false;
    Hide();
}

void PinWindow::ApplyLayout(POINT screenPos, bool centerOnPoint)
{
    if (!hwnd()) return;
    const int width = std::max(1, static_cast<int>(std::lround(sourceW_ * zoom_)) + margin_ * 2);
    const int height = std::max(1, static_cast<int>(std::lround(sourceH_ * zoom_)) + margin_ * 2);
    const RECT work = WorkAreaFor(screenPos);
    int x = centerOnPoint ? screenPos.x - width / 2 : screenPos.x;
    int y = centerOnPoint ? screenPos.y - height / 2 : screenPos.y;
    if (work.right > work.left) {
        x = std::clamp(x, static_cast<int>(work.left) + 4,
                       static_cast<int>(work.right) - width - 4);
        y = std::clamp(y, static_cast<int>(work.top) + 4,
                       static_cast<int>(work.bottom) - height - 4);
    }
    MoveTo(x, y, width, height);
}

void PinWindow::EnsureImage(ID2D1RenderTarget* target)
{
    if (image_.valid() || !graphics_ || !target || path_.empty()) {
        return;
    }
    const float maxDimension =
        std::max(1400.0f, static_cast<float>(std::max(sourceW_, sourceH_)));
    graphics_->images().Load(path_, maxDimension, target, image_);
}

void PinWindow::Paint(ID2D1RenderTarget* target)
{
    if (!graphics_ || !target) return;
    EnsureImage(target);
    const float s = dpi_;
    const float margin = static_cast<float>(margin_);
    const D2D1_RECT_F imageRect = D2D1::RectF(
        margin, margin, static_cast<float>(width()) - margin,
        static_cast<float>(height()) - margin);

    /* 柔和投影：几层逐渐扩大的低透明度圆角矩形。 */
    ComPtr<ID2D1SolidColorBrush> shadow;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f),
                                                shadow.GetAddressOf()))) {
        for (int layer = 4; layer >= 1; --layer) {
            const float grow = static_cast<float>(layer) * 2.2f * s;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(imageRect.left - grow, imageRect.top - grow + 3.0f * s,
                                imageRect.right + grow, imageRect.bottom + grow + 3.0f * s),
                    6.0f * s + grow, 6.0f * s + grow),
                shadow.Get());
        }
    }

    if (image_.valid()) {
        target->DrawBitmap(image_.bitmap.Get(), imageRect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    ComPtr<ID2D1SolidColorBrush> edge;
    if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f),
                                                edge.GetAddressOf()))) {
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(imageRect, 4.0f * s, 4.0f * s), edge.Get(), 1.0f * s);
    }
}

LRESULT PinWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_LBUTTONDOWN: {
        dragging_ = true;
        moved_ = false;
        POINT cursor{};
        GetCursorPos(&cursor);
        const POINT origin = screenOrigin();
        dragStart_ = cursor;
        originStart_ = origin;
        SetCapture(hwnd());
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!dragging_) break;
        POINT cursor{};
        GetCursorPos(&cursor);
        const int dx = cursor.x - dragStart_.x;
        const int dy = cursor.y - dragStart_.y;
        if (std::abs(dx) > 3 || std::abs(dy) > 3) moved_ = true;
        if (!moved_) return 0;
        MoveTo(originStart_.x + dx, originStart_.y + dy, width(), height());
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            if (!moved_ && !suppressClickCopy_) {
                clipboard::CopyImage(path_);
            }
        }
        suppressClickCopy_ = false;
        return 0;
    case WM_LBUTTONDBLCLK:
        /* 双击打开原图；吞掉紧随其后的那次单击复制。 */
        suppressClickCopy_ = true;
        if (!path_.empty()) util::OpenInShell(path_);
        return 0;
    case WM_RBUTTONUP:
        ClosePin();
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const float previous = zoom_;
        zoom_ = std::clamp(zoom_ * (delta > 0 ? 1.1f : 1.0f / 1.1f), 0.1f, 4.0f);
        if (std::abs(zoom_ - previous) < 0.0001f) return 0;
        POINT cursor{};
        GetCursorPos(&cursor);
        ApplyLayout(cursor, true);
        Render();
        return 0;
    }
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    default:
        break;
    }
    return LayeredWindow::HandleMessage(message, wParam, lParam);
}
