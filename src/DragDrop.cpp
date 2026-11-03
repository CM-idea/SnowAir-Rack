#include "DragDrop.h"
#include "Clipboard.h"
#include "Util.h"

#include <oleacc.h>
#include <uiautomationclient.h>
#pragma comment(lib, "uiautomationcore.lib")

#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace dragdrop {
namespace {


void UpdateFakeDragCursor()
{
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

bool UiaEditableAtPoint(POINT point)
{
    using Microsoft::WRL::ComPtr;
    static ComPtr<IUIAutomation> uia = [] {
        ComPtr<IUIAutomation> result;
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(result.GetAddressOf()));
        return result;
    }();
    if (!uia) return false;
    ComPtr<IUIAutomationElement> element;
    if (FAILED(uia->ElementFromPoint(point, element.GetAddressOf())) || !element) {
        return false;
    }
    CONTROLTYPEID type = 0;
    element->get_CurrentControlType(&type);
    if (type == UIA_EditControlTypeId) {
        return true;
    }
    ComPtr<IUnknown> pattern;
    if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, pattern.GetAddressOf())) &&
        pattern) {
        ComPtr<IValueProvider> value;
        pattern.As(&value);
        BOOL readOnly = TRUE;
        if (value && SUCCEEDED(value->get_IsReadOnly(&readOnly)) && !readOnly) {
            return true;
        }
    }
    return false;
}

bool MsaaTextAtPoint(POINT point)
{
    static HMODULE oleacc = LoadLibraryW(L"oleacc.dll");
    if (!oleacc) return false;
    using AccessibleObjectFromPointFn = HRESULT(WINAPI*)(POINT, IAccessible**, VARIANT*);
    static auto fn = reinterpret_cast<AccessibleObjectFromPointFn>(
        GetProcAddress(oleacc, "AccessibleObjectFromPoint"));
    if (!fn) return false;
    IAccessible* object = nullptr;
    VARIANT child{};
    VariantInit(&child);
    if (FAILED(fn(point, &object, &child)) || !object) {
        VariantClear(&child);
        return false;
    }
    bool text = false;
    VARIANT role{};
    VariantInit(&role);
    if (SUCCEEDED(object->get_accRole(child, &role)) && role.vt == VT_I4 &&
        role.lVal == ROLE_SYSTEM_TEXT) {
        text = true;
    }
    VariantClear(&role);
    VariantClear(&child);
    object->Release();
    return text;
}

