#pragma once

#include "Common.h"
#include "Item.h"
#include "LayeredWindow.h"
#include "Theme.h"

class Graphics;
class RackView;

/*
 * 悬停预览：一块独立的分层窗口，压在被悬停卡片的下方；
 * 大图显示，并带上完整便签正文，可以滚轮滚动。
 */
class PreviewWindow : public LayeredWindow {
public:
    PreviewWindow();
    ~PreviewWindow() override = default;

    bool Init(Graphics* graphics);
    void ShowItem(const Item& item, const D2D1_RECT_F& cardScreen, const RECT& workArea,
                  float scale, const Theme& theme);
    void Dismiss();

    bool showing() const { return visible() && item_ != nullptr; }
    const Item* item() const { return item_; }
    RECT screenRect() const;

protected:
    void BeforeDraw(ID2D1RenderTarget* target) override;
    void Paint(ID2D1RenderTarget* target) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    void EnsureFormat(float scale);
    float MeasureNoteHeight(float width, float scale) const;

    const Item* item_ = nullptr;
    Theme theme_;
    float scale_ = 1.0f;
    float panelWidth_ = 0.0f;
    float panelHeight_ = 0.0f;
    D2D1_RECT_F imageRect_{};
    D2D1_RECT_F noteRect_{};
    bool hasNote_ = false;
    int noteViewHeight_ = 0;
    int noteContentHeight_ = 0;
    int scrollY_ = 0;
    std::wstring loadedPath_;
    bool attempted_ = false;
    ImageBitmap preview_;
    ComPtr<IDWriteTextFormat> noteFormat_;
    float formatScale_ = 0.0f;
};
