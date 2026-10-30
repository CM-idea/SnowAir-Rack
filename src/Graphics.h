#pragma once

#include "Common.h"

class ImageLoader;

class Graphics {
public:
    bool Init();
    void Shutdown();

    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    ImageLoader& images() { return *images_; }

private:
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    std::unique_ptr<ImageLoader> images_;
};