HGLOBAL MakeBytesHandle(const std::vector<BYTE>& bytes)
{
    if (bytes.empty()) {
        return nullptr;
    }
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
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

HBITMAP MakeDragBitmap(const std::wstring& path, int cardWidth, int cardHeight,
                       float frameWidth, SIZE* sizeOut)
{
    using Microsoft::WRL::ComPtr;
    if (cardWidth < 24 || cardHeight < 24) {
        return nullptr;
    }
    /* 轻雪渲染的 SVG 预览：卡片里要留白，ghost 也照卡片的样子留一圈。 */
    bool svgPreview = false;
    if (path.size() >= 9) {
        std::wstring tail = path.substr(path.size() - 9);
        for (auto& c : tail) c = static_cast<wchar_t>(towlower(c));
        svgPreview = tail == L".svg3.png";
    }
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
        return nullptr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand,
                                                  decoder.GetAddressOf()))) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frameDecode;
    if (FAILED(decoder->GetFrame(0, frameDecode.GetAddressOf()))) {
        return nullptr;
    }

    const int frame = std::max(2, static_cast<int>(frameWidth));
    const int photoW = std::max(1, cardWidth - frame * 2);
    const int photoH = std::max(1, cardHeight - frame * 2);
    UINT srcW = 0, srcH = 0;
    frameDecode->GetSize(&srcW, &srcH);
    int drawW = photoW;
    int drawH = photoH;
    int drawX = 0;
    int drawY = 0;
    if (svgPreview && srcW > 0 && srcH > 0) {
        const int pad = std::max(4, frame * 3);
        const int boxW = std::max(1, photoW - pad * 2);
        const int boxH = std::max(1, photoH - pad * 2);
        const double scale = std::min(static_cast<double>(boxW) / srcW,
                                      static_cast<double>(boxH) / srcH);
        drawW = std::max(1, static_cast<int>(std::lround(srcW * scale)));
        drawH = std::max(1, static_cast<int>(std::lround(srcH * scale)));
        drawX = (photoW - drawW) / 2;
        drawY = (photoH - drawH) / 2;
    }
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(frameDecode.Get(), drawW, drawH,
                                  WICBitmapInterpolationModeFant))) {
        return nullptr;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0f,
                                     WICBitmapPaletteTypeCustom))) {
        return nullptr;
    }
    const UINT photoStride = static_cast<UINT>(photoW) * 4;
    std::vector<BYTE> photo(static_cast<size_t>(photoStride) * photoH, 0);
    if (svgPreview) {
        const UINT drawStride = static_cast<UINT>(drawW) * 4;
        std::vector<BYTE> scaled(static_cast<size_t>(drawStride) * drawH);
        if (FAILED(converter->CopyPixels(nullptr, drawStride,
                                         static_cast<UINT>(scaled.size()), scaled.data()))) {
            return nullptr;
        }
        for (int y = 0; y < drawH; ++y) {
            std::memcpy(photo.data() + (static_cast<size_t>(drawY + y) * photoW + drawX) * 4,
                        scaled.data() + static_cast<size_t>(y) * drawStride,
                        static_cast<size_t>(drawW) * 4);
        }
    } else if (FAILED(converter->CopyPixels(nullptr, photoStride,
                                            static_cast<UINT>(photo.size()), photo.data()))) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = cardWidth;
    info.bmiHeader.biHeight = -cardHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    auto* dst = static_cast<BYTE*>(pixels);
    std::memset(dst, 0, static_cast<size_t>(cardWidth) * cardHeight * 4);

    const double radius = 12.0 * frame / 4.0;
    const double photoRadius = 7.0 * frame / 4.0;
    auto insideRounded = [&](double x, double y, double width, double height,
                             double corner) {
        if (x < 0.0 || y < 0.0 || x >= width || y >= height) {
            return false;
        }
        const double cx = (x < corner) ? corner : (x > width - corner ? width - corner : x);
        const double cy = (y < corner) ? corner : (y > height - corner ? height - corner : y);
        const double dx = x - cx;
        const double dy = y - cy;
        return dx * dx + dy * dy <= corner * corner ||
               (x >= corner && x <= width - corner) ||
               (y >= corner && y <= height - corner);
    };

    for (int y = 0; y < cardHeight; ++y) {
        for (int x = 0; x < cardWidth; ++x) {
            double coverage = 0.0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    if (insideRounded(x + (sx + 0.5) * 0.5, y + (sy + 0.5) * 0.5,
                                      cardWidth, cardHeight, radius)) {
                        coverage += 0.25;
                    }
                }
            }
            if (coverage <= 0.0) {
                continue;
            }
            const bool onBorder = x < 1 || y < 1 || x >= cardWidth - 1 || y >= cardHeight - 1;
            double baseR = onBorder ? 222.0 : 248.0;
            double baseG = onBorder ? 227.0 : 249.0;
            double baseB = onBorder ? 234.0 : 250.0;
            const bool photoPixel =
                insideRounded(x - frame + 0.5, y - frame + 0.5, photoW, photoH,
                              photoRadius);
            if (photoPixel) {
                const size_t index =
                    (static_cast<size_t>(y - frame) * photoW + (x - frame)) * 4;
                const double alpha = photo[index + 3] / 255.0;
                baseR = photo[index + 2] * alpha + baseR * (1.0 - alpha);
                baseG = photo[index + 1] * alpha + baseG * (1.0 - alpha);
                baseB = photo[index + 0] * alpha + baseB * (1.0 - alpha);
            }
            const size_t out = (static_cast<size_t>(y) * cardWidth + x) * 4;
            dst[out + 0] = static_cast<BYTE>(baseB * coverage);
            dst[out + 1] = static_cast<BYTE>(baseG * coverage);
            dst[out + 2] = static_cast<BYTE>(baseR * coverage);
            dst[out + 3] = static_cast<BYTE>(coverage * 255.0);
        }
    }
    if (sizeOut) {
        sizeOut->cx = cardWidth;
        sizeOut->cy = cardHeight;
    }
    return bitmap;
}

