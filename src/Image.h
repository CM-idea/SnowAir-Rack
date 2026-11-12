#pragma once

#include "Common.h"

struct ImageBitmap {
    ComPtr<ID2D1Bitmap> bitmap;
    UINT width = 0;
    UINT height = 0;

    void Reset()
    {
        bitmap.Reset();
        width = 0;
        height = 0;
    }

    bool valid() const { return bitmap != nullptr; }
};

class ImageLoader {
public:
    bool Init();
    void Shutdown();

    HRESULT Load(const std::wstring& path, float maxDimension, ID2D1RenderTarget* target,
                 ImageBitmap& out);
    HRESULT GetSize(const std::wstring& path, UINT& width, UINT& height);
    /* 解成 32bpp PBGRA 像素（预乘 alpha），给需要自己做像素运算的地方用
       （例如篮子的旋转：软件光栅不会对旋转位图做亚像素采样，只能先转好再贴）。 */
    HRESULT LoadPixels(const std::wstring& path, UINT width, UINT height,
                       std::vector<BYTE>& out);

private:
    ComPtr<IWICImagingFactory> wic_;
};
