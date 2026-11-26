#include "Image.h"

#include "Util.h"

#include <filesystem>
#include <system_error>

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
     * 持有（注意：解码器释放后，只要位图还在，文件句柄就还开着），于是「预览过 / 挂在
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

std::wstring ImageLoader::EnsureThumbnail(const std::wstring& source,
                                           const std::wstring& cacheFile, UINT maxDim)
{
    if (!wic_ || source.empty() || cacheFile.empty() || maxDim == 0) {
        return {};
    }
    std::error_code ec;
    auto usable = [&]() -> bool {
        std::error_code sizeEc;
        return std::filesystem::exists(cacheFile, sizeEc) &&
               std::filesystem::file_size(cacheFile, sizeEc) > 0;
    };
    if (usable()) {
        /* 源文件比缓存新（截图像素被替换、轻雪重渲染了 SVG 预览、录屏换了封面）
           就要重新缩一份；只看「文件存在」会一直显示旧图。 */
        const uint64_t sourceTime = util::FileWriteTime(source);
        const uint64_t cacheTime = util::FileWriteTime(cacheFile);
        if (sourceTime == 0 || cacheTime == 0 || cacheTime >= sourceTime) {
            return cacheFile;
        }
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand,
                                               decoder.GetAddressOf())) ||
        !decoder) {
        return {};
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame) {
        return {};
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return {};
    }
    const UINT longSide = (std::max)(width, height);
    const float ratio = longSide > maxDim ? static_cast<float>(maxDim) / longSide : 1.0f;
    const UINT scaledW = (std::max)(1u, static_cast<UINT>(std::lround(width * ratio)));
    const UINT scaledH = (std::max)(1u, static_cast<UINT>(std::lround(height * ratio)));
    ComPtr<IWICBitmapSource> sourceBitmap;
    if (scaledW != width || scaledH != height) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic_->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler ||
            FAILED(scaler->Initialize(frame.Get(), scaledW, scaledH,
                                      WICBitmapInterpolationModeFant))) {
            return {};
        }
        sourceBitmap = scaler;
    } else {
        sourceBitmap = frame;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.GetAddressOf())) || !converter ||
        FAILED(converter->Initialize(sourceBitmap.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return {};
    }
    /* 先确保缓存目录存在（父目录由调用方按 data 目录拼好）。 */
    std::filesystem::path cachePath(cacheFile);
    if (!cachePath.parent_path().empty()) {
        std::filesystem::create_directories(cachePath.parent_path(), ec);
    }
    std::filesystem::remove(cacheFile, ec);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frameEncode;
    if (FAILED(wic_->CreateStream(stream.GetAddressOf())) || !stream ||
        FAILED(stream->InitializeFromFilename(cacheFile.c_str(), GENERIC_WRITE)) ||
        FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) ||
        !encoder || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frameEncode.GetAddressOf(), nullptr)) || !frameEncode ||
        FAILED(frameEncode->Initialize(nullptr)) || FAILED(frameEncode->SetSize(scaledW, scaledH))) {
        std::filesystem::remove(cacheFile, ec);
        return {};
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frameEncode->SetPixelFormat(&format)) ||
        FAILED(frameEncode->WriteSource(converter.Get(), nullptr)) ||
        FAILED(frameEncode->Commit()) || FAILED(encoder->Commit()) || !usable()) {
        std::filesystem::remove(cacheFile, ec);
        return {};
    }
    return cacheFile;
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