/* 文本卡片拖拽时用的示意图：便签纸色 + 正文前三行。 */
HBITMAP MakeTextDragBitmap(const std::wstring& text, int cardWidth, int cardHeight,
                           float frameWidth, SIZE* sizeOut)
{
    if (cardWidth < 24 || cardHeight < 24) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = cardWidth;
    info.bmiHeader.biHeight = -cardHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    auto* dst = static_cast<BYTE*>(pixels);
    std::memset(dst, 0, static_cast<size_t>(cardWidth) * cardHeight * 4);

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT paper{0, 0, cardWidth, cardHeight};
    HBRUSH brush = CreateSolidBrush(RGB(0xF4, 0xD4, 0x76));
    FillRect(dc, &paper, brush);
    DeleteObject(brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x5A, 0x46, 0x2A));
    const float scaleFactor = std::max(0.6f, frameWidth / 4.0f);
    const int fontHeight = -static_cast<int>(13.0f * scaleFactor + 0.5f);
    HFONT font = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    RECT textRect{static_cast<LONG>(12.0f * scaleFactor), static_cast<LONG>(10.0f * scaleFactor),
                  cardWidth - static_cast<LONG>(12.0f * scaleFactor),
                  cardHeight - static_cast<LONG>(10.0f * scaleFactor)};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    if (font) {
        DeleteObject(font);
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);

    /* 圆角之外、以及半覆盖的边缘按覆盖率预乘 alpha（OLE 拖拽图要求预乘）。 */
    const double radius = 12.0 * frameWidth / 4.0;
    auto insideRounded = [&](double x, double y) {
        if (x < 0.0 || y < 0.0 || x >= cardWidth || y >= cardHeight) {
            return false;
        }
        const double cx = std::min(std::max(x, radius), cardWidth - radius);
        const double cy = std::min(std::max(y, radius), cardHeight - radius);
        const double dx = x - cx;
        const double dy = y - cy;
        return dx * dx + dy * dy <= radius * radius;
    };
    for (int y = 0; y < cardHeight; ++y) {
        for (int x = 0; x < cardWidth; ++x) {
            double coverage = 0.0;
            for (int sy = 0; sy < 2; ++sy) {
                for (int sx = 0; sx < 2; ++sx) {
                    if (insideRounded(x + (sx + 0.5) * 0.5, y + (sy + 0.5) * 0.5)) {
                        coverage += 0.25;
                    }
                }
            }
            BYTE* pixel = dst + (static_cast<size_t>(y) * cardWidth + x) * 4;
            if (coverage <= 0.0) {
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
                continue;
            }
            pixel[0] = static_cast<BYTE>(pixel[0] * coverage);
            pixel[1] = static_cast<BYTE>(pixel[1] * coverage);
            pixel[2] = static_cast<BYTE>(pixel[2] * coverage);
            pixel[3] = static_cast<BYTE>(255.0 * coverage);
        }
    }
    if (sizeOut) {
        sizeOut->cx = cardWidth;
        sizeOut->cy = cardHeight;
    }
    return bitmap;
}

HWND CreateDragPreview(HBITMAP bitmap, int width, int height)
{
    static bool registered = false;
    constexpr wchar_t kClass[] = L"SnowAirRackDragPreview";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) {
            return nullptr;
        }
        registered = true;
    }
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,
        kClass, L"", WS_POPUP, 0, 0, width, height, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        return nullptr;
    }

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    POINT cursor{};
    GetCursorPos(&cursor);
    POINT destination{cursor.x - width / 2, cursor.y - height / 2};
    POINT source{0, 0};
    SIZE size{width, height};
    const BOOL updated = UpdateLayeredWindow(hwnd, screen, &destination, &size, memory,
                                             &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!updated) {
        DestroyWindow(hwnd);
        return nullptr;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, destination.x, destination.y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return hwnd;
}

class RackDataObject final : public IDataObject {
public:
    explicit RackDataObject(const std::wstring& path) : path_(path)
    {
        util::ReadFileBytes(path, png_);

        /* 先给位图，再给文件，最后给 PNG 字节：
           设计软件（Illustrator 等）优先取 CF_DIB，粘进去的是图片本身，
           而不会把路径当成文本来排版；资源管理器这类只要文件的仍拿 CF_HDROP。 */
        const UINT png = RegisterClipboardFormatW(L"PNG");
        const UINT pngMime = RegisterClipboardFormatW(L"image/png");

        formats_[count_++] = MakeFormat(CF_DIB);
        formats_[count_++] = MakeFormat(CF_HDROP);
        if (png) {
            formats_[count_++] = MakeFormat(png);
        }
        if (pngMime) {
            formats_[count_++] = MakeFormat(pngMime);
        }
    }

