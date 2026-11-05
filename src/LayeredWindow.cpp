#include "LayeredWindow.h"

LayeredWindow::LayeredWindow() = default;

LayeredWindow::~LayeredWindow()
{
    Destroy();
}

bool LayeredWindow::Create(Graphics* graphics, HINSTANCE instance, const wchar_t* className,
                           const wchar_t* title)
{
    if (hwnd_ || !graphics || !instance || !className) {
        return false;
    }
    graphics_ = graphics;
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &LayeredWindow::Thunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = className;
    ATOM atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        RackLog(L"LayeredWindow: RegisterClassExW(%s) failed %lu", className, GetLastError());
        return false;
    }
    classRegistered_ = true;

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(exStyle, className, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        RackLog(L"LayeredWindow: CreateWindowExW(%s) failed %lu", className, GetLastError());
        return false;
    }

    memoryDC_ = CreateCompatibleDC(nullptr);
    if (!memoryDC_) {
        RackLog(L"LayeredWindow: CreateCompatibleDC failed");
        Destroy();
        return false;
    }
    return true;
}

void LayeredWindow::Destroy()
{
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    target_.Reset();
    hiTarget_.Reset();
    hiWidth_ = 0;
    hiHeight_ = 0;
    ReleaseSurface();
    if (memoryDC_) {
        DeleteDC(memoryDC_);
        memoryDC_ = nullptr;
    }
}

POINT LayeredWindow::screenOrigin() const
{
    RECT rect{};
    if (hwnd_) {
        GetWindowRect(hwnd_, &rect);
    }
    return POINT{rect.left, rect.top};
}

bool LayeredWindow::EnsureSurface(int width, int height)
{
    if (width <= 0 || height <= 0 || !memoryDC_) {
        return false;
    }
    if (dib_ && width == surfaceWidth_ && height == surfaceHeight_) {
        return true;
    }

    ReleaseSurface();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; /* top-down, matches D2D coordinates */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memoryDC_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        RackLog(L"LayeredWindow: CreateDIBSection(%dx%d) failed %lu", width, height,
                GetLastError());
        if (dib) {
            DeleteObject(dib);
        }
        return false;
    }

    oldBitmap_ = SelectObject(memoryDC_, dib);
    dib_ = dib;
    bits_ = bits;
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    return true;
}

