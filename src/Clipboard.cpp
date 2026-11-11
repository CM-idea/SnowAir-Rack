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
    // 传统 CF_DIB：Adobe 拖拽只认这一份，因此必须是 32bpp 并保留 alpha。
    // 之前这里把 alpha 强制写成 0xFF，正是拖进去变白底的根因。
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
        // BGRA 原样拷贝，alpha 不再置 255
        std::memcpy(dst, src, stride);
        dst += stride;
    }

    GlobalUnlock(handle);
    return handle;
}

HGLOBAL MakeDibV5Handle(const DecodedImage& image)
{
    // CF_DIBV5：32bpp + alpha 掩码。设计软件（Adobe）优先取这份，透明通道才能保留。
    const size_t headerSize = sizeof(BITMAPV5HEADER);
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

    auto* header = reinterpret_cast<BITMAPV5HEADER*>(base);
    *header = {};
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = static_cast<LONG>(image.width);
    header->bV5Height = -static_cast<LONG>(image.height); /* top-down */
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(stride * image.height);
    header->bV5RedMask = 0x00FF0000;
    header->bV5GreenMask = 0x0000FF00;
    header->bV5BlueMask = 0x000000FF;
    header->bV5AlphaMask = 0xFF000000;
    header->bV5CSType = LCS_sRGB;
    header->bV5Intent = LCS_GM_GRAPHICS;

    std::memcpy(base + headerSize, image.bgra.data(), stride * image.height);
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

bool CopyFilePaths(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) {
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    if (EmptyClipboard()) {
        ok = SetClipboardData(CF_HDROP, util::MakeDropFilesHandle(paths)) != nullptr;
    }
    CloseClipboard();
    return ok;
}

HGLOBAL MakeDibFromFile(const std::wstring& path)
{
    DecodedImage image;
    if (!DecodeBgra(path, image)) {
        return nullptr;
    }
    return MakeDibHandle(image);
}

HGLOBAL MakeDibV5FromFile(const std::wstring& path)
{
    DecodedImage image;
    if (!DecodeBgra(path, image)) {
        return nullptr;
    }
    return MakeDibV5Handle(image);
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
            // 只给 DIBV5：Adobe 粘贴会走带 alpha 的这份；再给 CF_DIB 会被它优先取走，
            // 32bpp BI_RGB 的 alpha 一丢就成白底。Figma / 其他软件仍可从 DIBV5 或 PNG 取图。
            any |= SetHandleData(CF_DIBV5, MakeDibV5Handle(image));
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