    /* IUnknown 接口 */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG value = InterlockedDecrement(&ref_);
        if (value == 0) {
            delete this;
        }
        return static_cast<ULONG>(value);
    }

    /* IDataObject 接口 */
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!format || !medium) {
            return E_INVALIDARG;
        }
        if (format->lindex != -1 && format->lindex != 0) {
            return DV_E_FORMATETC;
        }

        for (const ExtraFormat& extra : extra_) {
            if (extra.format.cfFormat == format->cfFormat) {
                if (extra.medium.tymed & TYMED_ISTREAM) {
                    medium->tymed = TYMED_ISTREAM;
                    medium->pstm = extra.medium.pstm;
                    if (medium->pstm) {
                        medium->pstm->AddRef();
                    }
                    medium->pUnkForRelease = nullptr;
                    return S_OK;
                }
                HGLOBAL copy = OleDuplicateData(extra.medium.hGlobal, format->cfFormat,
                                                GMEM_MOVEABLE);
                if (!copy) {
                    return STG_E_MEDIUMFULL;
                }
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = copy;
                medium->pUnkForRelease = nullptr;
                return S_OK;
            }
        }

        if (!(format->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }

        HGLOBAL handle = nullptr;
        if (format->cfFormat == CF_DIB) {
            if (!dib_) {
                dib_ = clipboard::MakeDibFromFile(path_);
            }
            if (dib_) {
                handle = OleDuplicateData(dib_, CF_DIB, GMEM_MOVEABLE);
            }
        } else if (format->cfFormat == CF_HDROP) {
            handle = util::MakeDropFilesHandle(path_);
        } else if (format->cfFormat == RegisterClipboardFormatW(L"PNG") ||
                   format->cfFormat == RegisterClipboardFormatW(L"image/png")) {
            handle = MakeBytesHandle(png_);
        }

        if (!handle) {
            return STG_E_MEDIUMFULL;
        }

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format) {
            return E_INVALIDARG;
        }
        const UINT png = RegisterClipboardFormatW(L"PNG");
        const UINT pngMime = RegisterClipboardFormatW(L"image/png");
        bool known = format->cfFormat == CF_DIB || format->cfFormat == CF_HDROP ||
                     (png != 0 && format->cfFormat == png) ||
                     (pngMime != 0 && format->cfFormat == pngMime);
        bool supported = known && (format->tymed & TYMED_HGLOBAL);
        for (const ExtraFormat& extra : extra_) {
            if (extra.format.cfFormat == format->cfFormat &&
                (format->tymed & extra.medium.tymed)) {
                supported = true;
            }
        }
        if (!supported || (format->lindex != -1 && format->lindex != 0)) {
            return DV_E_FORMATETC;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out) {
            out->ptd = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium,
                                      BOOL release) override
    {
        if (!format || !medium || medium->tymed == 0) {
            return E_INVALIDARG;
        }
        ExtraFormat extra{};
        extra.format = *format;
        extra.medium = *medium;
        extra_.push_back(extra);
        if (release) {
            medium->pUnkForRelease = nullptr;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }

        std::vector<FORMATETC> all(formats_, formats_ + count_);
        for (const ExtraFormat& extra : extra_) {
            all.push_back(extra.format);
        }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(all.size()), all.data(), out);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    struct ExtraFormat {
        FORMATETC format{};
        STGMEDIUM medium{};
    };

    ~RackDataObject()
    {
        for (ExtraFormat& extra : extra_) {
            ReleaseStgMedium(&extra.medium);
        }
        if (dib_) {
            GlobalFree(dib_);
            dib_ = nullptr;
        }
    }

    static FORMATETC MakeFormat(UINT format)
    {
        FORMATETC value{};
        value.cfFormat = static_cast<CLIPFORMAT>(format);
        value.ptd = nullptr;
        value.dwAspect = DVASPECT_CONTENT;
        value.lindex = -1;
        value.tymed = TYMED_HGLOBAL;
        return value;
    }

    LONG ref_ = 1;
    std::wstring path_;
    std::vector<BYTE> png_;
    FORMATETC formats_[4]{};
    HGLOBAL dib_ = nullptr;
    UINT count_ = 0;
    std::vector<ExtraFormat> extra_;
};

