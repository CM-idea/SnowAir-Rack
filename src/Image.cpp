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

    /* 先解成像素、再用 CreateBitmap 自建位图。
     *
     * 不能直接 CreateBitmapFromWicBitmap：用文件名打开的 WIC 源会被 D2D 位图一直
     * 持有（实测：解码器释放后，只要位图还在，文件句柄就还开着），于是「预览过 / 挂在
     * 晾衣架上」的那张 PNG 一直被占用 —— 资源管理器删不掉、SnowAir 删历史也失败。
     * 转成像素后 D2D 不再持有任何 WIC 对象，文件句柄在 Load() 返回时就关掉了。 */
    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                               pixels.data());
    if (FAILED(hr)) {
        out.Reset();
        return hr;
    }

    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                         D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    hr = target->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), stride, &props,
                              out.bitmap.GetAddressOf());
    if (FAILED(hr)) {
        out.Reset();
        return hr;
    }
    out.width = width;
    out.height = height;
    return S_OK;
}

HRESULT ImageLoader::LoadPixels(const std::wstring& path, UINT width, UINT height,
                                std::vector<BYTE>& out)
{
    out.clear();
    if (!wic_ || path.empty() || width == 0 || height == 0) {
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
    ComPtr<IWICBitmapScaler> scaler;
    hr = wic_->CreateBitmapScaler(scaler.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    hr = scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = wic_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }
    hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return hr;
    }
    const UINT stride = width * 4;
    out.resize(static_cast<size_t>(stride) * height);
    return converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data());
}