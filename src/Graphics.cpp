#include "Graphics.h"
#include "Image.h"

bool Graphics::Init()
{
    if (d2d_ && dwrite_ && images_) {
        return true;
    }

    D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                   &options, reinterpret_cast<void**>(d2d_.GetAddressOf()));
    if (FAILED(hr)) {
        RackLog(L"Graphics: D2D1CreateFactory failed 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(hr)) {
        RackLog(L"Graphics: DWriteCreateFactory failed 0x%08X", static_cast<unsigned>(hr));
        d2d_.Reset();
        return false;
    }

    images_ = std::make_unique<ImageLoader>();
    if (!images_->Init()) {
        RackLog(L"Graphics: WIC initialization failed");
        images_.reset();
        dwrite_.Reset();
        d2d_.Reset();
        return false;
    }
    return true;
}

void Graphics::Shutdown()
{
    if (images_) {
        images_->Shutdown();
        images_.reset();
    }
    dwrite_.Reset();
    d2d_.Reset();
}