/* 便签正文的拖拽数据对象：只提供文本，设计软件收到的是文字本身。 */
class RackTextDataObject final : public IDataObject {
public:
    RackTextDataObject(std::wstring text, bool exposeFormats)
        : text_(std::move(text)), expose_(exposeFormats) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG value = InterlockedDecrement(&ref_);
        if (value == 0) {
            delete this;
        }
        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!format || !medium) {
            return E_INVALIDARG;
        }
        if (!expose_) {
            return DV_E_FORMATETC;
        }
        if (format->lindex != -1 && format->lindex != 0) {
            return DV_E_FORMATETC;
        }
        if (!(format->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }
        HGLOBAL handle = nullptr;
        if (format->cfFormat == CF_UNICODETEXT) {
            const size_t bytes = (text_.size() + 1) * sizeof(wchar_t);
            handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            if (handle) {
                void* target = GlobalLock(handle);
                if (target) {
                    std::memcpy(target, text_.c_str(), text_.size() * sizeof(wchar_t));
                    GlobalUnlock(handle);
                }
            }
        } else if (format->cfFormat == CF_TEXT || format->cfFormat == CF_OEMTEXT) {
            const int need = WideCharToMultiByte(CP_ACP, 0, text_.c_str(),
                                                 static_cast<int>(text_.size()), nullptr, 0,
                                                 nullptr, nullptr);
            handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                                 static_cast<size_t>(need) + 1);
            if (handle) {
                auto* target = static_cast<char*>(GlobalLock(handle));
                if (target) {
                    WideCharToMultiByte(CP_ACP, 0, text_.c_str(),
                                        static_cast<int>(text_.size()), target, need, nullptr,
                                        nullptr);
                    target[need] = '\0';
                    GlobalUnlock(handle);
                }
            }
        } else {
            return DV_E_FORMATETC;
        }
        if (!handle) {
            return STG_E_MEDIUMFULL;
        }
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format) {
            return E_INVALIDARG;
        }
        if (!expose_) {
            return DV_E_FORMATETC;
        }
        const bool known = format->cfFormat == CF_UNICODETEXT ||
                           format->cfFormat == CF_TEXT || format->cfFormat == CF_OEMTEXT;
        if (!known || !(format->tymed & TYMED_HGLOBAL) ||
            (format->lindex != -1 && format->lindex != 0)) {
            return DV_E_FORMATETC;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        if (out) {
            *out = {};
            out->ptd = nullptr;
            out->dwAspect = DVASPECT_CONTENT;
            out->lindex = -1;
            out->tymed = TYMED_HGLOBAL;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        if (!expose_) {
            return E_NOTIMPL;
        }
        FORMATETC formats[2]{};
        formats[0].cfFormat = CF_UNICODETEXT;
        formats[1].cfFormat = CF_TEXT;
        for (FORMATETC& format : formats) {
            format.ptd = nullptr;
            format.dwAspect = DVASPECT_CONTENT;
            format.lindex = -1;
            format.tymed = TYMED_HGLOBAL;
        }
        return SHCreateStdEnumFmtEtc(2, formats, out);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~RackTextDataObject() = default;

    LONG ref_ = 1;
    std::wstring text_;
    bool expose_ = true;
};


/* 桌面文件夹（用户桌面 + 公共桌面）。 */
std::wstring DesktopFolderPath()
{
    PWSTR raw = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw)) && raw) {
        folder = raw;
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    return folder;
}

/* 从 ShellWindows 的 IDispatch 里取出它当前显示的文件夹路径。 */
std::wstring FolderFromShellDispatch(IDispatch* dispatch)
{
    if (!dispatch) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IServiceProvider> provider;
    if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) || !provider) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        !browser) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IFolderView2> folderView;
    if (FAILED(view.As(&folderView)) || !folderView) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || !folder) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IPersistFolder2> persist;
    if (FAILED(folder.As(&persist)) || !persist) {
        return std::wstring();
    }
    PIDLIST_ABSOLUTE current = nullptr;
    if (FAILED(persist->GetCurFolder(&current)) || !current) {
        return std::wstring();
    }
    PWSTR name = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetNameFromIDList(current, SIGDN_FILESYSPATH, &name)) && name) {
        result = name;
        CoTaskMemFree(name);
    }
    CoTaskMemFree(current);
    return result;
}

bool FilesEqual(const std::wstring& a, const std::wstring& b)
{
    std::ifstream left(a, std::ios::binary | std::ios::ate);
    std::ifstream right(b, std::ios::binary | std::ios::ate);
    if (!left || !right) return false;
    const std::streamoff size = left.tellg();
    if (size < 0 || size != right.tellg()) return false;
    left.seekg(0);
    right.seekg(0);
    std::vector<char> bufA(64 * 1024), bufB(64 * 1024);
    std::streamoff remaining = size;
    while (remaining > 0) {
        const std::streamsize chunk = static_cast<std::streamsize>(
            std::min<std::streamoff>(remaining, static_cast<std::streamoff>(bufA.size())));
        left.read(bufA.data(), chunk);
        right.read(bufB.data(), chunk);
        if (left.gcount() != chunk || right.gcount() != chunk) return false;
        if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(chunk)) != 0) return false;
        remaining -= chunk;
    }
    return true;
}

