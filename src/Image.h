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

private:
    ComPtr<IWICImagingFactory> wic_;
};
