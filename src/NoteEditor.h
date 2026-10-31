#pragma once

#include "Common.h"
#include "LayeredWindow.h"
#include "Theme.h"

#include <functional>

class Graphics;

/*
 * 便签编辑卡片。
 *
 * 打开时，绳子上的那张便签原地「长」成编辑卡片：先是便签的纸色和正文，
 * 边放大边淡入成编辑界面。保存/取消时反向播放：卡片原地淡回便签的外观，
 * 再缩小飞回绳子上的夹子，落位后与晾衣架画的那张卡片交叉淡出完成交接。
 * 编辑期间卡片可以拖动（光标保持箭头，输入区仍是工字光标）。
 *
 * 视觉跟「新建便签」弹窗一致（轻雪的弹窗配色：卡片/描边/主色按钮）。
 * 文字输入交给一个无边框原生 EDIT（挂在卡片上方），输入法、选区、滚动、
 * 粘贴都由系统处理；卡片只负责背景与装饰的绘制。
 */
class NoteEditor : public LayeredWindow {
public:
    /*
     * 打开模态编辑器，返回 true 表示用户点了“完成”。
     * `anchor` 是被编辑便签卡片的屏幕矩形：打开从这里开始，收回也飞回这里。
     * `onShown` 在第一帧画出来之后调用，宿主可在这时把卡片藏掉，避免闪一下。
     */
    static bool Edit(HWND owner, HINSTANCE instance, Graphics* graphics, const Theme& theme,
                     const std::wstring& title, const RECT& anchor,
                     const std::wstring& initial, std::wstring& text,
                     std::function<void()> onShown = nullptr,
                     std::function<void(const RECT&)> onClosed = nullptr,
                     std::function<void()> onLanded = nullptr);

protected:
    void Paint(ID2D1RenderTarget* target) override;
    void AfterDraw(HDC dc, void* bits, int width, int height) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    struct Layout {
        D2D1_RECT_F card{};
        D2D1_RECT_F title{};
        D2D1_RECT_F close{};
        D2D1_RECT_F field{};
        D2D1_RECT_F hint{};
        D2D1_RECT_F cancel{};
        D2D1_RECT_F save{};
    };

    enum class Hot { None, Close, Cancel, Save };

    bool Open(HWND owner, const RECT& anchor, const std::wstring& initial, float scale);
    void CreateEdit();
    void DestroyEdit();
    void ApplyEditFont();
    void LayoutEdit();
    void EnsureFormats();
    IDWriteTextFormat* MorphFormat(float size);
    Layout Measure() const;
    Hot HitTest(float x, float y) const;
    std::wstring EditText() const;
    void Finish(bool accepted);
    void CompleteFinish();
    void StartMorph(const RECT& from, const RECT& to, bool closing, int milliseconds);
    void TickMorph();
    void ActivateForInput();
    D2D1_COLOR_F FieldColor() const;
    static LRESULT CALLBACK EditThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    Theme theme_{};
    std::wstring title_;
    std::wstring initial_;
    std::wstring result_;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    WNDPROC editOldProc_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    COLORREF editBrushColor_ = 0;
    RECT workArea_{};
    RECT cardAnchor_{};
    float scale_ = 1.0f;        /* 屏幕缩放 */
    float contentScale_ = 1.0f; /* 编辑内容缩放（动画中） */
    float morphT_ = 1.0f;       /* 0 = 还是便签，1 = 完全是输入框 */
    float formatScale_ = 0.0f;
    Hot hot_ = Hot::None;
    Hot pressed_ = Hot::None;
    bool done_ = false;
    bool accepted_ = false;
    std::function<void(const RECT&)> onClosed_;
    std::function<void()> onLanded_;
    bool morphing_ = false;
    bool closing_ = false;
    bool pendingAccepted_ = false;
    bool dragging_ = false;
    bool flying_ = false;
    bool noteOnly_ = false;
    bool alphaFading_ = false;
    uint64_t alphaStart_ = 0;
    POINT dragOrigin_{};
    RECT dragStart_{};
    /* 拖动时把输入框先藏起来、正文由卡片自己画：跟着窗口一起动，不会闪。 */
    std::wstring dragText_;
    POINT dragTextOrigin_{};
    int lastEditW_ = 0;
    int lastEditH_ = 0;
    RECT morphFrom_{};
    RECT morphTo_{};
    uint64_t morphStart_ = 0;
    int morphMs_ = 1;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> hintFormat_;
    ComPtr<IDWriteTextFormat> buttonFormat_;
    /* 收回动画里正文要连续缩放，字号是插值出来的，单独缓存一个 format。 */
    ComPtr<IDWriteTextFormat> morphFormat_;
    float morphFormatSize_ = -1.0f;
};