/* 光标落点对应的文件夹：桌面，或某个资源管理器窗口当前打开的目录。 */
std::wstring FolderUnderCursor(POINT cursor)
{
    HWND window = WindowFromPoint(cursor);
    if (!window) {
        return std::wstring();
    }
    const HWND top = GetAncestor(window, GA_ROOT);
    wchar_t name[64]{};
    if (top && GetClassNameW(top, name, static_cast<int>(std::size(name))) > 0) {
        if (_wcsicmp(name, L"Progman") == 0 || _wcsicmp(name, L"WorkerW") == 0) {
            return DesktopFolderPath();
        }
    }
    if (!top) {
        return std::wstring();
    }
    Microsoft::WRL::ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&windows))) ||
        !windows) {
        return std::wstring();
    }
    VARIANT variant{};
    VariantInit(&variant);
    variant.vt = VT_I4;
    variant.lVal = static_cast<LONG>(reinterpret_cast<LONG_PTR>(top));
    long shellHwnd = 0;
    Microsoft::WRL::ComPtr<IDispatch> dispatch;
    if (FAILED(windows->FindWindowSW(&variant, &variant, SWC_BROWSER, &shellHwnd,
                                     SWFO_NEEDDISPATCH, &dispatch)) ||
        !dispatch) {
        return std::wstring();
    }
    return FolderFromShellDispatch(dispatch.Get());
}

class RackDropSource final : public IDropSource {
public:
    /* rack 用来判断「有没有拖回绳子区域」：出去了再拖回来就取消这次拖拽，
       原卡片留在绳子上并轻微晃动提示，而不是把内容交给桌面复制一份。 */
    RackDropSource(HWND preview, RECT returnBand, DragOutcome* scene, std::wstring sourceFolder,
                   std::wstring sourcePath, bool fakeCursor = false)
        : preview_(preview), returnBand_(returnBand), scene_(scene),
          sourceFolder_(std::move(sourceFolder)), sourcePath_(std::move(sourcePath)),
          fakeCursor_(fakeCursor) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG value = InterlockedDecrement(&ref_);
        if (value == 0) {
            delete this;
        }
        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
    {
        if (escapePressed) {
            if (scene_) scene_->cancelled = true;
            return DRAGDROP_S_CANCEL;
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool insideRack = PtInRect(&returnBand_, cursor) != FALSE;
        if (!(keyState & MK_LBUTTON)) {
            /* 松手时还在绳子区域：按取消处理，终止本次拖拽。
               不再要求之前离开过绳子——拖出去一点又拖回来也算取消。 */
            if (insideRack) {
                if (scene_) {
                    scene_->returnedToRack = true;
                }
                return DRAGDROP_S_CANCEL;
            }
            /* 落在「源文件所在的同一个文件夹」里（包括桌面）：不是复制，取消这次拖拽。
               否则资源管理器会复制到同目录，弹出「源文件名和目标文件名相同」。 */
            const std::wstring targetFolder = FolderUnderCursor(cursor);
            const bool sameFolder = !targetFolder.empty() && !sourceFolder_.empty() &&
                                    util::SamePath(targetFolder, sourceFolder_);
            /* 同名同内容：桌面上的那份当作“同一个文件”，把它的图标移到落点；
               同名不同内容交给资源管理器替换；不同名同内容仍然正常复制。 */
            if (!targetFolder.empty() && !sourcePath_.empty()) {
                const std::wstring name =
                    std::filesystem::path(sourcePath_).filename().wstring();
                const std::wstring target = util::JoinPath(targetFolder, name);
                if (!name.empty() && util::FileExists(target) &&
                    !util::SamePath(target, sourcePath_) && FilesEqual(sourcePath_, target)) {
                    if (scene_) {
                        scene_->duplicate = true;
                        scene_->duplicatePath = target;
                        scene_->desktop = util::SamePath(targetFolder, DesktopFolderPath());
                        scene_->point = cursor;
                    }
                    return DRAGDROP_S_CANCEL;
                }
            }
            if (sameFolder) {
                if (scene_) {
                    scene_->sameFolder = true;
                    scene_->desktop = util::SamePath(targetFolder, DesktopFolderPath());
                    scene_->point = cursor;
                }
                return DRAGDROP_S_CANCEL;
            }
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        auto applyCursor = [this]() -> HRESULT {
            if (fakeCursor_) {
                UpdateFakeDragCursor();
                return S_OK;
            }
            /* 文件拖拽交给 OLE 自己画 copy / no-drop，保留系统自带的复制角标。 */
            return DRAGDROP_S_USEDEFAULTCURSORS;
        };
        if (preview_ && IsWindow(preview_)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            RECT rect{};
            GetWindowRect(preview_, &rect);
            const int x = cursor.x - (rect.right - rect.left) / 2;
            const int y = cursor.y - (rect.bottom - rect.top) / 2;
            /* 预览窗口创建时已经置顶：这里只平移，不再每次 SetWindowPos(HWND_TOPMOST)。
               反复请求置顶会触发窗口管理器重排，拖拽就会发涩。 */
            if (x == rect.left && y == rect.top) {
                return applyCursor();
            }
            SetWindowPos(preview_, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return applyCursor();
    }

private:
    ~RackDropSource() = default;
    HWND preview_ = nullptr;
    RECT returnBand_{};
    DragOutcome* scene_ = nullptr;
    std::wstring sourceFolder_;
    std::wstring sourcePath_;
    bool fakeCursor_ = false;
    LONG ref_ = 1;
};

} /* namespace */

void SetDragCursor()
{
    UpdateFakeDragCursor();
}

bool EditableAtPoint(POINT point)
{
    return UiaEditableAtPoint(point) || MsaaTextAtPoint(point);
}

bool RepositionDesktopIcon(const std::wstring& path, POINT screenPoint)
{
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    if (name.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&windows))) ||
        !windows) {
        return false;
    }
    VARIANT empty{};
    VariantInit(&empty);
    long shellHwnd = 0;
    Microsoft::WRL::ComPtr<IDispatch> dispatch;
    if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &shellHwnd,
                                     SWFO_NEEDDISPATCH, &dispatch)) ||
        !dispatch) {
        return false;
    }
    Microsoft::WRL::ComPtr<IServiceProvider> provider;
    if (FAILED(dispatch.As(&provider)) || !provider) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        !browser) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) {
        return false;
    }
    Microsoft::WRL::ComPtr<IFolderView2> folderView;
    if (FAILED(view.As(&folderView)) || !folderView) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&folder))) || !folder) {
        return false;
    }
    PIDLIST_RELATIVE relative = nullptr;
    const HRESULT parsed = folder->ParseDisplayName(
        reinterpret_cast<HWND>(static_cast<LONG_PTR>(shellHwnd)), nullptr,
        const_cast<LPWSTR>(name.c_str()), nullptr, &relative, nullptr);
    if (FAILED(parsed) || !relative) {
        return false;
    }
    HWND viewHwnd = nullptr;
    view->GetWindow(&viewHwnd);
    POINT client = screenPoint;
    if (viewHwnd) {
        ScreenToClient(viewHwnd, &client);
    }
    PCUITEMID_CHILD items[1] = {relative};
    const HRESULT placed =
        folderView->SelectAndPositionItems(1, items, &client, SVSI_POSITIONITEM | SVSI_SELECT);
    CoTaskMemFree(relative);
    return SUCCEEDED(placed);
}

