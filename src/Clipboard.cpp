#include "Clipboard.h"
#include "Util.h"

namespace clipboard {
namespace {

struct DecodedImage {
    UINT width = 0;
    UINT height = 0;
    std::vector<BYTE> bgra;
};

bool DecodeBgra(const std::wstring& path, DecodedImage& out)
{
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(wic.GetAddressOf()));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.GetAddressOf()));
    }
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              decoder.GetAddressOf()))) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
        return false;
    }

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf()))) {
        return false;
    }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.bgra = std::move(pixels);
    return true;
}

HGLOBAL AllocGlobal(size_t size)
{
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    return handle;
}

bool SetHandleData(UINT format, HGLOBAL handle)
{
    if (!handle) {
        return false;
    }
    if (!SetClipboardData(format, handle)) {
        GlobalFree(handle);
        return false;
    }
    /* 句柄已经交给剪贴板，这里不能再释放。 */
    return true;
}

HGLOBAL MakePngHandle(const std::vector<BYTE>& bytes)
{
    if (bytes.empty()) {
        return nullptr;
    }
    HGLOBAL handle = AllocGlobal(bytes.size());
    if (!handle) {
        return nullptr;
    }
    void* target = GlobalLock(handle);
    if (!target) {
        GlobalFree(handle);
        return nullptr;
    }
    std::memcpy(target, bytes.data(), bytes.size());
    GlobalUnlock(handle);
    return handle;
}

HGLOBAL MakeDibHandle(const DecodedImage& image)
{
    const size_t headerSize = sizeof(BITMAPINFOHEADER);
    const size_t stride = static_cast<size_t>(image.width) * 4;
    const size_t total = headerSize + stride * image.height;
    HGLOBAL handle = AllocGlobal(total);
    if (!handle) {
        return nullptr;
    }
    auto* base = static_cast<BYTE*>(GlobalLock(handle));
    if (!base) {
        GlobalFree(handle);
        return nullptr;
    }

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(base);
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(image.width);
    header->biHeight = static_cast<LONG>(image.height); /* bottom-up */
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(stride * image.height);
    header->biXPelsPerMeter = 2835;
    header->biYPelsPerMeter = 2835;

    BYTE* dst = base + headerSize;
    for (UINT y = 0; y < image.height; ++y) {
        const BYTE* src = image.bgra.data() +
                          static_cast<size_t>(image.height - 1 - y) * stride;
        for (UINT x = 0; x < image.width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 2];
            dst[x * 4 + 3] = 0xFF; /* CF_DIB consumers expect an opaque image */
        }
        dst += stride;
    }

    GlobalUnlock(handle);
    return handle;
}

} /* namespace */

bool CopyText(const std::wstring& text)
{
    if (text.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle) {
            if (void* dst = GlobalLock(handle)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(handle);
                ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
            }
            if (!ok) GlobalFree(handle);
        }
    }
    CloseClipboard();
    return ok;
}

bool CopyFilePath(const std::wstring& path)
{
    if (!util::FileExists(path)) {
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    if (EmptyClipboard()) {
        ok = SetClipboardData(CF_HDROP, util::MakeDropFilesHandle(path)) != nullptr;
    }
    CloseClipboard();
    return ok;
}

bool CopyImage(const std::wstring& path)
{
    if (!util::FileExists(path)) {
        RackLog(L"Clipboard: file does not exist: %s", path.c_str());
        return false;
    }

    std::vector<BYTE> png;
    util::ReadFileBytes(path, png);

    DecodedImage image;
    const bool decoded = DecodeBgra(path, image);
    if (!decoded && png.empty()) {
        return false;
    }

    if (!OpenClipboard(nullptr)) {
        RackLog(L"Clipboard: OpenClipboard failed %lu", GetLastError());
        return false;
    }

    bool ok = false;
    if (EmptyClipboard()) {
        bool any = false;
        if (decoded) {
            any |= SetHandleData(CF_DIB, MakeDibHandle(image));
        }
        if (!png.empty()) {
            UINT pngFormat = RegisterClipboardFormatW(L"PNG");
            if (pngFormat != 0) {
                any |= SetHandleData(pngFormat, MakePngHandle(png));
            }
            UINT imagePng = RegisterClipboardFormatW(L"image/png");
            if (imagePng != 0) {
                any |= SetHandleData(imagePng, MakePngHandle(png));
            }
        }
        any |= SetHandleData(CF_HDROP, util::MakeDropFilesHandle(path));
        ok = any;
    }

    CloseClipboard();
    return ok;
}

} /* namespace clipboard */
