#pragma once

#include "Common.h"
#include "Graphics.h"

/*
 * 顶层、逐像素 alpha 的分层窗口：只有画过的像素才接收鼠标。
 *
 * 画面先画进 32 位 DIB，再用 UpdateLayeredWindow 提交，于是得到：
 *   - 逐像素透明，不需要交换链；
 *   - 命中测试跟着 alpha 通道走，完全透明的像素会把点击让给下面的
 *     桌面或其它窗口。
 *
 * 所有窗口都跑在宿主的 UI 线程上。
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
    /* 只挪位置：用现有画面做一次 UpdateLayeredWindow，拖动时不会闪。 */
    void MovePresent(int x, int y);
    /* 改位置/大小并换画面：一次 UpdateLayeredWindow 完成，动画不闪。 */
    void RenderAt(int x, int y, int w, int h);
    void SetTopmost();
    void ShowNoActivate();
    void Hide();
    void SetSupersample(bool enabled) { supersample_ = enabled; }
    /* 整窗透明度（1 = 不透明），用于和下面的卡片做交叉淡入淡出。 */
    void SetOpacity(float alpha);
    void Render();

    void SetDpiScale(float scale) { dpiScale_ = scale; }
    void InvalidateSurface();

    POINT screenOrigin() const;

protected:
    virtual void BeforeDraw(ID2D1RenderTarget* target) { (void)target; }
    virtual void Paint(ID2D1RenderTarget* target) = 0;
    /* D2D 画完之后再用 GDI 补画（例如和原生 EDIT 里完全一致的文字），
       此时内存 DC 里的内容还是上一帧画好的位图。 */
    virtual void AfterDraw(HDC dc, void* bits, int width, int height) {
        (void)dc; (void)bits; (void)width; (void)height;
    }
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
    POINT presentAt_{};
    bool hasPresentAt_ = false;
    float opacity_ = 1.0f;
};
