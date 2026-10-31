#include "DragDrop.h"
#include "Util.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace dragdrop {
namespace {


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
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(frameDecode.Get(), photoW, photoH,
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
    std::vector<BYTE> photo(photoStride * static_cast<UINT>(photoH));
    if (FAILED(converter->CopyPixels(nullptr, photoStride,
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

        const UINT drop = CF_HDROP;
        const UINT png = RegisterClipboardFormatW(L"PNG");
        const UINT text = CF_UNICODETEXT;

        formats_[0] = MakeFormat(drop);
        formats_[1] = MakeFormat(png);
        formats_[2] = MakeFormat(text);
        count_ = png ? 3 : 2;
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
        if (format->cfFormat == CF_HDROP) {
            handle = util::MakeDropFilesHandle(path_);
        } else if (format->cfFormat == CF_UNICODETEXT) {
            const size_t bytes = (path_.size() + 1) * sizeof(wchar_t);
            handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            if (handle) {
                void* target = GlobalLock(handle);
                if (target) {
                    std::memcpy(target, path_.c_str(), path_.size() * sizeof(wchar_t));
                    GlobalUnlock(handle);
                }
            }
        } else {
            UINT png = RegisterClipboardFormatW(L"PNG");
            if (format->cfFormat == png) {
                handle = MakeBytesHandle(png_);
            }
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
        bool known = format->cfFormat == CF_HDROP ||
                     format->cfFormat == CF_UNICODETEXT ||
                     (png != 0 && format->cfFormat == png);
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
    FORMATETC formats_[3]{};
    UINT count_ = 0;
    std::vector<ExtraFormat> extra_;
};

class RackDropSource final : public IDropSource {
public:
    explicit RackDropSource(HWND preview) : preview_(preview) {}

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
            return DRAGDROP_S_CANCEL;
        }
        if (!(keyState & MK_LBUTTON)) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        if (preview_ && IsWindow(preview_)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            RECT rect{};
            GetWindowRect(preview_, &rect);
            SetWindowPos(preview_, HWND_TOPMOST,
                         cursor.x - (rect.right - rect.left) / 2,
                         cursor.y - (rect.bottom - rect.top) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~RackDropSource() = default;
    HWND preview_ = nullptr;
    LONG ref_ = 1;
};

} /* namespace */

DWORD BeginDrag(const std::wstring& path, int cardWidth, int cardHeight,
                float frameWidth, bool* dropped)
{
    if (dropped) {
        *dropped = false;
    }
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
    auto* source = new (std::nothrow) RackDropSource(dragPreview);
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