DWORD BeginDrag(const std::wstring& path, HWND rack, RECT returnBand, int cardWidth, int cardHeight,
                float frameWidth, bool* dropped, DragOutcome* outcome)
{
    (void)rack;
    if (dropped) *dropped = false;
    if (outcome) *outcome = DragOutcome{};
    if (!util::FileExists(path)) {
        return DROPEFFECT_NONE;
    }

    SIZE dragSize{};
    HBITMAP dragBitmap =
        MakeDragBitmap(path, cardWidth, cardHeight, frameWidth, &dragSize);
    HWND dragPreview = dragBitmap
                           ? CreateDragPreview(dragBitmap, dragSize.cx, dragSize.cy)
                           : nullptr;

    auto* data = new (std::nothrow) RackDataObject(path);
    auto* source = new (std::nothrow) RackDropSource(
        dragPreview, returnBand, outcome,
        std::filesystem::path(path).parent_path().wstring(), path);
    if (!data || !source) {
        if (data) {
            data->Release();
        }
        if (source) {
            source->Release();
        }
        if (dragPreview) {
            DestroyWindow(dragPreview);
        }
        if (dragBitmap) {
            DeleteObject(dragBitmap);
        }
        return DROPEFFECT_NONE;
    }

    /* 把 OLE 自带的缩略图藏起来：真正显示的是上面那层分层窗口，
       它有逐像素的圆角。 */
    HBITMAP invisible = nullptr;
    IDragSourceHelper* helper = nullptr;
    BITMAPINFO emptyInfo{};
    emptyInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    emptyInfo.bmiHeader.biWidth = 1;
    emptyInfo.bmiHeader.biHeight = -1;
    emptyInfo.bmiHeader.biPlanes = 1;
    emptyInfo.bmiHeader.biBitCount = 32;
    void* emptyBits = nullptr;
    invisible = CreateDIBSection(nullptr, &emptyInfo, DIB_RGB_COLORS, &emptyBits,
                                 nullptr, 0);
    if (invisible && SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr,
                                                CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&helper)))) {
        SHDRAGIMAGE image{};
        image.sizeDragImage = {1, 1};
        image.hbmpDragImage = invisible;
        const HRESULT emptyHr = helper->InitializeFromBitmap(&image, data);
        if (FAILED(emptyHr)) {
            RackLog(L"DragDrop: transparent drag image failed 0x%08X",
                    static_cast<unsigned>(emptyHr));
        }
    }

    /*
     * 只给 COPY。给 MOVE 的话，用户把文件拖到文件夹时，Explorer 会真的把
     * 轻雪的原图从输出目录移走；插件绝不能移动或删除宿主的文件。
     */
    DWORD effect = DROPEFFECT_NONE;
    /* 只允许复制：移动由我们自己在「落点在桌面」时完成，
       免得拖到别的文件夹时把用户的文件搬走。 */
    HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    if (helper) {
        helper->Release();
    }
    if (invisible) {
        DeleteObject(invisible);
    }
    if (dragPreview) {
        DestroyWindow(dragPreview);
    }
    if (dragBitmap) {
        DeleteObject(dragBitmap);
    }
    data->Release();
    source->Release();

    if (FAILED(hr) && hr != DRAGDROP_S_DROP && hr != DRAGDROP_S_CANCEL) {
        RackLog(L"DragDrop: DoDragDrop failed 0x%08X", static_cast<unsigned>(hr));
        return DROPEFFECT_NONE;
    }
    if (dropped) {
        *dropped = effect != DROPEFFECT_NONE;
    }
    return effect;
}

