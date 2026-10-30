#include "Image.h"

bool ImageLoader::Init()
{
    if (wic_) {
        return true;
    }
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(wic_.GetAddressOf()));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic_.GetAddressOf()));
    }
    return SUCCEEDED(hr);
}

void ImageLoader::Shutdown()
{
    wic_.Reset();
}

HRESULT ImageLoader::GetSize(const std::wstring& path, UINT& width, UINT& height)
{
    width = 0;
    height = 0;
    if (!wic_ || path.empty()) {
        return E_INVALIDARG;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    return frame->GetSize(&width, &height);
}

HRESULT ImageLoader::Load(const std::wstring& path, float maxDimension,
                          ID2D1RenderTarget* target, ImageBitmap& out)
{
    out.Reset();
    if (!wic_ || !target || path.empty()) {
        return E_INVALIDARG;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand,
                                                 decoder.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    UINT width = 0, height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    ComPtr<IWICBitmapSource> source;
    frame.As(&source);

    if (maxDimension > 0.0f) {
        const float longest = static_cast<float>(std::max(width, height));
        if (longest > maxDimension) {
            const float ratio = maxDimension / longest;
            const UINT scaledW = std::max<UINT>(1, static_cast<UINT>(std::lround(width * ratio)));
            const UINT scaledH = std::max<UINT>(1, static_cast<UINT>(std::lround(height * ratio)));
            ComPtr<IWICBitmapScaler> scaler;
            hr = wic_->CreateBitmapScaler(scaler.GetAddressOf());
            if (FAILED(hr)) {
                return hr;
            }
            hr = scaler->Initialize(frame.Get(), scaledW, scaledH,
                                    WICBitmapInterpolationModeFant);
            if (FAILED(hr)) {
                return hr;
            }
            source = scaler;
            width = scaledW;
            height = scaledH;
        }
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wic_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return hr;
    }

    hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                           out.bitmap.GetAddressOf());
    if (FAILED(hr)) {
        out.Reset();
        return hr;
    }
    out.width = width;
    out.height = height;
    return S_OK;
}
