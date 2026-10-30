#pragma once

#include "Common.h"
#include "Image.h"
#include "LayeredWindow.h"

class Graphics;

/* 点击绳子上图片卡片的夹子时，把图片按 SnowAir 的贴图形态贴到桌面：
   置顶无边框、带投影、可拖动、滚轮缩放，右键/双击关闭。 */
class PinWindow : public LayeredWindow {
public:
    bool Open(Graphics* graphics, const std::wstring& path, POINT screenPos);
    void ClosePin();
    bool closed() const { return closed_; }
    const std::wstring& path() const { return path_; }

protected:
    void Paint(ID2D1RenderTarget* target) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    void ApplyLayout(POINT screenPos, bool centerOnPoint);
    void EnsureImage(ID2D1RenderTarget* target);

    Graphics* graphics_ = nullptr;
    ImageBitmap image_;
    std::wstring path_;
    UINT sourceW_ = 0;
    UINT sourceH_ = 0;
    float zoom_ = 1.0f;
    float dpi_ = 1.0f;
    int margin_ = 12;
    bool dragging_ = false;
    bool moved_ = false;
    bool suppressClickCopy_ = false;
    bool closed_ = false;
    POINT dragStart_{};
    POINT originStart_{};
};