DWORD BeginTextDrag(const std::wstring& text, HWND rack, RECT returnBand, HBITMAP ghost,
                    SIZE ghostSize,
                    int cardWidth, int cardHeight, float frameWidth, bool* dropped,
                    DragOutcome* outcome, const std::wstring& previewPath, bool pasteOnly)
{
    (void)rack;
    if (dropped) *dropped = false;
    if (outcome) *outcome = DragOutcome{};
    if (text.empty()) {
        if (ghost) {
            DeleteObject(ghost);
        }
        return DROPEFFECT_NONE;
    }

    SIZE dragSize = ghostSize;
    HBITMAP dragBitmap = ghost;
    if (!dragBitmap && !previewPath.empty()) {
        dragBitmap = MakeDragBitmap(previewPath, cardWidth, cardHeight, frameWidth, &dragSize);
    }
    if (!dragBitmap) {
        dragBitmap = MakeTextDragBitmap(text, cardWidth, cardHeight, frameWidth, &dragSize);
    }
    HWND dragPreview = dragBitmap ? CreateDragPreview(dragBitmap, dragSize.cx, dragSize.cy)
                                  : nullptr;

    auto* data = new (std::nothrow) RackTextDataObject(text, !pasteOnly);
    auto* source = new (std::nothrow) RackDropSource(dragPreview, returnBand, outcome, std::wstring(),
                                                      std::wstring(), pasteOnly);
    if (!data || !source) {
        if (data) {
            data->Release();
        }
        if (source) {
            source->Release();
        }
        if (dragPreview) {
            DestroyWindow(dragPreview);
        }
        if (dragBitmap) {
            DeleteObject(dragBitmap);
        }
        return DROPEFFECT_NONE;
    }

    /* 和图片拖拽一样：把 OLE 自带的缩略图藏起来，用上面那层分层窗口显示卡片。 */
    HBITMAP invisible = nullptr;
    IDragSourceHelper* helper = nullptr;
    BITMAPINFO emptyInfo{};
    emptyInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    emptyInfo.bmiHeader.biWidth = 1;
    emptyInfo.bmiHeader.biHeight = -1;
    emptyInfo.bmiHeader.biPlanes = 1;
    emptyInfo.bmiHeader.biBitCount = 32;
    void* emptyBits = nullptr;
    invisible = CreateDIBSection(nullptr, &emptyInfo, DIB_RGB_COLORS, &emptyBits, nullptr, 0);
    if (invisible && SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr,
                                                CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&helper)))) {
        SHDRAGIMAGE image{};
        image.sizeDragImage = {1, 1};
        image.hbmpDragImage = invisible;
        helper->InitializeFromBitmap(&image, data);
    }

    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    if (helper) {
        helper->Release();
    }
    if (invisible) {
        DeleteObject(invisible);
    }
    if (dragPreview) {
        DestroyWindow(dragPreview);
    }
    if (dragBitmap) {
        DeleteObject(dragBitmap);
    }
    data->Release();
    source->Release();

    if (FAILED(hr) && hr != DRAGDROP_S_DROP && hr != DRAGDROP_S_CANCEL) {
        RackLog(L"DragDrop: DoDragDrop failed 0x%08X", static_cast<unsigned>(hr));
        return DROPEFFECT_NONE;
    }
    if (dropped) {
        *dropped = effect != DROPEFFECT_NONE;
    }
    return effect;
}

} /* namespace dragdrop */