void LayeredWindow::ReleaseSurface()
{
    if (memoryDC_ && oldBitmap_) {
        SelectObject(memoryDC_, oldBitmap_);
        oldBitmap_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    bits_ = nullptr;
    surfaceWidth_ = 0;
    surfaceHeight_ = 0;
}

void LayeredWindow::MoveTo(int x, int y, int w, int h)
{
    if (!hwnd_) {
        return;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    width_ = w;
    height_ = h;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS);
}

void LayeredWindow::SetTopmost()
{
    if (!hwnd_) {
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void LayeredWindow::ShowNoActivate()
{
    if (!hwnd_) {
        return;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTopmost();
}

void LayeredWindow::Hide()
{
    if (hwnd_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void LayeredWindow::InvalidateSurface()
{
    target_.Reset();
    hiTarget_.Reset();
    hiWidth_ = 0;
    hiHeight_ = 0;
    ReleaseSurface();
}

void LayeredWindow::Render()
{
    if (!hwnd_ || !graphics_ || width_ <= 0 || height_ <= 0) {
        return;
    }
    if (!EnsureSurface(width_, height_)) {
        return;
    }

    if (!target_) {
        /* 软件 DC 渲染目标：默认（硬件）会为进程拉起一套 D3D11 设备，实测常驻私有内存
           多 ~22MB；晾衣架只是 2560×60 的 2D 条，软件光栅足够，先按软件建。
           如果以后某个大图/动效窗口确实需要硬件，再给 LayeredWindow 加 per-window 开关。 */
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT hr = graphics_->d2d()->CreateDCRenderTarget(&props, target_.GetAddressOf());
        if (FAILED(hr)) {
            RackLog(L"LayeredWindow: CreateDCRenderTarget failed 0x%08X",
                    static_cast<unsigned>(hr));
            return;
        }
        target_->SetDpi(96.0f, 96.0f);
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    RECT bounds{0, 0, width_, height_};
    HRESULT hr = target_->BindDC(memoryDC_, &bounds);
    if (FAILED(hr)) {
        return;
    }

    if (supersample_) {
        if (!hiTarget_ || hiWidth_ != width_ || hiHeight_ != height_) {
            hiTarget_.Reset();
            hiScale_ = 1.0f;
            /* 不同 D2D 软件目标的上限不一样；2x 建不出来就退 1.5x/1.25x。
               位图按 width*scale 建，DPI 也乘同样的 scale，这样 DIP 尺寸
               始终是 width×height，Paint 用原坐标，不会再放大画面。 */
            /* 3x/4x 再用双线性缩回会只采样 2x2，反而更锐、更闪；
               2x 双线性正好 2x2，配合 cubic 缩回最稳。 */
            static const float kFactors[] = {2.0f, 1.5f, 1.25f};
            for (float factor : kFactors) {
                const float hiW = static_cast<float>(width_) * factor;
                const float hiH = static_cast<float>(height_) * factor;
                if (hiW > 8192.0f || hiH > 8192.0f) {
                    continue;
                }
                const HRESULT createHr = target_->CreateCompatibleRenderTarget(
                    D2D1::SizeF(hiW, hiH), hiTarget_.GetAddressOf());
                if (createHr != S_OK || !hiTarget_) {
                    hiTarget_.Reset();
                    continue;
                }
                hiWidth_ = width_;
                hiHeight_ = height_;
                hiScale_ = factor;
                hiTarget_->SetDpi(96.0f * factor, 96.0f * factor);
                hiTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                hiTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                RackLog(L"LayeredWindow: supersample %dx%d x%.2f", width_, height_, factor);
                break;
            }
        }
        if (hiTarget_) {
            BeforeDraw(hiTarget_.Get());
            hiTarget_->BeginDraw();
            hiTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
            hiTarget_->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
            Paint(hiTarget_.Get());
            const HRESULT hiHr = hiTarget_->EndDraw();
            if (SUCCEEDED(hiHr)) {
                ComPtr<ID2D1Bitmap> bitmap;
                if (SUCCEEDED(hiTarget_->GetBitmap(bitmap.GetAddressOf())) && bitmap) {
                    target_->BeginDraw();
                    target_->SetTransform(D2D1::Matrix3x2F::Identity());
                    target_->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
                    target_->SetTransform(D2D1::Matrix3x2F::Scale(1.0f / hiScale_,
                                                                 1.0f / hiScale_));
                    target_->DrawBitmap(
                        bitmap.Get(),
                        D2D1::RectF(0.0f, 0.0f,
                                    static_cast<float>(width_) * hiScale_,
                                    static_cast<float>(height_) * hiScale_),
                        1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    hr = target_->EndDraw();
                    if (SUCCEEDED(hr)) {
                        Present();
                    } else if (hr == D2DERR_RECREATE_TARGET) {
                        InvalidateSurface();
                    }
                    return;
                }
            } else if (hiHr == D2DERR_RECREATE_TARGET) {
                hiTarget_.Reset();
                hiWidth_ = 0;
                hiHeight_ = 0;
            }
        }
    }

    BeforeDraw(target_.Get());
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    Paint(target_.Get());
    hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        InvalidateSurface();
        return;
    }
    if (FAILED(hr)) {
        return;
    }
    AfterDraw(memoryDC_, bits_, width_, height_);
    Present();
}

void LayeredWindow::Present()
{
    if (!hwnd_ || !memoryDC_ || !dib_) {
        return;
    }
    POINT source{0, 0};
    SIZE size{width_, height_};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(rack::Clamp(opacity_, 0.0f, 1.0f) * 255.0f + 0.5f);
    blend.AlphaFormat = AC_SRC_ALPHA;
    /* 有目标位置时把「移动/改大小」和「换画面」放在同一次调用里完成，
       中间不会出现系统拉伸旧画面的那一帧（动画会闪）。 */
    POINT destination = presentAt_;
    UpdateLayeredWindow(hwnd_, nullptr, hasPresentAt_ ? &destination : nullptr, &size,
                        memoryDC_, &source, 0, &blend, ULW_ALPHA);
}

void LayeredWindow::SetOpacity(float alpha)
{
    opacity_ = rack::Clamp(alpha, 0.0f, 1.0f);
}

void LayeredWindow::MovePresent(int x, int y)
{
    if (!hwnd_) {
        return;
    }
    if (!memoryDC_ || !dib_) {
        MoveTo(x, y, width_, height_);
        return;
    }
    presentAt_ = POINT{x, y};
    hasPresentAt_ = true;
    Present();
    hasPresentAt_ = false;
}

void LayeredWindow::RenderAt(int x, int y, int w, int h)
{
    if (!hwnd_) {
        return;
    }
    width_ = std::max(1, w);
    height_ = std::max(1, h);
    presentAt_ = POINT{x, y};
    hasPresentAt_ = true;
    Render();
    hasPresentAt_ = false;
}

LRESULT LayeredWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCCREATE:
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_DPICHANGED:
        dpiScale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK LayeredWindow::Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<LayeredWindow*>(create->lpCreateParams);
        if (self) {
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }

    auto* self = reinterpret_cast<LayeredWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
