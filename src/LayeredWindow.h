#pragma once

#include "Common.h"
#include "Graphics.h"

/*
 * A top-level, per-pixel-alpha, click-through-unless-painted window.
 *
 * The window is painted into a 32-bpp DIB and presented with
 * UpdateLayeredWindow, which gives us:
 *   - per-pixel transparency without a compositor swap chain;
 *   - hit testing that follows the alpha channel, so fully transparent
 *     pixels let clicks reach the desktop/app below.
 *
 * Every window runs on the host UI thread.
 */
class LayeredWindow {
public:
    LayeredWindow();
    virtual ~LayeredWindow();

    bool Create(Graphics* graphics, HINSTANCE instance, const wchar_t* className,
                const wchar_t* title = L"");
    void Destroy();

    HWND hwnd() const { return hwnd_; }
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_) != FALSE; }
    int width() const { return width_; }
    int height() const { return height_; }
    float dpiScale() const { return dpiScale_; }

    void MoveTo(int x, int y, int w, int h);
    void SetTopmost();
    void ShowNoActivate();
    void Hide();
    void SetSupersample(bool enabled) { supersample_ = enabled; }
    void Render();

    void SetDpiScale(float scale) { dpiScale_ = scale; }
    void InvalidateSurface();

    POINT screenOrigin() const;

protected:
    virtual void BeforeDraw(ID2D1RenderTarget* target) { (void)target; }
    virtual void Paint(ID2D1RenderTarget* target) = 0;
    virtual LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    Graphics* graphics_ = nullptr;
    HINSTANCE instance_ = nullptr;

private:
    static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureSurface(int width, int height);
    void ReleaseSurface();
    void Present();

    HWND hwnd_ = nullptr;
    HDC memoryDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    float dpiScale_ = 1.0f;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1BitmapRenderTarget> hiTarget_;
    int hiWidth_ = 0;
    int hiHeight_ = 0;
    bool supersample_ = false;
    bool classRegistered_ = false;
};
