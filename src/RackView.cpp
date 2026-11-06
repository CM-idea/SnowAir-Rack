#include "RackView.h"
#include "NotePalette.h"
#include "Graphics.h"
#include "Util.h"
#include <cwctype>
#include <filesystem>
#include <map>

namespace {

constexpr float kLineY = 14.0f;
constexpr float kCardTop = 42.0f;
/* 卡片规格：照片最大 136x104，卡片宽 150，卡片间距 24。 */
constexpr float kPhotoMaxWidth = 136.0f;
constexpr float kNoteCardWidth = 150.0f;
constexpr float kNoteCardHeight = 136.0f;
/* 新式非图片卡：150 方卡，头部 33，圆角 15，20% 外圈 4.5。 */
constexpr float kFlatCardSize = 150.0f;
constexpr float kFlatHeader = 33.0f;
constexpr float kFlatRadius = 15.0f;
constexpr float kFlatRing = 4.5f;
constexpr float kColorRowHeight = 36.0f;
constexpr float kPhotoMaxHeight = 104.0f;
constexpr float kClipSlotRatio = 0.32f;
constexpr float kGap = 24.0f;
constexpr float kWindowHeight = 520.0f;
constexpr float kNoteHeight = 24.0f;
constexpr float kFrame = 4.0f;
constexpr float kOuterPad = 24.0f;
constexpr float kAccessoryWidth = 260.0f;

/* 轻雪渲染的 SVG 预览缓存是 <id>.svg3.png；这张图已经裁到墨迹边缘，
   卡片里再铺满就会贴边，所以按它识别、单独留一圈白。 */
bool IsSvgPreviewPath(const std::wstring& path)
{
    constexpr size_t kSuffixLen = 9;   // ".svg3.png"
    if (path.size() < kSuffixLen) return false;
    std::wstring tail = path.substr(path.size() - kSuffixLen);
    for (auto& c : tail) c = static_cast<wchar_t>(towlower(c));
    return tail == L".svg3.png";
}
/* 截图/便签入场动画的时长，Layout、Draw 和 HasArrival 共用，
   保证交接只发生在同一帧。 */
constexpr float kArrivalSeconds = 0.65f;
/* 卡片位置的目标变了之后，先停这么久再开始靠拢。 */
constexpr uint64_t kCloseHoldMs = 140;

/* 弹簧积分器：位置和目标之间的阻尼弹簧，卡片靠拢/让位动画使用它。
   参数和参考实现一致（response 大一点更慢，damping 越小越弹）。 */
void SlideSpring(float& position, float& velocity, float target, float dt, float response,
                 float damping)
{
    if (dt <= 0.0f) {
        return;
    }
    const float natural = 2.0f * 3.14159265f / response;
    const float decay = natural * damping;
    const float damped = natural * std::sqrt(std::max(0.0f, 1.0f - damping * damping));
    if (damped <= 0.0f) {
        position = target;
        velocity = 0.0f;
        return;
    }
    const float offset = position - target;
    const float sine = std::sin(damped * dt);
    const float cosine = std::cos(damped * dt);
    const float envelope = std::exp(-decay * dt);
    const float coefficient = (velocity + decay * offset) / damped;
    position = target + envelope * (offset * cosine + coefficient * sine);
    velocity = envelope * (velocity * cosine - (decay * coefficient + damped * offset) * sine);
    if (std::abs(position - target) < 0.0015f && std::abs(velocity) < 0.01f) {
        position = target;
        velocity = 0.0f;
    }
}

/* 文件卡片标题里带着「等 N 个」：取出来显示成「N 个文件」，取不到按单个处理。 */
int FileCountFromLabel(const std::wstring& label)
{
    const std::wstring marker = L" 等 ";
    const size_t pos = label.find(marker);
    if (pos == std::wstring::npos) {
        return 1;
    }
    size_t index = pos + marker.size();
    int value = 0;
    bool found = false;
    while (index < label.size() && label[index] >= L'0' && label[index] <= L'9') {
        value = value * 10 + (label[index] - L'0');
        ++index;
        found = true;
    }
    return found && value > 0 ? value : 1;
}

std::wstring ReadRegistryString(HKEY root, const std::wstring& key, const wchar_t* value)
{
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG status =
        RegQueryValueExW(handle, value, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return std::wstring();
    }
    std::wstring text(buffer);
    while (!text.empty() && (text.back() == L'\0' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

/* 可执行文件的「文件说明」（版本信息里的 FileDescription），没有就退回文件名。 */
std::wstring ExeDisplayName(const std::wstring& exePath)
{
    std::wstring description;
    DWORD unused = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(exePath.c_str(), &unused);
    if (bytes > 0) {
        std::vector<BYTE> data(bytes);
        if (GetFileVersionInfoW(exePath.c_str(), 0, bytes, data.data())) {
            struct Translation {
                WORD language;
                WORD codepage;
            };
            Translation* translations = nullptr;
            UINT length = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<void**>(&translations), &length) &&
                length >= sizeof(Translation)) {
                for (UINT i = 0; i < length / sizeof(Translation) && description.empty(); ++i) {
                    wchar_t query[128]{};
                    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                               translations[i].language, translations[i].codepage);
                    wchar_t* text = nullptr;
                    UINT size = 0;
                    if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&text),
                                       &size) &&
                        text && size > 1) {
                        description = text;
                    }
                }
            }
        }
    }
    if (description.empty()) {
        description = std::filesystem::path(exePath).stem().wstring();
        for (auto& ch : description) {
            ch = static_cast<wchar_t>(towupper(ch));
        }
    }
    return description;
}

/* 从「.docx」的默认打开命令里取出程序名：WPS Office / Word / 记事本…… */
std::wstring FileDescriptionOfAssociation(const std::wstring& extension)
{
    std::wstring progId = ReadRegistryString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + extension +
            L"\\UserChoice",
        L"ProgId");
    if (progId.empty()) {
        progId = ReadRegistryString(HKEY_CLASSES_ROOT, extension, L"");
    }
    if (progId.empty()) {
        return std::wstring();
    }
    std::wstring command = ReadRegistryString(HKEY_CLASSES_ROOT, progId + L"\\shell\\open\\command", L"");
    if (command.empty()) {
        return std::wstring();
    }
    /* 命令形如 "\"C:\\...\\wps.exe\" \"%1\""，取第一段。 */
    std::wstring exePath = command;
    const size_t quote = exePath.find(L'"');
    if (quote != std::wstring::npos) {
        const size_t closing = exePath.find(L'"', quote + 1);
        if (closing != std::wstring::npos) {
            exePath = exePath.substr(quote + 1, closing - quote - 1);
        }
    } else {
        const size_t space = exePath.find(L' ');
        if (space != std::wstring::npos) {
            exePath = exePath.substr(0, space);
        }
    }
    if (exePath.empty() || !std::filesystem::exists(exePath)) {
        return std::wstring();
    }
    return ExeDisplayName(exePath);
}

/* 扩展名默认交给哪个程序打开：.docx → WPS / Word，用来在文件夹图标右边标注类型。 */
std::wstring AppNameForExtension(const std::wstring& extension)
{
    if (extension.empty()) {
        return std::wstring();
    }
    static std::map<std::wstring, std::wstring> cache;
    const auto cached = cache.find(extension);
    if (cached != cache.end()) {
        return cached->second;
    }

    /* 先问系统默认程序；WPS 这类走启动器的关联常常查不到，再退回注册表里的
       打开命令，最后用可执行文件名。 */
    std::wstring name;
    wchar_t buffer[512]{};
    DWORD length = static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t));
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_FRIENDLYAPPNAME,
                                    extension.c_str(), nullptr, buffer, &length)) &&
        buffer[0] != L'\0') {
        name = buffer;
    }
    if (name.empty()) {
        name = FileDescriptionOfAssociation(extension);
    }
    /* 「WPS Office」这类产品后缀去掉，只留名字。 */
    const std::wstring office = L" Office";
    if (name.size() > office.size() &&
        name.compare(name.size() - office.size(), office.size(), office) == 0) {
        name.erase(name.size() - office.size());
    }
    if (name.empty()) {
        name = extension;
        if (!name.empty() && name[0] == L'.') {
            name.erase(0, 1);
        }
        for (auto& ch : name) {
            ch = static_cast<wchar_t>(towupper(ch));
        }
    }
    cache[extension] = name;
    return name;
}

D2D1_POINT_2F RotateAround(D2D1_POINT_2F point, D2D1_POINT_2F center, float degrees)
{
    const float radians = degrees * 3.1415926535f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float dx = point.x - center.x;
    const float dy = point.y - center.y;
    return D2D1::Point2F(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
}

float CardEntryOffset(const Item& item)
{
    const uint64_t now = rack::NowMs();
    if (item.addedMs == 0 || now <= item.addedMs) {
        return 0.0f;
    }
    const float age = static_cast<float>(now - item.addedMs) / 1000.0f;
    if (age > 1.2f) {
        return 0.0f;
    }
    /* 上绳落位用的弹簧：响应 0.42 s、阻尼 0.72、初始偏移 -46。 */
    constexpr float kResponse = 0.42f;
    constexpr float kDamping = 0.72f;
    const float natural = 2.0f * 3.14159265f / kResponse;
    const float decay = natural * kDamping;
    const float damped = natural * std::sqrt(1.0f - kDamping * kDamping);
    return -46.0f * std::exp(-decay * age) *
           (std::cos(damped * age) + (decay / damped) * std::sin(damped * age));
}

bool IsNewCard(const Item& item)
{
    switch (item.cardType) {
    case card::Type::Note:
    case card::Type::Text:
    case card::Type::Link:
    case card::Type::Color:
    case card::Type::File:
    case card::Type::Video:
    case card::Type::Music: return true;
    default: return false;
    }
}

D2D1_COLOR_F AccentFor(const Item& item)
{
    const uint32_t rgb = item.accentSet ? item.accent : card::DefaultAccent(item.cardType);
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

D2D1_COLOR_F RgbColor(uint32_t rgb)
{
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

D2D1_COLOR_F MixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    return D2D1::ColorF(rack::Lerp(a.r, b.r, t), rack::Lerp(a.g, b.g, t),
                        rack::Lerp(a.b, b.b, t), 1.0f);
}

bool AccentUsesDarkInk(const D2D1_COLOR_F& accent)
{
    const float luminance = 0.299f * accent.r + 0.587f * accent.g + 0.114f * accent.b;
    /* 只有接近白/特别浅的头部才用深字；普通彩色头部保持白字。 */
    return luminance > 0.85f;
}

int ColorCountFor(const Item& item)
{
    std::vector<card::Rgba> colors;
    if (!item.note.empty() && card::ParseColors(item.note, colors)) {
        return static_cast<int>(colors.size());
    }
    return 1;
}

int CardCountFor(const Item& item)
{
    if (item.cardType == card::Type::Color) {
        return std::max(1, ColorCountFor(item));
    }
    if (item.cardType == card::Type::File || item.cardType == card::Type::Music ||
        item.cardType == card::Type::Video) {
        const int parsed = FileCountFromLabel(item.label);
        if (parsed > 1) return parsed;
        if (!item.paths.empty()) return static_cast<int>(item.paths.size());
        return 1;
    }
    return 1;
}

/* 便签比普通方卡略大一点，避免一排卡片过于规整。 */
float FlatCardSide(const Item& item, float cs)
{
    const float factor = item.cardType == card::Type::Note ? 1.08f : 1.0f;
    return kFlatCardSize * cs * factor;
}

/*
 * 新式卡的内部几何：头部 / 正文 / 链接命中区 / 颜色行。
 * rect 用实际绘制矩形（含按下位移），色卡滚动量按 150 基准缩放。
 */
void BuildCardMetrics(const Item& item, const D2D1_RECT_F& rect, CardBox& out)
{
    out.card = rect;
    out.flat = true;
    const float u = (rect.right - rect.left) / kFlatCardSize;
    out.header = D2D1::RectF(rect.left, rect.top, rect.right, rect.top + kFlatHeader * u);
    out.body = D2D1::RectF(rect.left, out.header.bottom, rect.right, rect.bottom);
    out.linkText = rack::Inset(out.body, 11.0f * u, 8.0f * u);
    out.colorRows.clear();
    if (item.cardType != card::Type::Color) return;

    std::vector<card::Rgba> colors;
    if (!card::ParseColors(item.note, colors)) return;
    const float pad = 11.0f * u;
    const float swatch = 30.0f * u;
    const float left = rect.left + pad;
    const float top = out.body.top + 4.0f * u - item.colorScroll * u;
    out.colorRows.reserve(colors.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        ColorRowBox row;
        row.index = static_cast<int>(i);
        row.row = D2D1::RectF(left, top + static_cast<float>(i) * kColorRowHeight * u,
                              rect.right - pad,
                              top + static_cast<float>(i + 1) * kColorRowHeight * u);
        row.swatch = D2D1::RectF(row.row.left, row.row.top + 3.0f * u, row.row.left + swatch,
                                 row.row.top + 3.0f * u + swatch);
        const float textLeft = row.swatch.right + 8.0f * u;
        D2D1_RECT_F hexRect = D2D1::RectF(textLeft, row.row.top + 3.0f * u,
                                          rect.right - pad, row.row.top + 18.0f * u);
        D2D1_RECT_F rgbRect = D2D1::RectF(textLeft, row.row.top + 18.0f * u,
                                          rect.right - pad, row.row.top + 32.0f * u);
        /* rgb() 来源的卡把 rgb 放上面，hex 放下面；两种来源的卡外观能区分。 */
        if (card::ColorTextIsRgb(item.note)) {
            std::swap(hexRect, rgbRect);
        }
        row.hex = hexRect;
        row.rgb = rgbRect;
        out.colorRows.push_back(row);
    }
}

void ShiftCard(CardBox& box, float dy)
{
    box.card.top += dy;
    box.card.bottom += dy;
    box.image.top += dy;
    box.image.bottom += dy;
    box.header.top += dy;
    box.header.bottom += dy;
    box.body.top += dy;
    box.body.bottom += dy;
    box.linkText.top += dy;
    box.linkText.bottom += dy;
    for (auto& row : box.colorRows) {
        row.row.top += dy;
        row.row.bottom += dy;
        row.swatch.top += dy;
        row.swatch.bottom += dy;
        row.hex.top += dy;
        row.hex.bottom += dy;
        row.rgb.top += dy;
        row.rgb.bottom += dy;
    }
    if (box.hasNote) {
        box.note.top += dy;
        box.note.bottom += dy;
    }
}

} /* namespace */

bool RackView::Init(Graphics* graphics)
{
    graphics_ = graphics;
    if (!graphics_ || !graphics_->dwrite()) {
        return false;
    }
    EnsureFormats(1.0f);
    return noteFormat_ != nullptr && placeholderFormat_ != nullptr &&
           toastFormat_ != nullptr && cardTitleFormat_ != nullptr &&
           cardBodyFormat_ != nullptr;
}

void RackView::LoadAssets(ID2D1RenderTarget* target)
{
    if (assetsAttempted_ || !graphics_ || !target) {
        return;
    }
    assetsAttempted_ = true;
    const std::wstring dir = util::JoinPath(util::GetDllDir(), L"assets");
    /* 每张位图按接近实际显示尺寸的两倍解码。让 D2D 在绘制时把 340px 的
       信封缩到 70px，边缘会出现明显的台阶；这里先用 WIC Fant 缩好。 */
    const float assetScale = std::max(1.0f, scale_);
    graphics_->images().Load(util::JoinPath(dir, L"basket.png"), 224.0f * assetScale,
                             target, basket_);
    static const int kBasketFile[4] = {3, 0, 2, 1};  // 银灰, 奶白, 木棉, 深沉
    for (int i = 0; i < 4; ++i) {
        graphics_->images().Load(
            util::JoinPath(dir, L"basket_" + std::to_wstring(kBasketFile[i]) + L".png"),
            224.0f * assetScale, target, basketTheme_[i]);
    }
    graphics_->images().Load(util::JoinPath(dir, L"envelope_back.png"),
                             140.0f * assetScale, target, envelope_);
    graphics_->images().Load(util::JoinPath(dir, L"envelope_front.png"),
                             140.0f * assetScale, target, envelopeFront_);
    graphics_->images().Load(util::JoinPath(dir, L"letter.png"), 130.0f * assetScale,
                             target, letter_);
    static const int kStationeryFile[4] = {1, 0, 2, 3};  // 银灰, 奶白, 木棉, 深沉
    for (int i = 0; i < 4; ++i) {
        const std::wstring prefix = L"envelope_" + std::to_wstring(kStationeryFile[i]);
        graphics_->images().Load(util::JoinPath(dir, prefix + L"_back.png"),
                                 140.0f * assetScale, target, envelopeThemeBack_[i]);
        graphics_->images().Load(util::JoinPath(dir, prefix + L"_front.png"),
                                 140.0f * assetScale, target, envelopeThemeFront_[i]);
    }
    LoadIconFont();
}

void RackView::LoadIconFont()
{
    if (iconFace_ || !graphics_ || !graphics_->dwrite() || !graphics_->d2d()) {
        return;
    }
    const std::wstring path =
        util::JoinPath(util::JoinPath(util::GetDllDir(), L"assets"), L"iconfont.ttf");
    ComPtr<IDWriteFontFile> file;
    if (FAILED(graphics_->dwrite()->CreateFontFileReference(
            path.c_str(), nullptr, file.GetAddressOf())) || !file) {
        return;
    }
    IDWriteFontFile* files[] = {file.Get()};
    if (FAILED(graphics_->dwrite()->CreateFontFace(
            DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, files, 0,
            DWRITE_FONT_SIMULATIONS_NONE, iconFace_.GetAddressOf())) ||
        !iconFace_) {
        return;
    }
    BuildIconGlyph(0xE641, iconLock_);
    BuildIconGlyph(0xE643, iconUnlock_);
    BuildIconGlyph(0xE624, iconClose_);
    BuildIconGlyph(0xE61C, iconFolder_); /* 文件夹（和剪切板面板同一个字形） */
    /* 卡片类型图标统一用 font_5228143 / SnowAirIcons.ttf 里的字形。 */
    BuildIconGlyph(0xE67E, iconLink_);   /* lianjie */
    BuildIconGlyph(0xE61D, iconText_);   /* a-shibiewenzi1：参考稿的扫描框 */
    BuildIconGlyph(0xE673, iconColor_);  /* wuse */
    BuildIconGlyph(0xE686, iconVideo_);  /* shipin */
    BuildIconGlyph(0xE67F, iconMusic_);  /* yinle */
}

void RackView::BuildIconGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out)
{
    if (!iconFace_ || !graphics_ || !graphics_->d2d()) {
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(graphics_->d2d()->CreatePathGeometry(geometry.GetAddressOf())) ||
        !geometry) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) {
        return;
    }
    UINT16 glyph = 0;
    if (iconFace_->GetGlyphIndices(&codepoint, 1, &glyph) != S_OK) {
        return;
    }
    FLOAT advance = 0.0f;
    auto* writeSink = reinterpret_cast<IDWriteGeometrySink*>(sink.Get());
    const HRESULT hr = iconFace_->GetGlyphRunOutline(
        100.0f, &glyph, &advance, nullptr, 1, FALSE, FALSE, writeSink);
    sink->Close();
    if (SUCCEEDED(hr)) {
        out = geometry;
    }
}

void RackView::DrawIcon(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                        D2D1_POINT_2F center, float size, ID2D1Brush* brush)
{
    if (!target || !glyph || !brush) {
        return;
    }
    D2D1_RECT_F bounds{};
    if (FAILED(glyph->GetBounds(D2D1::Matrix3x2F::Identity(), &bounds))) {
        return;
    }
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    const float scale = size / std::max(width, height);
    D2D1_MATRIX_3X2_F saved{};
    target->GetTransform(&saved);
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(
            center.x - (bounds.left + bounds.right) * 0.5f * scale,
            center.y - (bounds.top + bounds.bottom) * 0.5f * scale);
    target->SetTransform(transform * saved);
    target->FillGeometry(glyph, brush);
    target->SetTransform(saved);
}

void RackView::EnsureFormats(float scale)
{
    if (!graphics_ || !graphics_->dwrite()) {
        return;
    }
    if (noteFormat_ && placeholderFormat_ && badgeFormat_ && toastFormat_ &&
        cardTitleFormat_ && cardMetaFormat_ && cardBodyFormat_ && cardColorHexFormat_ &&
        cardColorRgbFormat_ && cardColorHexBoldFormat_ && cardColorRgbBoldFormat_ &&
        cardFileNameFormat_ && cardFileNameWrapFormat_ &&
        std::abs(formatScale_ - scale) < 0.001f &&
        std::abs(formatCardScale_ - cardScale_) < 0.001f) {
        return;
    }

    auto makeFormat = [&](float size, DWRITE_FONT_WEIGHT weight =
                                           DWRITE_FONT_WEIGHT_NORMAL)
        -> ComPtr<IDWriteTextFormat> {
        ComPtr<IDWriteTextFormat> format;
        HRESULT hr = graphics_->dwrite()->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        if (FAILED(hr)) {
            hr = graphics_->dwrite()->CreateTextFormat(
                L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.GetAddressOf());
        }
        if (SUCCEEDED(hr) && format) {
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        return format;
    };

    noteFormat_ = makeFormat(10.0f * scale);
    placeholderFormat_ = makeFormat(9.0f * scale);
    badgeFormat_ = makeFormat(10.0f * scale);
    fileTagFormat_ = makeFormat(10.0f * scale);
    if (fileTagFormat_) {
        fileTagFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fileTagFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (badgeFormat_) {
        badgeFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        badgeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        badgeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    toastFormat_ = makeFormat(12.0f * scale);
    if (toastFormat_) {
        toastFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        toastFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        toastFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    /* 新式非图片卡：标题/元信息按卡片档位给固定字号，其余文字按同一档位
       微调。这里 scale 是「Windows 缩放 × 卡片档位」，ui 只是 Windows 缩放。 */
    const float ui = cardScale_ > 0.5f ? scale / cardScale_ : scale;
    float titleSize = 12.0f;
    float metaSize = 10.0f;
    float bodySize = 11.5f;
    float colorSize = 10.5f;
    float fileSize = 11.0f;
    if (cardScale_ <= 0.9f) {
        titleSize = 10.0f;
        metaSize = 8.0f;
        bodySize = 9.5f;
        colorSize = 9.0f;
        fileSize = 9.0f;
    } else if (cardScale_ >= 1.2f) {
        titleSize = 14.0f;
        metaSize = 12.0f;
        bodySize = 13.5f;
        colorSize = 12.5f;
        fileSize = 12.8f;
    }
    cardTitleFormat_ = makeFormat(titleSize * ui);
    if (cardTitleFormat_) {
        cardTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cardMetaFormat_ = makeFormat(metaSize * ui);
    if (cardMetaFormat_) cardMetaFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    cardBodyFormat_ = makeFormat(bodySize * ui);
    /* Hex 和 rgb 用同一字号；10.5 能保证 rgb(22,119,255) 在 150 卡内不被裁掉。 */
    cardColorHexFormat_ = makeFormat(colorSize * ui);
    if (cardColorHexFormat_) {
        cardColorHexFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cardColorRgbFormat_ = makeFormat(colorSize * ui);
    if (cardColorRgbFormat_) cardColorRgbFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    cardColorHexBoldFormat_ =
        makeFormat(colorSize * ui, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (cardColorHexBoldFormat_) {
        cardColorHexBoldFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cardColorRgbBoldFormat_ =
        makeFormat(colorSize * ui, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (cardColorRgbBoldFormat_) {
        cardColorRgbBoldFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cardFileNameFormat_ = makeFormat(fileSize * ui);
    if (cardFileNameFormat_) {
        cardFileNameFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    cardFileNameWrapFormat_ = makeFormat(fileSize * ui);
    if (cardFileNameWrapFormat_) {
        cardFileNameWrapFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    formatScale_ = scale;
    formatCardScale_ = cardScale_;
}

void RackView::Shutdown()
{
    noteFormat_.Reset();
    placeholderFormat_.Reset();
    badgeFormat_.Reset();
    toastFormat_.Reset();
    cardTitleFormat_.Reset();
    cardMetaFormat_.Reset();
    cardBodyFormat_.Reset();
    cardColorHexFormat_.Reset();
    cardColorRgbFormat_.Reset();
    cardColorHexBoldFormat_.Reset();
    cardColorRgbBoldFormat_.Reset();
    cardFileNameFormat_.Reset();
    cardFileNameWrapFormat_.Reset();
    iconFace_.Reset();
    iconLock_.Reset();
    iconUnlock_.Reset();
    iconClose_.Reset();
    basket_.Reset();
    for (auto& basket : basketTheme_) basket.Reset();
    envelope_.Reset();
    envelopeFront_.Reset();
    letter_.Reset();
    for (int i = 0; i < 4; ++i) {
        envelopeThemeBack_[i].Reset();
        envelopeThemeFront_[i].Reset();
    }
    assetsAttempted_ = false;
    cards_.clear();
    graphics_ = nullptr;
}

RackLayout RackView::Layout(const std::vector<Item>& items, float maxWidth, float scale,
                            float expandAmount, float cardScale)
{
    RackLayout result;
    scale_ = std::max(0.5f, scale);
    cardScale_ = rack::Clamp(cardScale, 0.8f, 1.35f);
    const float cs = scale_ * cardScale_;
    /* 字号跟着卡片大小走：小/中/大三档不是只缩卡片、字号不动。 */
    EnsureFormats(cs);
    const float s = scale_;
    result.width = std::max(320.0f * s, maxWidth);
    result.height = kWindowHeight * s;
    result.lineY = kLineY * s;
    result.expanded = expandAmount > 0.5f;
    layout_ = result;

    cards_.clear();
    basketRect_ = {};
    envelopeRect_ = {};

    const float photoWidth =
        std::max(120.0f * s, result.width - (2.0f * kOuterPad + kAccessoryWidth) * s);
    const float maxContent = photoWidth;
    std::vector<float> widths(items.size(), 0.0f);
    std::vector<float> heights(items.size(), 0.0f);

    auto measure = [&]() {
        float total = 0.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            if (IsNewCard(items[i])) {
                widths[i] = FlatCardSide(items[i], cs);
                heights[i] = widths[i];
                total += widths[i];
                continue;
            }
            const float aspect = (items[i].aspect > 0.01f) ? items[i].aspect : 1.4f;
            float photoW = kPhotoMaxWidth * cs;
            float photoH = photoW / aspect;
            if (photoH > kPhotoMaxHeight * cs) {
                photoH = kPhotoMaxHeight * cs;
                photoW = photoH * aspect;
            }
            widths[i] = photoW + 2.0f * kFrame * cs;
            heights[i] = photoH + 2.0f * kFrame * cs;
            total += widths[i];
        }
        total += kGap * cs * static_cast<float>(items.size() - 1);
        return total;
    };

    float total = measure();
    if (total > maxContent) {
        const float factor = rack::Clamp(maxContent / total, 0.45f, 1.0f);
        for (size_t i = 0; i < items.size(); ++i) {
            widths[i] *= factor;
            heights[i] *= factor;
        }
        total = 0.0f;
        for (float width : widths) {
            total += width;
        }
        total += kGap * cs * static_cast<float>(items.size() - 1);
    }

    float gap = kGap * cs;
    if (total > maxContent && items.size() > 1) {
        gap = std::max(10.0f * s, (maxContent - total) / static_cast<float>(items.size() - 1) +
                                     gap);
        total = 0.0f;
        for (float width : widths) {
            total += width;
        }
        total += gap * static_cast<float>(items.size() - 1);
    }

    const float lineY = kLineY * s;
    const float sag = (16.0f + std::min<float>(6.0f, static_cast<float>(items.size())) * 1.2f) * s;
    const float lineSpan = std::max(1.0f, result.width - 2.0f * kOuterPad * s);
    result.lineY = lineY;
    result.sag = sag;
    result.lineStart = kOuterPad * s;
    result.lineEnd = result.width - kOuterPad * s;

    float maxCardHeight = 0.0f;
    for (float height : heights) {
        maxCardHeight = std::max(maxCardHeight, height);
    }
    /* 收起时卡片停在窗口上方，展开时滑进视野。 */
    const float slide =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) * -(maxCardHeight + 30.0f * s);
    float x = kOuterPad * s + (photoWidth - total) * 0.5f;

    /* 显示位置用弹簧跟随目标：目标由布局按索引算出，移除/插入卡片后，
       其它卡片的目标位置整体平移，由弹簧平滑过渡到新位置。 */
    const uint64_t nowMs = rack::NowMs();
    float dt = 0.0f;
    if (lastLayoutMs_ != 0 && nowMs > lastLayoutMs_) {
        dt = std::min(0.05f, static_cast<float>(nowMs - lastLayoutMs_) / 1000.0f);
    }
    lastLayoutMs_ = nowMs;
    sliding_ = false;
    auto slotFor = [&](int id) -> SlotSpring& {
        for (auto& slot : slots_) {
            if (slot.id == id) {
                return slot;
            }
        }
        SlotSpring fresh;
        fresh.id = id;
        slots_.push_back(fresh);
        return slots_.back();
    };

    cards_.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        CardBox box;
        const float w = widths[i];
        const float offset = items[i].rackOffsetX;
        SlotSpring& slot = slotFor(items[i].id);
        if (!slot.primed) {
            slot.pos = x;
            slot.target = x;
            slot.velocity = 0.0f;
            slot.primed = true;
        } else {
            /* 目标位置变了（有卡片被移除/新增）先原地停顿，
               再交给弹簧平滑过渡：短暂停顿后靠拢。 */
            if (std::abs(slot.target - x) > 0.5f) {
                slot.target = x;
                slot.holdUntil = skipHoldOnce_ ? 0 : nowMs + kCloseHoldMs;
            }
            if (nowMs >= slot.holdUntil && dt > 0.0f) {
                SlideSpring(slot.pos, slot.velocity, x, dt, 0.55f, 0.78f);
            }
            if (std::abs(slot.pos - x) > 0.15f || std::abs(slot.velocity) > 0.8f) {
                sliding_ = true;
            }
        }
        const float left = slot.pos + offset;
        box.id = items[i].id;
        /* 绳子是下垂的弧线：卡片上沿跟着它所在位置的实际绳高走，
           这样每个夹子压住卡片上沿的深度始终一样，不会中间夹得深、
           两边悬空。 */
        const float cx = left + w * 0.5f;
        const float t = rack::Clamp((cx - result.lineStart) / std::max(1.0f, lineSpan), 0.0f,
                                    1.0f);
        const float pinTop = lineY + 4.0f * sag * t * (1.0f - t);
        /* 夹身从 pinTop 向下压约 16*clipScale；卡片上沿放在
           pinTop + 12*clipScale，夹子稳定咬住约 4*clipScale。
           夹子和悬挂深度都跟卡片大中小一起缩放。 */
        const float clipScale = s * cardScale_;
        const float cardY = pinTop + 12.0f * clipScale - 3.0f * s + slide;
        box.card = D2D1::RectF(left, cardY, left + w, cardY + heights[i]);
        box.image = rack::Inset(box.card, kFrame * cs, kFrame * cs);
        box.cardType = items[i].cardType;
        if (IsNewCard(items[i])) {
            BuildCardMetrics(items[i], box.card, box);
        }
        const bool hasNote = !items[i].note.empty() && !items[i].path.empty();
        box.hasNote = hasNote;
        if (hasNote) {
            box.note = D2D1::RectF(box.card.left + 4.0f * s,
                                   box.card.bottom + 4.0f * s,
                                   box.card.right - 4.0f * s,
                                   box.card.bottom + 4.0f * s + kNoteHeight * s);
        }
        box.pinTop = pinTop;
        const int variant = (items[i].id * 37) % 8;
        /* 和图片卡一致：每张卡按 id 得到一个固定的小角度，避免一排太规整。 */
        box.angle = static_cast<float>(variant - 4) * 0.6f;
        cards_.push_back(box);
        x += w + gap;
    }

    /* 已经不在绳子上的卡片，弹簧状态也跟着清掉。 */
    for (auto it = slots_.begin(); it != slots_.end();) {
        const bool alive =
            std::any_of(items.begin(), items.end(), [&](const Item& item) { return item.id == it->id; });
        if (alive) {
            ++it;
        } else {
            it = slots_.erase(it);
        }
    }

    float top = lineY - 2.0f * s;
    float bottom = lineY + sag + 136.0f * s;
    float left = 0.0f;
    float right = 0.0f;
    if (!cards_.empty()) {
        bottom = cards_.front().card.bottom;
        left = cards_.front().card.left;
        right = cards_.back().card.right;
    }
    for (const auto& box : cards_) {
        top = std::min(top, box.card.top);
        bottom = std::max(bottom, box.card.bottom);
        left = std::min(left, box.card.left);
        right = std::max(right, box.card.right);
        if (box.hasNote) {
            bottom = std::max(bottom, box.note.bottom);
            right = std::max(right, box.note.right);
        }
    }
    (void)left;
    (void)right;
    bottom = std::max(bottom, lineY + sag + 136.0f * s);
    // 收起状态只保留顶部的热区。保留一个不可见的高窗口，
    // 会把桌面上的点击拦截。
    result.height = expandAmount < 0.3f
                        ? 60.0f * s
                        : std::max(240.0f * s, bottom + 28.0f * s);
    layout_.height = result.height;
    result.content = D2D1::RectF(0.0f, lineY - 2.0f * s, result.width, bottom + 8.0f * s);

    const float accessoriesRight = result.width - kOuterPad * s;
    const float basketCenter = accessoriesRight - 78.0f * s;
    /* 篮子隐藏时信封右移到篮子的槽位；换槽位和卡片一样交给弹簧追，
       不是瞬间跳变。 */
    const float envelopeTarget =
        accessoriesRight - (historyDisabled_ ? 78.0f : 196.0f) * s;
    if (!envelopeXPrimed_) {
        envelopeX_ = envelopeTarget;
        envelopeXVelocity_ = 0.0f;
        envelopeXPrimed_ = true;
    } else if (dt > 0.0f) {
        SlideSpring(envelopeX_, envelopeXVelocity_, envelopeTarget, dt, 0.55f, 0.78f);
    }
    envelopeSliding_ =
        std::abs(envelopeX_ - envelopeTarget) > 0.15f || std::abs(envelopeXVelocity_) > 0.8f;
    const float envelopeCenter = envelopeX_;
    const float accessoryLineSpan = std::max(1.0f, result.lineEnd - result.lineStart);
    auto ropeYAt = [&](float x) {
        const float t = rack::Clamp((x - result.lineStart) / accessoryLineSpan, 0.0f, 1.0f);
        return lineY + 4.0f * sag * t * (1.0f - t);
    };
    if (historyDisabled_) {
        basketRect_ = {};
    } else {
        const float basketTop = ropeYAt(basketCenter) - 14.0f * s;
        basketRect_ = D2D1::RectF(basketCenter - 54.0f * s, basketTop + 24.0f * s,
                                  basketCenter + 54.0f * s, basketTop + 112.0f * s);
    }
    const float envelopeTop = ropeYAt(envelopeCenter) + 8.0f * s;
    const float envelopeWidth = 70.0f * s;
    const float envelopeHeight = envelopeWidth * 376.0f / 340.0f;
    envelopeRect_ = D2D1::RectF(envelopeCenter - envelopeWidth * 0.5f, envelopeTop,
                                envelopeCenter + envelopeWidth * 0.5f,
                                envelopeTop + envelopeHeight + 26.0f * s);
    layout_ = result;
    return result;
}

std::vector<float> RackView::TargetLefts(const std::vector<Item>& items) const
{
    std::vector<float> lefts(items.size(), 0.0f);
    if (items.empty()) {
        return lefts;
    }
    const float s = scale_;
    const float cs = scale_ * cardScale_;
    const float width = std::max(320.0f * s, layout_.width);
    const float photoWidth =
        std::max(120.0f * s, width - (2.0f * kOuterPad + kAccessoryWidth) * s);
    const float maxContent = photoWidth;
    std::vector<float> widths(items.size(), 0.0f);
    auto measure = [&]() {
        float total = 0.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            if (IsNewCard(items[i])) {
                widths[i] = FlatCardSide(items[i], cs);
                total += widths[i];
                continue;
            }
            const float aspect = (items[i].aspect > 0.01f) ? items[i].aspect : 1.4f;
            float photoW = kPhotoMaxWidth * cs;
            float photoH = photoW / aspect;
            if (photoH > kPhotoMaxHeight * cs) {
                photoH = kPhotoMaxHeight * cs;
                photoW = photoH * aspect;
            }
            widths[i] = photoW + 2.0f * kFrame * cs;
            total += widths[i];
        }
        total += kGap * cs * static_cast<float>(items.size() - 1);
        return total;
    };
    float total = measure();
    if (total > maxContent) {
        const float factor = rack::Clamp(maxContent / total, 0.45f, 1.0f);
        for (float& w : widths) {
            w *= factor;
        }
        total = 0.0f;
        for (float w : widths) total += w;
        total += kGap * cs * static_cast<float>(items.size() - 1);
    }
    float gap = kGap * cs;
    if (total > maxContent && items.size() > 1) {
        gap = std::max(10.0f * s,
                       (maxContent - total) / static_cast<float>(items.size() - 1) + gap);
    }
    float x = kOuterPad * s + (photoWidth - total) * 0.5f;
    for (size_t i = 0; i < items.size(); ++i) {
        lefts[i] = x;
        x += widths[i] + gap;
    }
    return lefts;
}

void RackView::SnapSlotsTo(const std::vector<Item>& items,
                           const std::vector<float>& lefts)
{
    if (lefts.size() != items.size()) {
        return;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        SlotSpring* found = nullptr;
        for (auto& slot : slots_) {
            if (slot.id == items[i].id) {
                found = &slot;
                break;
            }
        }
        if (!found) {
            SlotSpring fresh;
            fresh.id = items[i].id;
            slots_.push_back(fresh);
            found = &slots_.back();
        }
        found->pos = lefts[i];
        found->target = lefts[i];
        found->velocity = 0.0f;
        found->holdUntil = 0;
        found->primed = true;
    }
}

void RackView::DrawShadow(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                          float dy, float spread, float alpha)
{
    (void)radius;
    D2D1_GRADIENT_STOP stops[3]{};
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.95f);
    stops[1].position = 0.58f;
    stops[1].color = D2D1::ColorF(0.f, 0.f, 0.f, alpha * 0.42f);
    stops[2].position = 1.0f;
    stops[2].color = D2D1::ColorF(0.f, 0.f, 0.f, 0.0f);

    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> brush;
    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = (rect.top + rect.bottom) * 0.5f + dy;
    const float radiusX = (rect.right - rect.left) * 0.5f + spread;
    const float radiusY = (rect.bottom - rect.top) * 0.5f + spread;
    if (FAILED(target->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2,
                                                    D2D1_EXTEND_MODE_CLAMP,
                                                    collection.GetAddressOf())) ||
        FAILED(target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(centerX, centerY),
                                                D2D1::Point2F(0.f, 0.f), radiusX, radiusY),
            collection.Get(), brush.GetAddressOf()))) {
        return;
    }

    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radiusX, radiusY), brush.Get());
}

void RackView::Draw(ID2D1RenderTarget* target, const std::vector<Item>& items,
                    const Theme& theme, float expandAmount, float swaySeconds,
                    float basketHover, float paper0, float cardHover, int cardHoverIndex,
                    int hovered, int pressed, int historyCount, int clipboardCount)
{
    if (!target || !graphics_) {
        return;
    }
    const float s = scale_;
    const RackLayout& layout = layout_;
    const bool expanded = expandAmount > 0.01f;
    lastHovered_ = hovered;

    /* 卡片开始绘制之前，先移除已完成出场动画的索引。原来在 DrawArrival 里清索引，
       会让新卡片整整一帧不画：drawGroup 因为「仍在入场动画」跳过它，DrawArrival
       又因为「已经落位」跳过它，结果图片/便签刚落到绳上时闪一帧。 */
    if (arrivalIndex_ >= 0 && (arrivalIndex_ >= static_cast<int>(cards_.size()) ||
                               !HasArrival())) {
        arrivalIndex_ = -1;
    }

    /* 不再给 layout.content 铺 1/255 的「命中底」。
       分层窗口的鼠标命中只看 alpha：铺了底之后整块内容区都算窗口，
       卡片之间/绳子下方的空白会把点击全部拦下来，下面的程序点不到。
       可交互区域由 RackWindow::AfterDraw 只给绳子、卡片本体和配件补 alpha。 */

    lineRetract_ =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);

    if (!expanded) {
        return;
    }

    DrawLine(target, theme, s, expandAmount);
    DrawAccessories(target, theme, s, expandAmount, basketHover, paper0, historyCount,
                    clipboardCount);

    /* 摆动用一次轻的角冲量做激励：周期约 0.95 s、阻尼比很低，
       比直接衰减的正弦更慢更软。 */
    auto swayFor = [&](const Item& item) {
        float base = 0.0f;
        if (swaySeconds > 0.0f) {
            constexpr float kResponse = 0.93f;
            constexpr float kDamping = 0.19f;
            constexpr float kImpulse = 9.6f;
            const float natural = 2.0f * 3.14159265f / kResponse;
            const float decay = natural * kDamping;
            const float damped = natural * std::sqrt(1.0f - kDamping * kDamping);
            base = std::exp(-decay * swaySeconds) * (kImpulse / damped) *
                   std::sin(damped * swaySeconds);
        }
        if (item.shakeStartMs != 0 && item.shakeAmp > 0.0f) {
            const float age = static_cast<float>(rack::NowMs() - item.shakeStartMs) / 1000.0f;
            if (age >= 0.0f && age < 1.2f) {
                // 晃动：前 0.30 s 缓出到新角度，之后用弹簧（刚度 38、阻尼 2.4）回落到 0。
                if (age < 0.30f) {
                    base += item.shakeAmp * rack::EaseOutCubic(age / 0.30f);
                } else {
                    const float t = age - 0.30f;
                    constexpr float kStiffness = 38.0f;
                    constexpr float kDamping = 2.4f;
                    const float w = std::sqrt(kStiffness);
                    const float zeta = kDamping / (2.0f * w);
                    const float wd = w * std::sqrt(1.0f - zeta * zeta);
                    base += item.shakeAmp * std::exp(-zeta * w * t) *
                            (std::cos(wd * t) + (zeta / std::sqrt(1.0f - zeta * zeta)) *
                                                      std::sin(wd * t));
                }
            }
        }
        return base;
    };

    /* 图片卡的外阴影不再绘制：阴影是半透明像素，分层窗口的命中测试
       只看 alpha，18px 的阴影会把卡片之间的空隙也占成可点击区域，
       右键点缝隙时下面的程序收不到。卡片本体仍保留描边和圆角。 */

    auto drawGroup = [&](size_t i, bool isHovered) {
        if (static_cast<int>(i) == arrivalIndex_) return;
        const float sway = swayFor(items[i]);
        CardBox box = cards_[i];
        ShiftCard(box, CardEntryOffset(items[i]));

        ComPtr<ID2D1Layer> fadeLayer;
        const bool fade = static_cast<int>(i) == dragging_;
        if (fade) {
            if (SUCCEEDED(target->CreateLayer(fadeLayer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 0.45f),
                    fadeLayer.Get());
            } else {
                fadeLayer.Reset();
            }
        }
        const float hoverAmount =
            static_cast<int>(i) == cardHoverIndex ? cardHover : 0.0f;
        /* 编辑中的便签：卡片本体交给编辑器动画，绳子上只留夹子。 */
        const bool editing = static_cast<int>(i) == editingCard_;
        if (!editing) {
            const bool hotLock = hoverPart_ == static_cast<int>(RackPart::Lock) &&
                                 hoverPartIndex_ == static_cast<int>(i);
            const bool hotClose = hoverPart_ == static_cast<int>(RackPart::Close) &&
                                  hoverPartIndex_ == static_cast<int>(i);
            const bool hotLink = hoverPart_ == static_cast<int>(RackPart::CardLink) &&
                                 hoverPartIndex_ == static_cast<int>(i);
            const bool hotColor = hoverPart_ == static_cast<int>(RackPart::CardColor) &&
                                  hoverPartIndex_ == static_cast<int>(i);
            DrawCard(target, items[i], box, s, sway, hoverAmount, isHovered,
                     static_cast<int>(i) == pressed, hotLock, hotClose, hotLink, hotColor);
        }
        DrawPin(target, box, theme, s);
        if (box.hasNote && !editing) {
            DrawNote(target, items[i], box, theme, s, sway);
        }
        if (fadeLayer) {
            target->PopLayer();
        }
    };

    /* 先画没被悬停的卡片，最后画悬停的那张，让它压在邻居上面；
       夹子在卡片之后画，才像是真的夹住了照片上沿。 */
    for (size_t i = 0; i < cards_.size() && i < items.size(); ++i) {
        if (static_cast<int>(i) == hovered) {
            continue;
        }
        drawGroup(i, false);
    }
    if (hovered >= 0 && hovered < static_cast<int>(cards_.size()) &&
        hovered < static_cast<int>(items.size())) {
        drawGroup(static_cast<size_t>(hovered), true);
    }

    DrawRemoved(target, s);
    if (arrivalIndex_ >= 0 && arrivalIndex_ < static_cast<int>(items.size())) {
        DrawArrival(target, items[static_cast<size_t>(arrivalIndex_)], s);
    }
    DrawCopyToast(target, s);
}

/* 信封当前画在哪儿：弹簧已开始运动则使用弹簧位置，否则按开关算目标。 */
float RackView::EnvelopeCenterX() const
{
    if (envelopeXPrimed_) {
        return envelopeX_;
    }
    return layout_.width - kOuterPad * scale_ - (historyDisabled_ ? 78.0f : 196.0f) * scale_;
}

D2D1_POINT_2F RackView::EnvelopeBadgeCenter() const
{
    const float s = scale_;
    const float span = std::max(1.0f, layout_.lineEnd - layout_.lineStart);
    const float envelopeCenter = EnvelopeCenterX();
    const float t = rack::Clamp((envelopeCenter - layout_.lineStart) / span, 0.0f, 1.0f);
    const float ropeY = layout_.lineY + 4.0f * layout_.sag * t * (1.0f - t);
    const float envelopeBottom = ropeY + 8.0f * s + 70.0f * s * 376.0f / 340.0f;
    float badgeTop = envelopeBottom + 2.0f * s;
    if (!historyDisabled_) {
        const float basketCenter = layout_.width - kOuterPad * s - 78.0f * s;
        const float bt = rack::Clamp((basketCenter - layout_.lineStart) / span, 0.0f, 1.0f);
        const float basketBottom =
            layout_.lineY + 4.0f * layout_.sag * bt * (1.0f - bt) - 14.0f * s + 112.0f * s;
        badgeTop = std::max(envelopeBottom, basketBottom) + 2.0f * s;
    }
    return D2D1::Point2F(envelopeCenter, badgeTop + 18.0f * s);
}

void RackView::BeginArrival(int index)
{
    if (index < 0) return;
    arrivalIndex_ = index;
    arrivalStartMs_ = rack::NowMs();
    arrivalStart_ = D2D1::Point2F(layout_.width * 0.5f, layout_.height * 0.72f);
    arrivalW_ = 0.0f;
    arrivalH_ = 0.0f;
    arrivalStartSet_ = false;
}

void RackView::SetArrivalStart(float x, float y, float w, float h)
{
    arrivalStart_ = D2D1::Point2F(x, y);
    arrivalW_ = w;
    arrivalH_ = h;
    arrivalStartSet_ = true;
}

bool RackView::HasArrival() const
{
    return arrivalIndex_ >= 0 &&
           static_cast<float>(rack::NowMs() - arrivalStartMs_) / 1000.0f < kArrivalSeconds;
}

float RackView::AnimationHeight() const
{
    const float s = scale_;
    float bottom = 0.0f;
    if (HasFalling()) {
        /* 掉落的卡片从绳子上方一直落到绳下约 520 DIP。 */
        bottom = std::max(bottom, layout_.lineY + layout_.sag + 160.0f * s + 560.0f * s);
    }
    if (HasArrival()) {
        /* 文本/便签入场没有 captureH，起始高度和 DrawArrival 一样按目标
           卡高的 1.35 倍算；否则窗口只按 120*s 加高，底部会被裁掉。 */
        float startH = arrivalH_;
        if (startH <= 0.0f && arrivalIndex_ >= 0 &&
            arrivalIndex_ < static_cast<int>(cards_.size())) {
            const CardBox& target = cards_[static_cast<size_t>(arrivalIndex_)];
            startH = (target.card.bottom - target.card.top) * 1.35f;
        }
        const float half = std::max(startH, 120.0f * s) * 0.5f;
        bottom = std::max(bottom, arrivalStart_.y + half + 32.0f * s);
    }
    return bottom;
}

void RackView::DrawArrival(ID2D1RenderTarget* target, const Item& item, float s)
{
    if (arrivalIndex_ < 0 || arrivalIndex_ >= static_cast<int>(cards_.size())) {
        arrivalIndex_ = -1;
        return;
    }
    const float age = static_cast<float>(rack::NowMs() - arrivalStartMs_) / 1000.0f;
    const CardBox& targetBox = cards_[static_cast<size_t>(arrivalIndex_)];
    /* 任何情况下都不能直接返回而不画：Draw() 已经把这卡从常规批次里跳过了，
       这里 return 就会让它闪掉一帧。 */
    const float raw = rack::Clamp(age / kArrivalSeconds, 0.0f, 1.0f);
    const float t = raw < 0.5f ? 4.0f * raw * raw * raw
                               : 1.0f - std::pow(-2.0f * raw + 2.0f, 3.0f) * 0.5f;
    const float targetW = targetBox.card.right - targetBox.card.left;
    const float targetH = targetBox.card.bottom - targetBox.card.top;
    const float startW = arrivalW_ > 0.0f ? arrivalW_ : targetW * 1.35f;
    const float startH = arrivalH_ > 0.0f ? arrivalH_ : targetH * 1.35f;
    const float width = startW + (targetW - startW) * t;
    const float height = startH + (targetH - startH) * t;
    const D2D1_POINT_2F start = arrivalStartSet_
                                    ? arrivalStart_
                                    : D2D1::Point2F(layout_.width * 0.5f,
                                                    layout_.height * 0.72f);
    const float targetCx = (targetBox.card.left + targetBox.card.right) * 0.5f;
    const float targetCy = (targetBox.card.top + targetBox.card.bottom) * 0.5f;
    const float x = start.x + (targetCx - start.x) * t;
    const float y = start.y + (targetCy - start.y) * t -
                    30.0f * s * std::sin(3.14159265f * t);
    CardBox box = targetBox;
    box.card = D2D1::RectF(x - width * 0.5f, y - height * 0.5f,
                           x + width * 0.5f, y + height * 0.5f);
    const float inset = std::min(4.0f * s, kFrame * s * t);
    box.image = rack::Inset(box.card, inset, inset);
    const float spin = targetBox.angle * t;
    DrawCard(target, item, box, s, spin, 0.0f, false, false);
}

bool RackView::ColorContentContains(int index, float x, float y) const
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return false;
    }
    const CardBox& box = cards_[static_cast<size_t>(index)];
    if (!box.flat || box.cardType != card::Type::Color) {
        return false;
    }
    const float u = (box.card.right - box.card.left) / kFlatCardSize;
    const float ring = kFlatRing * u;
    const D2D1_RECT_F white = D2D1::RectF(box.card.left + ring, box.body.top,
                                          box.card.right - ring, box.card.bottom - ring);
    return rack::PointIn(white, x, y);
}

void RackView::PrepareCardRelease(int index)
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return;
    }
    const CardBox& box = cards_[static_cast<size_t>(index)];
    for (auto& slot : slots_) {
        if (slot.id == box.id) {
            slot.pos = box.card.left;
            slot.velocity = 0.0f;
            break;
        }
    }
    /* 重排松手后的第一帧不要 140ms 停顿，直接从落点弹到新槽位。 */
    skipHoldOnce_ = true;
}

void RackView::SetHistoryDisabled(bool disabled)
{
    if (historyDisabled_ == disabled) {
        return;
    }
    historyDisabled_ = disabled;
    if (disabled) {
        basketRect_ = {};
    }
}

void RackView::BeginRemove(int index, const Item& item, bool toBasket)
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return;
    }
    RemovedCard removed;
    removed.item = item;
    removed.box = cards_[static_cast<size_t>(index)];
    removed.removedMs = rack::NowMs();
    /* 篮子藏起来后没有可落的地方，退回成从绳上掉落。 */
    removed.toBasket = toBasket && !historyDisabled_;
    removed_.push_back(std::move(removed));
}

void RackView::DrawRemoved(ID2D1RenderTarget* target, float s)
{
    if (removed_.empty()) {
        return;
    }
    const uint64_t now = rack::NowMs();
    const float basketCenterX = layout_.width - kOuterPad * s - 78.0f * s;
    const float mouthY = basketRect_.top > 0.0f ? basketRect_.top + 8.0f * s
                                                : layout_.lineY + 40.0f * s;
    for (auto it = removed_.begin(); it != removed_.end();) {
        const float age = static_cast<float>(now - it->removedMs) / 1000.0f;
        if (!it->toBasket) {
            // 从绳上掉落：0.55 s、520 像素、缓入三次方，额外加一点旋转和线性淡出，
            // 不做成自由落体那种加速下坠。
            constexpr float kFallSeconds = 0.55f;
            if (age > kFallSeconds) {
                it = removed_.erase(it);
                continue;
            }
            const float raw = rack::Clamp(age / kFallSeconds, 0.0f, 1.0f);
            const float e = raw * raw * raw;
            CardBox box = it->box;
            ShiftCard(box, 520.0f * s * e);
            const float spin = (it->box.angle * 7.0f + 20.0f) * e;
            ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 1.0f - e),
                    layer.Get());
            }
            DrawCard(target, it->item, box, s, spin, 0.0f, false, false);
            if (layer) target->PopLayer();
            ++it;
            continue;
        }
        if (age > 0.64f) {
            it = removed_.erase(it);
            continue;
        }
        const float t = rack::Clamp(age / 0.64f, 0.0f, 1.0f);
        const float ease = t * t * t * (10.0f + t * (-15.0f + 6.0f * t));
        CardBox box = it->box;
        const float originalW = box.card.right - box.card.left;
        const float originalH = box.card.bottom - box.card.top;
        const float smallW = std::min(22.0f * s, std::max(10.0f * s, originalW * 0.15f));
        const float smallH = smallW * originalH / std::max(1.0f, originalW);
        const float startX = (box.card.left + box.card.right) * 0.5f;
        const float startY = (box.card.top + box.card.bottom) * 0.5f;
        const float width = originalW + (smallW - originalW) * ease;
        const float height = originalH + (smallH - originalH) * ease;
        const float x = startX + (basketCenterX - startX) * ease;
        const float lift = std::min(38.0f * s, std::max(10.0f * s, startY - 10.0f * s));
        const float y = startY + (mouthY - startY) * ease -
                        lift * std::sin(3.14159265f * ease);
        box.card = D2D1::RectF(x - width * 0.5f, y - height * 0.5f,
                               x + width * 0.5f, y + height * 0.5f);
        const float inset = std::min(4.0f * s, kFrame * s);
        box.image = rack::Inset(box.card, inset, inset);
        const float spin = -box.angle * ease +
                           (basketCenterX < startX ? -1.0f : 1.0f) * 10.0f *
                               std::sin(3.14159265f * ease);
        const float fade = 1.0f - 0.8f * rack::Clamp((t - 0.82f) / 0.18f, 0.0f, 1.0f);
        ComPtr<ID2D1Layer> layer;
        if (SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
            target->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::IdentityMatrix(), fade),
                layer.Get());
        }
        DrawCard(target, it->item, box, s, spin, 0.0f, false, false);
        if (layer) {
            target->PopLayer();
        }
        ++it;
    }
}

void RackView::DrawLine(ID2D1RenderTarget* target, const Theme& theme, float s,
                        float expandAmount)
{
    const RackLayout& layout = layout_;
    if (layout.lineEnd <= layout.lineStart) {
        return;
    }
    const float retract =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);
    // 各主题下绳子的颜色与线宽。
    D2D1_COLOR_F ropeColor{};
    D2D1_COLOR_F ropeHighlight{};
    float ropeWidth = 1.6f;
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 1:  // 奶白
        ropeColor = D2D1::ColorF(0.68f, 0.69f, 0.69f);
        ropeHighlight = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        ropeWidth = 1.9f;
        break;
    case 2:  // 木棉
        ropeColor = D2D1::ColorF(0.73f, 0.65f, 0.54f);
        ropeHighlight = D2D1::ColorF(0.92f, 0.87f, 0.79f);
        ropeWidth = 2.3f;
        break;
    case 3:  // 深沉
        ropeColor = D2D1::ColorF(0.34f, 0.36f, 0.39f);
        ropeHighlight = D2D1::ColorF(0.58f, 0.61f, 0.64f);
        ropeWidth = 1.65f;
        break;
    default:  // 银灰
        ropeColor = D2D1::ColorF(0.54f, 0.59f, 0.65f);
        ropeHighlight = D2D1::ColorF(0.78f, 0.81f, 0.85f);
        ropeWidth = 1.6f;
        break;
    }
    const float y = layout.lineY + retract;
    const float sag = layout.sag;

    /* 绳子首尾各 8% 做淡出，看起来像从屏幕外延伸过来，
       而不是两个硬邦邦的断点。 */
    auto makeFadeBrush = [&](const D2D1_COLOR_F& color,
                             ComPtr<ID2D1LinearGradientBrush>& out) -> bool {
        D2D1_GRADIENT_STOP stops[4]{};
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(color.r, color.g, color.b, 0.0f);
        stops[1].position = 0.08f;
        stops[1].color = color;
        stops[2].position = 0.92f;
        stops[2].color = color;
        stops[3].position = 1.0f;
        stops[3].color = D2D1::ColorF(color.r, color.g, color.b, 0.0f);
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(target->CreateGradientStopCollection(
                stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                collection.GetAddressOf()))) {
            return false;
        }
        return SUCCEEDED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(layout.lineStart, y), D2D1::Point2F(layout.lineEnd, y)),
            collection.Get(), out.GetAddressOf()));
    };

    ComPtr<ID2D1LinearGradientBrush> shadow;
    ComPtr<ID2D1LinearGradientBrush> line;
    ComPtr<ID2D1LinearGradientBrush> highlight;
    if (!makeFadeBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.22f), shadow) ||
        !makeFadeBrush(ropeColor, line) ||
        !makeFadeBrush(ropeHighlight, highlight)) {
        return;
    }
    const float midX = (layout.lineStart + layout.lineEnd) * 0.5f;
    auto buildCurve = [&](float dy, ComPtr<ID2D1PathGeometry>& path) -> bool {
        if (FAILED(graphics_->d2d()->CreatePathGeometry(path.GetAddressOf()))) {
            return false;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(sink.GetAddressOf()))) {
            return false;
        }
        const D2D1_POINT_2F start = D2D1::Point2F(layout.lineStart, y + dy);
        const D2D1_POINT_2F end = D2D1::Point2F(layout.lineEnd, y + dy);
        const D2D1_POINT_2F control = D2D1::Point2F(midX, y + sag * 2.0f + dy);
        const D2D1_POINT_2F c1 = D2D1::Point2F(start.x + (control.x - start.x) * 2.0f / 3.0f,
                                              start.y + (control.y - start.y) * 2.0f / 3.0f);
        const D2D1_POINT_2F c2 = D2D1::Point2F(end.x + (control.x - end.x) * 2.0f / 3.0f,
                                              end.y + (control.y - end.y) * 2.0f / 3.0f);
        sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(c1, c2, end));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        return true;
    };

    ComPtr<ID2D1PathGeometry> shadowPath;
    if (buildCurve(1.0f * s, shadowPath)) {
        target->DrawGeometry(shadowPath.Get(), shadow.Get(), 2.2f * s);
    }
    ComPtr<ID2D1PathGeometry> linePath;
    if (buildCurve(0.0f, linePath)) {
        target->DrawGeometry(linePath.Get(), line.Get(), ropeWidth * s);
    }
    ComPtr<ID2D1PathGeometry> highlightPath;
    if (buildCurve(-0.35f * s, highlightPath)) {
        target->DrawGeometry(highlightPath.Get(), highlight.Get(),
                             std::max(0.4f, ropeWidth * 0.26f) * s);
    }
}

void RackView::DrawPin(ID2D1RenderTarget* target, const CardBox& box, const Theme& theme,
                       float s)
{
    const float cx = (box.card.left + box.card.right) * 0.5f;
    /* 夹子跟卡片大中小一起缩放；槽仍在绳子上，夹身压住卡片上沿。 */
    const float clipScale = s * cardScale_;
    const float clipHeight = 24.0f * clipScale;
    DrawClipAt(target, cx, box.pinTop + lineRetract_ - clipHeight * kClipSlotRatio,
               theme, clipScale);
}

void RackView::DrawClipAt(ID2D1RenderTarget* target, float cx, float top, const Theme& theme,
                          float s, float heightDips)
{
    /* 夹子是整块圆角金属片（9x26），不是两瓣的晾衣夹：
       轮廓简单，做一道柔和的高光即可。 */
    const float width = 8.0f * s;
    const float height = heightDips * s;
    const D2D1_RECT_F rect =
        D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);

    // 夹子用主题的金属色，而不是固定一套钢色。
    D2D1_COLOR_F peg[3]{};
    D2D1_COLOR_F pegEdge{};
    switch (rack::ClampInt(theme.index, 0, 3)) {
    case 3:  // 深沉，石墨灰 #575E66
        peg[0] = D2D1::ColorF(0.43f, 0.46f, 0.49f);
        peg[1] = D2D1::ColorF(0.34f, 0.37f, 0.40f);
        peg[2] = D2D1::ColorF(0.27f, 0.30f, 0.32f);
        pegEdge = D2D1::ColorF(0.26f, 0.29f, 0.31f);
        break;
    case 2:  // 木棉，亚麻米 #DFC5A3（浅榉木）
        peg[0] = D2D1::ColorF(0.92f, 0.84f, 0.74f);
        peg[1] = D2D1::ColorF(0.87f, 0.77f, 0.64f);
        peg[2] = D2D1::ColorF(0.80f, 0.69f, 0.54f);
        pegEdge = D2D1::ColorF(0.68f, 0.56f, 0.41f);
        break;
    case 1:  // 奶白，瓷白 #F3F0E8
        peg[0] = D2D1::ColorF(0.98f, 0.97f, 0.95f);
        peg[1] = D2D1::ColorF(0.95f, 0.94f, 0.91f);
        peg[2] = D2D1::ColorF(0.91f, 0.89f, 0.85f);
        pegEdge = D2D1::ColorF(0.76f, 0.75f, 0.71f);
        break;
    default:  // 银灰，经典银 #DCE3EC
        peg[0] = D2D1::ColorF(0.90f, 0.92f, 0.95f);
        peg[1] = D2D1::ColorF(0.86f, 0.89f, 0.93f);
        peg[2] = D2D1::ColorF(0.76f, 0.80f, 0.85f);
        pegEdge = D2D1::ColorF(0.54f, 0.60f, 0.67f);
        break;
    }
    D2D1_GRADIENT_STOP stops[3]{};
    stops[0].position = 0.0f;
    stops[0].color = peg[0];
    stops[1].position = 0.45f;
    stops[1].color = peg[1];
    stops[2].position = 1.0f;
    stops[2].color = peg[2];

    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    if (FAILED(target->CreateGradientStopCollection(
            stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            collection.GetAddressOf())) ||
        FAILED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(rect.left, rect.top),
                D2D1::Point2F(rect.right, rect.top)),
            collection.Get(), fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(pegEdge, edge.GetAddressOf()))) {
        return;
    }

    const bool wood = rack::ClampInt(theme.index, 0, 3) == 2;
    if (wood) {
        // 木夹子的后夹片，稍微偏右一点。
        ComPtr<ID2D1SolidColorBrush> rear;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                    D2D1::ColorF(0.78f, 0.66f, 0.51f), rear.GetAddressOf()))) {
            D2D1_RECT_F back = rect;
            back.left += 1.6f * s;
            back.right += 1.6f * s;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(back, 2.4f * s, 2.4f * s), rear.Get());
        }
    }
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect, wood ? 2.4f * s : 3.0f * s,
                          wood ? 2.4f * s : 3.0f * s),
        fill.Get());
    target->DrawRoundedRectangle(
        D2D1::RoundedRect(rect, wood ? 2.4f * s : 3.0f * s,
                          wood ? 2.4f * s : 3.0f * s),
        edge.Get(), wood ? 0.7f * s : 0.6f * s);
    if (wood) {
        ComPtr<ID2D1SolidColorBrush> grain;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(0.54f, 0.39f, 0.22f, 0.26f), grain.GetAddressOf()))) {
            // 一道很淡的木纹，不要画成条纹木纹。
            const float gx = rect.left + 3.0f * s;
            target->DrawLine(D2D1::Point2F(gx, rect.top + 4.0f * s),
                             D2D1::Point2F(gx + 0.4f * s, rect.bottom - 5.0f * s),
                             grain.Get(), 0.45f * s);
            const float slotY = top + height * kClipSlotRatio;
            target->DrawLine(D2D1::Point2F(rect.left + 1.0f * s, slotY),
                             D2D1::Point2F(rect.right - 1.0f * s, slotY),
                             grain.Get(), 0.55f * s);
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(rect.right - 0.5f * s,
                                            rect.top + height * 0.52f),
                              1.4f * s, 1.6f * s),
                grain.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> highlight;
    if (!wood && SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(1.f, 1.f, 1.f, theme.dark ? 0.18f : 0.50f),
            highlight.GetAddressOf()))) {
        target->DrawLine(D2D1::Point2F(rect.left + 2.0f * s, rect.top + 3.0f * s),
                         D2D1::Point2F(rect.left + 2.0f * s, rect.bottom - 3.0f * s),
                         highlight.Get(), 0.7f * s);
    }

    /* 中间那道槽当作夹子的弹簧轴。 */
    ComPtr<ID2D1SolidColorBrush> slot;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(edge->GetColor().r, edge->GetColor().g, edge->GetColor().b, 0.72f),
            slot.GetAddressOf()))) {
        const float slotY = top + height * kClipSlotRatio;
        target->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(cx - 2.5f * s, slotY - 0.7f * s, cx + 2.5f * s,
                            slotY + 0.7f * s),
                0.7f * s, 0.7f * s),
            slot.Get());
    }
}

void RackView::DrawAccessories(ID2D1RenderTarget* target, const Theme& theme, float s,
                               float expandAmount, float basketHover, float paper0,
                               int historyCount, int clipboardCount)
{
    const RackLayout& layout = layout_;
    const float lineSpan = std::max(1.0f, layout.lineEnd - layout.lineStart);
    const float retract =
        (1.0f - rack::Clamp(expandAmount, 0.0f, 1.0f)) *
        -(layout.lineY + layout.sag + 24.0f * s);
    auto ropeYAt = [&](float x) {
        const float t = rack::Clamp((x - layout.lineStart) / lineSpan, 0.0f, 1.0f);
        return layout.lineY + retract + 4.0f * layout.sag * t * (1.0f - t);
    };

    const float accessoriesRight = layout.width - kOuterPad * s;
    const float basketCenter = accessoriesRight - 78.0f * s;
    const float envelopeCenter = EnvelopeCenterX();
    /* 两个数量角标共用同一条基线，即使信封和篮子的高度不同。 */
    const float envelopeBottom =
        ropeYAt(envelopeCenter) + 8.0f * s + 70.0f * s * 376.0f / 340.0f;
    float badgeTop = envelopeBottom + 2.0f * s;
    if (!historyDisabled_) {
        const float basketBottom = ropeYAt(basketCenter) - 14.0f * s + 112.0f * s;
        badgeTop = std::max(envelopeBottom, basketBottom) + 2.0f * s;
    }

    const int themeIndex = rack::ClampInt(theme.index, 0, 3);

    /* 篮子先画：从「停用历史」恢复时它就在信封现在的位置，让信封从它上面
       滑开、把篮子露出来，而不是两个配件硬叠在一起。平时两者不重叠。 */
    const ImageBitmap* basket = basketTheme_[themeIndex].valid()
                                   ? &basketTheme_[themeIndex]
                                   : &basket_;
    if (!historyDisabled_ && basket && basket->valid()) {
        /* 弹簧值要保留符号：离开时它会越过 0 后回摆，
           篮子才像自然停住。 */
        const float basketHoverAmount = basketHover;
        const float basketSize = 112.0f * s;
        const float themeDrop = themeIndex == 1 ? 2.0f * s : 0.0f;
        const float top = ropeYAt(basketCenter) - 14.0f * s + themeDrop;
        D2D1_RECT_F dest = D2D1::RectF(basketCenter - basketSize * 0.5f, top,
                                       basketCenter + basketSize * 0.5f, top + basketSize);
        const D2D1_POINT_2F hook = D2D1::Point2F(basketCenter, ropeYAt(basketCenter));
        target->SetTransform(
            D2D1::Matrix3x2F::Rotation(-2.0f * basketHoverAmount, hook));
        target->DrawBitmap(basket->bitmap.Get(), dest, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        DrawCountBadge(target, basketCenter, badgeTop, historyCount, s);
    }

    const ImageBitmap* envelopeBack = envelopeThemeBack_[themeIndex].valid()
                                          ? &envelopeThemeBack_[themeIndex]
                                          : &envelope_;
    const ImageBitmap* envelopeFront = envelopeThemeFront_[themeIndex].valid()
                                           ? &envelopeThemeFront_[themeIndex]
                                           : &envelopeFront_;
    if (envelopeBack && envelopeBack->valid()) {
        const float envelopeWidth = 70.0f * s;
        const float envelopeHeight = envelopeWidth * 376.0f / 340.0f;
        const float top = ropeYAt(envelopeCenter) + 8.0f * s;
        const D2D1_RECT_F dest =
            D2D1::RectF(envelopeCenter - envelopeWidth * 0.5f, top,
                        envelopeCenter + envelopeWidth * 0.5f, top + envelopeHeight);

        /* 图层顺序（由下往上）：
           背面 → 夹子 → 信纸 → 正面。带主题的背面本身已经包含纸张，
           所以单独的「信纸」图层只给旧版素材兜底。 */
        target->DrawBitmap(envelopeBack->bitmap.Get(), dest, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        const float envelopeClipScale = s * cardScale_;
        DrawClipAt(target, envelopeCenter,
                   ropeYAt(envelopeCenter) -
                       24.0f * envelopeClipScale * kClipSlotRatio,
                   theme, envelopeClipScale);

        // 所有主题共用同一张信纸，换的只是它下面的信封配色。
        if (letter_.valid()) {
            const float amount = rack::Clamp(paper0, 0.0f, 1.05f);
            const float paperTop = top + (0.18f - 0.18f * amount) * envelopeHeight;
            const float paperWidth = envelopeWidth * (65.0f / 85.0f);
            const float paperHeight = paperWidth * 312.0f / 288.0f;
            const D2D1_RECT_F paper = D2D1::RectF(
                envelopeCenter - paperWidth * 0.5f, paperTop,
                envelopeCenter + paperWidth * 0.5f, paperTop + paperHeight);
            target->DrawBitmap(letter_.bitmap.Get(), paper, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        if (envelopeFront && envelopeFront->valid()) {
            target->DrawBitmap(envelopeFront->bitmap.Get(), dest, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        DrawCountBadge(target, envelopeCenter, badgeTop, clipboardCount, s);
    }
}

void RackView::DrawCountBadge(ID2D1RenderTarget* target, float centerX, float top, int count,
                              float s)
{
    if (!badgeFormat_) {
        return;
    }
    /* 三位数以上要显示成 999+，胶囊跟着文字加宽；两位数以内保持原来的尺寸。 */
    wchar_t label[8]{};
    if (count > 999) {
        wcscpy_s(label, L"999+");
    } else {
        swprintf_s(label, L"%d", count);
    }

    float textWidth = 0.0f;
    if (graphics_ && graphics_->dwrite()) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                label, static_cast<UINT32>(wcslen(label)), badgeFormat_.Get(), 120.0f * s,
                18.0f * s, layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textWidth = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }
    const float width = std::max(28.0f * s, textWidth + 10.0f * s);
    const float height = 18.0f * s;
    D2D1_RECT_F rect =
        D2D1::RectF(centerX - width * 0.5f, top, centerX + width * 0.5f, top + height);

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> text;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.15f, 0.82f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.99f),
                                             text.GetAddressOf()))) {
        return;
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 9.0f * s, 9.0f * s), fill.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 9.0f * s, 9.0f * s), border.Get(),
                                 0.8f * s);

    target->DrawText(label, static_cast<UINT32>(wcslen(label)), badgeFormat_.Get(), rect,
                     text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RackView::ShowColorFlash(int itemId, int colorIndex, int part, uint64_t untilMs)
{
    copyFlashItemId_ = itemId;
    copyFlashColorIndex_ = colorIndex;
    copyFlashPart_ = part;
    copyFlashUntil_ = untilMs;
}

void RackView::ClearColorFlash()
{
    copyFlashItemId_ = -1;
    copyFlashColorIndex_ = -1;
    copyFlashPart_ = 0;
    copyFlashUntil_ = 0;
}

void RackView::ShowCopyToast(int index, uint64_t untilMs)
{
    copyToastIndex_ = index;
    copyToastUntil_ = untilMs;
}

void RackView::ClearCopyToast()
{
    copyToastIndex_ = -1;
    copyToastUntil_ = 0;
}

void RackView::DrawCopyToast(ID2D1RenderTarget* target, float s)
{
    if (copyToastIndex_ < 0 || rack::NowMs() >= copyToastUntil_ ||
        copyToastIndex_ >= static_cast<int>(cards_.size()) || !toastFormat_) {
        return;
    }
    const CardBox& box = cards_[copyToastIndex_];
    const float width = 70.0f * s;
    const float height = 22.0f * s;
    const float centerX = (box.card.left + box.card.right) * 0.5f;
    const D2D1_RECT_F rect = D2D1::RectF(centerX - width * 0.5f,
                                         box.card.bottom + 10.0f * s,
                                         centerX + width * 0.5f,
                                         box.card.bottom + 10.0f * s + height);
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> text;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.15f, 0.82f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.99f),
                                             text.GetAddressOf()))) {
        return;
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, 11.0f * s, 11.0f * s),
                                 fill.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(rect, 11.0f * s, 11.0f * s),
                                 border.Get(), 0.8f * s);
    target->DrawText(L"复制成功", 4, toastFormat_.Get(), rect, text.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RackView::DrawTypeBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card, int kind,
                             float s)
{
    /* 只画图标本身，不加底托，避免看起来像一个可以点的按钮。 */
    const float size = 18.0f * s;
    const D2D1_RECT_F area = D2D1::RectF(card.left + 7.0f * s,
                                         card.bottom - 7.0f * s - size,
                                         card.left + 7.0f * s + size,
                                         card.bottom - 7.0f * s);
    ComPtr<ID2D1SolidColorBrush> ink;
    ComPtr<ID2D1SolidColorBrush> outline;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                                             ink.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f),
                                             outline.GetAddressOf()))) {
        return;
    }
    const float edge = 0.9f * s;
    if (kind == 2) {
        /* 摄像机：机身 + 右侧镜头三角。 */
        const float bx = area.left + 3.5f * s;
        const float by = area.top + 6.2f * s;
        const float bw = 7.5f * s;
        const float bh = 5.6f * s;
        const D2D1_ROUNDED_RECT body =
            D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), 1.2f * s, 1.2f * s);
        target->FillRoundedRectangle(body, ink.Get());
        target->DrawRoundedRectangle(body, outline.Get(), edge);
        ComPtr<ID2D1PathGeometry> lens;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(lens.GetAddressOf())) && lens) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(lens->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(D2D1::Point2F(bx + bw + 0.8f * s, by + 1.6f * s),
                                  D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(bx + bw + 4.2f * s, by - 0.4f * s));
                sink->AddLine(D2D1::Point2F(bx + bw + 4.2f * s, by + bh + 0.4f * s));
                sink->AddLine(D2D1::Point2F(bx + bw + 0.8f * s, by + bh - 1.6f * s));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->FillGeometry(lens.Get(), ink.Get());
                target->DrawGeometry(lens.Get(), outline.Get(), edge);
            }
        }
    } else {
        /* 文件夹：主体 + 左上角标签。 */
        const float bx = area.left + 3.5f * s;
        const float by = area.top + 5.6f * s;
        const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
            D2D1::RectF(bx, by + 1.6f * s, bx + 11.0f * s, by + 8.4f * s), 1.2f * s,
            1.2f * s);
        const D2D1_ROUNDED_RECT tab = D2D1::RoundedRect(
            D2D1::RectF(bx, by, bx + 4.6f * s, by + 3.0f * s), 1.2f * s, 1.2f * s);
        target->FillRoundedRectangle(body, ink.Get());
        target->DrawRoundedRectangle(body, outline.Get(), edge);
        target->FillRoundedRectangle(tab, ink.Get());
        target->DrawRoundedRectangle(tab, outline.Get(), edge);
    }
}

void RackView::DrawFileBadge(ID2D1RenderTarget* target, const D2D1_RECT_F& card,
                             const Item& item, float s)
{
    const float iconSize = 13.0f * s;
    const D2D1_RECT_F icon = D2D1::RectF(card.left + 9.0f * s, card.bottom - 9.0f * s - iconSize,
                                         card.left + 9.0f * s + iconSize,
                                         card.bottom - 9.0f * s);
    ComPtr<ID2D1SolidColorBrush> iconInk;
    ComPtr<ID2D1SolidColorBrush> textInk;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f),
                                             iconInk.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.68f),
                                             textInk.GetAddressOf()))) {
        return;
    }
    /* 文件夹用字体图标（和剪切板面板同一个字形），比原来的角标大一档。 */
    if (iconFolder_) {
        DrawIcon(target, iconFolder_.Get(),
                 D2D1::Point2F((icon.left + icon.right) * 0.5f,
                               (icon.top + icon.bottom) * 0.5f),
                 iconSize, iconInk.Get());
    }

    if (!fileTagFormat_) {
        return;
    }
    const int count = FileCountFromLabel(item.label);
    std::wstring text;
    if (count > 1) {
        text = std::to_wstring(count) + L" 个文件";
    } else {
        text = AppNameForExtension(std::filesystem::path(item.path).extension().wstring());
    }
    if (text.empty()) {
        return;
    }
    const float textLeft = icon.right + 5.0f * s;
    const float textRight = card.right - 9.0f * s;
    if (textRight <= textLeft) {
        return;
    }
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fileTagFormat_.Get(),
                     D2D1::RectF(textLeft, icon.top, textRight, icon.bottom), textInk.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void RackView::DrawCardShadow(ID2D1RenderTarget* target, const CardBox& box, float s,
                              bool hovered)
{
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    target->SetTransform(D2D1::Matrix3x2F::Rotation(box.angle, pivot));
    DrawShadow(target, box.card, 12.0f * s, 5.0f * s, hovered ? 20.0f * s : 18.0f * s,
               hovered ? 0.16f : 0.12f);
    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RackView::DrawFlatIcon(ID2D1RenderTarget* target, card::Type type,
                            D2D1_POINT_2F center, float size, ID2D1Brush* brush, float stroke)
{
    if (!brush || size <= 0.0f) return;
    /* 优先用 iconfont.ttf（font_5228143 / SnowAirIcons）里的字形。 */
    ID2D1PathGeometry* glyph = nullptr;
    switch (type) {
    case card::Type::Text: glyph = iconText_.Get(); break;
    case card::Type::Link: glyph = iconLink_.Get(); break;
    case card::Type::File: glyph = iconFolder_.Get(); break;
    case card::Type::Video: glyph = iconVideo_.Get(); break;
    case card::Type::Music: glyph = iconMusic_.Get(); break;
    /* 字体里的 wuse 是外观设置同款方块；颜色卡用参考稿的三圆，不走字体。 */
    case card::Type::Color: glyph = nullptr; break;
    default: break;
    }
    if (glyph) {
        DrawIcon(target, glyph, center, size, brush);
        return;
    }
    switch (type) {
    case card::Type::Text: {
        const D2D1_RECT_F frame = D2D1::RectF(center.x - size * 0.42f, center.y - size * 0.42f,
                                              center.x + size * 0.42f, center.y + size * 0.42f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(frame, size * 0.12f, size * 0.12f),
                                     brush, stroke);
        for (int i = -1; i <= 1; ++i) {
            const float y = center.y + static_cast<float>(i) * size * 0.17f;
            target->DrawLine(D2D1::Point2F(center.x - size * 0.22f, y),
                             D2D1::Point2F(center.x + size * 0.22f, y), brush, stroke);
        }
        break;
    }
    case card::Type::Link: {
        D2D1_MATRIX_3X2_F saved{};
        target->GetTransform(&saved);
        for (int i = 0; i < 2; ++i) {
            const float angle = i == 0 ? 45.0f : -45.0f;
            const float shift = i == 0 ? -size * 0.10f : size * 0.10f;
            target->SetTransform(D2D1::Matrix3x2F::Translation(shift, 0.0f) *
                                 D2D1::Matrix3x2F::Rotation(angle, center) * saved);
            const D2D1_RECT_F capsule = D2D1::RectF(
                center.x - size * 0.30f, center.y - size * 0.12f,
                center.x + size * 0.12f, center.y + size * 0.12f);
            target->DrawRoundedRectangle(
                D2D1::RoundedRect(capsule, size * 0.12f, size * 0.12f), brush, stroke);
        }
        target->SetTransform(saved);
        break;
    }
    case card::Type::File: {
        const float left = center.x - size * 0.40f;
        const float top = center.y - size * 0.30f;
        const D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
            D2D1::RectF(left, top + size * 0.14f, left + size * 0.80f, top + size * 0.62f),
            size * 0.08f, size * 0.08f);
        const D2D1_ROUNDED_RECT tab = D2D1::RoundedRect(
            D2D1::RectF(left, top, left + size * 0.34f, top + size * 0.24f), size * 0.07f,
            size * 0.07f);
        target->DrawRoundedRectangle(body, brush, stroke);
        target->DrawRoundedRectangle(tab, brush, stroke);
        break;
    }
    case card::Type::Video: {
        const D2D1_RECT_F screen = D2D1::RectF(
            center.x - size * 0.42f, center.y - size * 0.32f,
            center.x + size * 0.42f, center.y + size * 0.22f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(screen, size * 0.10f, size * 0.10f),
                                     brush, stroke);
        ComPtr<ID2D1PathGeometry> play;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(play.GetAddressOf())) && play) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(play->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(D2D1::Point2F(center.x - size * 0.07f, center.y - size * 0.14f),
                                  D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(center.x + size * 0.12f, center.y - size * 0.05f));
                sink->AddLine(D2D1::Point2F(center.x - size * 0.07f, center.y + size * 0.04f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->FillGeometry(play.Get(), brush);
            }
        }
        target->DrawLine(D2D1::Point2F(center.x, center.y + size * 0.22f),
                         D2D1::Point2F(center.x, center.y + size * 0.32f), brush, stroke);
        target->DrawLine(D2D1::Point2F(center.x - size * 0.16f, center.y + size * 0.32f),
                         D2D1::Point2F(center.x + size * 0.16f, center.y + size * 0.32f), brush,
                         stroke);
        break;
    }
    case card::Type::Music: {
        const float headR = size * 0.15f;
        const D2D1_POINT_2F leftHead =
            D2D1::Point2F(center.x - size * 0.18f, center.y + size * 0.20f);
        const D2D1_POINT_2F rightHead =
            D2D1::Point2F(center.x + size * 0.22f, center.y + size * 0.08f);
        target->FillEllipse(D2D1::Ellipse(leftHead, headR, headR * 0.80f), brush);
        target->FillEllipse(D2D1::Ellipse(rightHead, headR, headR * 0.80f), brush);
        target->DrawLine(D2D1::Point2F(leftHead.x + headR * 0.9f, leftHead.y),
                         D2D1::Point2F(leftHead.x + headR * 0.9f, center.y - size * 0.30f),
                         brush, stroke);
        target->DrawLine(D2D1::Point2F(rightHead.x + headR * 0.9f, rightHead.y),
                         D2D1::Point2F(rightHead.x + headR * 0.9f, center.y - size * 0.36f),
                         brush, stroke);
        target->DrawLine(D2D1::Point2F(leftHead.x + headR * 0.9f, center.y - size * 0.30f),
                         D2D1::Point2F(rightHead.x + headR * 0.9f, center.y - size * 0.36f),
                         brush, stroke);
        target->DrawLine(D2D1::Point2F(leftHead.x + headR * 0.9f, center.y - size * 0.22f),
                         D2D1::Point2F(rightHead.x + headR * 0.9f, center.y - size * 0.28f),
                         brush, stroke);
        break;
    }
    case card::Type::Color: {
        const D2D1_POINT_2F top = D2D1::Point2F(center.x, center.y - size * 0.20f);
        const D2D1_POINT_2F left =
            D2D1::Point2F(center.x - size * 0.21f, center.y + size * 0.16f);
        const D2D1_POINT_2F right =
            D2D1::Point2F(center.x + size * 0.21f, center.y + size * 0.16f);
        const float r = size * 0.20f;
        target->DrawEllipse(D2D1::Ellipse(top, r, r), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(left, r, r), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(right, r, r), brush, stroke);
        break;
    }
    default: break;
    }
}

void RackView::DrawFlatCard(ID2D1RenderTarget* target, const Item& item,
                            const D2D1_RECT_F& rect, float s, bool hotLink, bool hotColor)
{
    (void)s;
    if (!cardTitleFormat_ || !cardBodyFormat_) return;
    const float u = (rect.right - rect.left) / kFlatCardSize;
    const float radius = kFlatRadius * u;
    const float ring = kFlatRing * u;
    const D2D1_COLOR_F accent = AccentFor(item);
    const bool darkInk = AccentUsesDarkInk(accent);
    const D2D1_COLOR_F headerInk =
        darkInk ? D2D1::ColorF(0.318f, 0.318f, 0.318f) : D2D1::ColorF(1.0f, 1.0f, 1.0f);
    const D2D1_RECT_F header =
        D2D1::RectF(rect.left, rect.top, rect.right, rect.top + kFlatHeader * u);
    const D2D1_RECT_F body =
        D2D1::RectF(rect.left, header.bottom, rect.right, rect.bottom);

    ComPtr<ID2D1SolidColorBrush> white;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    ComPtr<ID2D1SolidColorBrush> ringBrush;
    ComPtr<ID2D1SolidColorBrush> headerBrush;
    ComPtr<ID2D1SolidColorBrush> metaBrush;
    ComPtr<ID2D1SolidColorBrush> bodyBrush;
    ComPtr<ID2D1SolidColorBrush> mutedBrush;
    ComPtr<ID2D1SolidColorBrush> swatchBorder;
    ComPtr<ID2D1SolidColorBrush> softEdge;
    ComPtr<ID2D1SolidColorBrush> flashGreen;
    const D2D1_COLOR_F ringColor =
        MixColor(accent, D2D1::ColorF(1.0f, 1.0f, 1.0f), 0.80f);
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                             white.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(accent, accentBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(ringColor, ringBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(ringColor.r, ringColor.g, ringColor.b, 0.075f),
            softEdge.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(headerInk, headerBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(
            D2D1::ColorF(headerInk.r, headerInk.g, headerInk.b, 0.60f),
            metaBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.318f, 0.318f, 0.318f),
                                             bodyBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.45f, 0.45f, 0.45f),
                                             mutedBrush.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
                                             swatchBorder.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0x34C759),
                                             flashGreen.GetAddressOf()))) {
        return;
    }

    /* 外圈用不透明的「20% 类型色 + 80% 白」预合成色，白卡再内缩一圈。
       这样最外沿的 AA 像素是类型色而不是白色，深色桌面上不会出现
       「GIF 透明图」那种白色毛边。外沿再加 1u 很淡的同色柔光，
       让圆角在深色背景上不至于像刀切一样锐。 */
    for (int layer = 0; layer < 3; ++layer) {
        const float grow = (3.0f - static_cast<float>(layer)) * 0.8f * u;
        const D2D1_RECT_F softRect = rack::Inset(rect, -grow, -grow);
        target->FillRoundedRectangle(
            D2D1::RoundedRect(softRect, radius + grow, radius + grow), softEdge.Get());
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), ringBrush.Get());
    const D2D1_RECT_F whiteCard = rack::Inset(rect, ring, ring);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(whiteCard, std::max(1.0f, radius - ring),
                          std::max(1.0f, radius - ring)),
        white.Get());
    target->FillRoundedRectangle(D2D1::RoundedRect(header, radius, radius),
                                 accentBrush.Get());
    target->FillRectangle(
        D2D1::RectF(header.left, header.top + radius, header.right, header.bottom),
        accentBrush.Get());

    const int count = CardCountFor(item);
    std::wstring title = card::TypeLabel(item.cardType);
    if (item.cardType == card::Type::File) {
        std::vector<std::wstring> paths = item.paths;
        if (paths.empty() && !item.path.empty()) {
            paths.push_back(item.path);
        }
        title = card::FileLabel(paths);
    }
    if (count > 1) title += L"（" + std::to_wstring(count) + L"）";
    target->DrawText(title.c_str(), static_cast<UINT32>(title.size()), cardTitleFormat_.Get(),
                     D2D1::RectF(header.left + 11.0f * u, header.top + 4.0f * u,
                                 header.right - 32.0f * u, header.top + 19.0f * u),
                     headerBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    std::wstring meta = card::FormatCardTime(item.createdAt);
    if ((item.cardType == card::Type::File || item.cardType == card::Type::Music ||
         item.cardType == card::Type::Video) &&
        !item.path.empty()) {
        const std::wstring app =
            AppNameForExtension(std::filesystem::path(item.path).extension().wstring());
        if (!app.empty()) {
            if (!meta.empty()) meta += L" ";
            meta += app;
        }
    }
    if (!meta.empty()) {
        target->DrawText(meta.c_str(), static_cast<UINT32>(meta.size()), cardMetaFormat_.Get(),
                         D2D1::RectF(header.left + 11.0f * u, header.top + 19.0f * u,
                                     header.right - 32.0f * u, header.top + 31.0f * u),
                         metaBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    DrawFlatIcon(target, item.cardType,
                 D2D1::Point2F(header.right - 16.0f * u, header.top + 16.5f * u), 13.0f * u,
                 headerBrush.Get(), 1.2f * u);

    target->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (item.cardType == card::Type::Color) {
        std::vector<card::Rgba> colors;
        if (card::ParseColors(item.note, colors) && !colors.empty()) {
            ComPtr<ID2D1SolidColorBrush> swatch;
            if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0),
                                                        swatch.GetAddressOf()))) {
                for (size_t i = 0; i < colors.size(); ++i) {
                    const float rowTop = body.top + 4.0f * u -
                                         item.colorScroll * u +
                                         static_cast<float>(i) * kColorRowHeight * u;
                    if (rowTop + kColorRowHeight * u < body.top) continue;
                    if (rowTop > body.bottom) break;
                    const int colorIndex = static_cast<int>(i);
                    D2D1_RECT_F swatchRect =
                        D2D1::RectF(rect.left + 11.0f * u, rowTop + 3.0f * u,
                                    rect.left + 41.0f * u, rowTop + 33.0f * u);
                    /* 悬停色块时略微放大，给一个不改变命中区的视觉反馈。 */
                    if (hotColor && hoverColorIndex_ == colorIndex &&
                        hoverColorPart_ == 0) {
                        swatchRect = rack::Inset(swatchRect, -2.0f * u, -2.0f * u);
                    }
                    const D2D1_RECT_F topTextRect =
                        D2D1::RectF(rect.left + 49.0f * u, rowTop + 3.0f * u,
                                    rect.right - 11.0f * u, rowTop + 18.0f * u);
                    const D2D1_RECT_F bottomTextRect =
                        D2D1::RectF(rect.left + 49.0f * u, rowTop + 18.0f * u,
                                    rect.right - 11.0f * u, rowTop + 32.0f * u);
                    const bool rgbFirst = card::ColorTextIsRgb(item.note);
                    const D2D1_RECT_F hexRect = rgbFirst ? bottomTextRect : topTextRect;
                    const D2D1_RECT_F rgbRect = rgbFirst ? topTextRect : bottomTextRect;
                    const card::Rgba& color = colors[i];
                    swatch->SetColor(D2D1::ColorF(color.r / 255.0f, color.g / 255.0f,
                                                  color.b / 255.0f, color.a / 255.0f));
                    target->FillRoundedRectangle(
                        D2D1::RoundedRect(swatchRect, 6.0f * u, 6.0f * u), white.Get());
                    target->FillRoundedRectangle(
                        D2D1::RoundedRect(swatchRect, 6.0f * u, 6.0f * u), swatch.Get());
                    target->DrawRoundedRectangle(
                        D2D1::RoundedRect(swatchRect, 6.0f * u, 6.0f * u),
                        swatchBorder.Get(), 0.7f * u);
                    const bool hotHex =
                        hotColor && hoverColorIndex_ == colorIndex && hoverColorPart_ == 1;
                    const bool hotRgb =
                        hotColor && hoverColorIndex_ == colorIndex && hoverColorPart_ == 2;
                    IDWriteTextFormat* hexFormat =
                        (hotHex && cardColorHexBoldFormat_) ? cardColorHexBoldFormat_.Get()
                                                            : cardColorHexFormat_.Get();
                    IDWriteTextFormat* rgbFormat =
                        (hotRgb && cardColorRgbBoldFormat_) ? cardColorRgbBoldFormat_.Get()
                                                            : cardColorRgbFormat_.Get();
                    const bool flashThis =
                        copyFlashUntil_ > rack::NowMs() && copyFlashItemId_ == item.id &&
                        copyFlashColorIndex_ == colorIndex;
                    const bool flashHex = flashThis && copyFlashPart_ == 1;
                    const bool flashRgb = flashThis && copyFlashPart_ == 2;
                    /* 上面那行用实色，下面那行用浅色；RGB/Hex 谁在上谁实。 */
                    ID2D1Brush* hexBrush = rgbFirst ? mutedBrush.Get() : bodyBrush.Get();
                    ID2D1Brush* rgbBrush = rgbFirst ? bodyBrush.Get() : mutedBrush.Get();
                    const std::wstring hex = card::HexText(color);
                    const std::wstring rgb = card::RgbText(color);
                    target->DrawText(hex.c_str(), static_cast<UINT32>(hex.size()), hexFormat,
                                     hexRect,
                                     flashHex ? flashGreen.Get() : hexBrush,
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    target->DrawText(rgb.c_str(), static_cast<UINT32>(rgb.size()), rgbFormat,
                                     rgbRect,
                                     flashRgb ? flashGreen.Get() : rgbBrush,
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    } else if (item.cardType == card::Type::Text || item.cardType == card::Type::Link) {
        const D2D1_RECT_F textRect = rack::Inset(body, 11.0f * u, 8.0f * u);
        /* 链接正文悬停变蓝（默认 #1677FF，跟随这张卡的强调色）。 */
        ID2D1Brush* textBrush =
            (item.cardType == card::Type::Link && hotLink) ? accentBrush.Get()
                                                           : bodyBrush.Get();
        target->DrawText(item.note.c_str(), static_cast<UINT32>(item.note.size()),
                         cardBodyFormat_.Get(), textRect, textBrush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        std::vector<std::wstring> names;
        std::vector<std::wstring> paths = item.paths;
        if (paths.empty() && !item.path.empty()) {
            paths.push_back(item.path);
        }
        const bool foldersOnly =
            item.cardType == card::Type::File && card::AllPathsAreFolders(paths);
        if (!paths.empty()) {
            for (const auto& path : paths) {
                names.push_back(foldersOnly ? path : card::FileName(path));
            }
        } else if (!item.label.empty()) {
            names.push_back(item.label);
        }
        const float lineHeight = 16.0f * u;
        const float textLeft = rect.left + 11.0f * u;
        const float textWidth = rect.right - 11.0f * u - textLeft;
        float y = body.top + 7.0f * u;
        for (const auto& name : names) {
            if (y >= body.bottom - 2.0f * u || textWidth <= 0.0f) break;
            if (cardFileNameWrapFormat_ && graphics_ && graphics_->dwrite()) {
                ComPtr<IDWriteTextLayout> layout;
                const HRESULT hr = graphics_->dwrite()->CreateTextLayout(
                    name.c_str(), static_cast<UINT32>(name.size()),
                    cardFileNameWrapFormat_.Get(), textWidth, body.bottom - y,
                    layout.GetAddressOf());
                if (SUCCEEDED(hr) && layout) {
                    DWRITE_TEXT_METRICS metrics{};
                    layout->GetMetrics(&metrics);
                    const float height = std::max(lineHeight, metrics.height);
                    target->DrawTextLayout(D2D1::Point2F(textLeft, y), layout.Get(),
                                           bodyBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    y += height + 2.0f * u;
                    continue;
                }
            }
            if (y + lineHeight > body.bottom) break;
            target->DrawText(name.c_str(), static_cast<UINT32>(name.size()),
                             cardFileNameFormat_.Get(),
                             D2D1::RectF(textLeft, y, textLeft + textWidth, y + lineHeight),
                             bodyBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += lineHeight;
        }
    }
    target->PopAxisAlignedClip();
}

void RackView::DrawPaperCard(ID2D1RenderTarget* target, const Item& item,
                             const D2D1_RECT_F& rect, float s)
{
    (void)s;
    if (!cardTitleFormat_ || !cardBodyFormat_) return;
    const float u = (rect.right - rect.left) / kFlatCardSize;
    const float sw = (rect.right - rect.left) / 496.0f;
    const float sh = (rect.bottom - rect.top) / 503.0f;
    D2D1_COLOR_F headerColor = NotePaperHeader();
    D2D1_COLOR_F titleColor = NotePaperTitle();
    D2D1_COLOR_F paperTop = NotePaperTop();
    D2D1_COLOR_F paperBottom = NotePaperBottom();
    D2D1_COLOR_F bodyInk = NotePaperInk();
    if (item.accentSet) {
        const D2D1_COLOR_F accent = RgbColor(item.accent);
        headerColor = accent;
        /* 正文背景 = 选中颜色 30% 叠在白底上。 */
        paperTop = MixColor(accent, D2D1::ColorF(1, 1, 1), 0.70f);
        paperBottom = paperTop;
        /* 便签标题统一白色，和其他卡片的头部文字一致。 */
        titleColor = D2D1::ColorF(1.0f, 1.0f, 1.0f);
        /* 便签正文始终跟普通卡片正文同色。 */
        bodyInk = NotePaperInk();
    }
    auto pt = [&](float x, float y) {
        return D2D1::Point2F(rect.left + x * sw, rect.top + y * sh);
    };
    ComPtr<ID2D1PathGeometry> notePath;
    if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(notePath.GetAddressOf())) && notePath) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(notePath->Open(sink.GetAddressOf())) && sink) {
            sink->BeginFigure(pt(494.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(pt(0.0f, 0.0f));
            sink->AddLine(pt(0.0f, 494.5f));
            sink->AddBezier(D2D1::BezierSegment(pt(188.7f, 497.2f), pt(294.4f, 499.8f),
                                                pt(486.0f, 478.5f)));
            sink->AddBezier(D2D1::BezierSegment(pt(493.7f, 297.8f), pt(495.4f, 192.9f),
                                                pt(494.0f, 0.0f)));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, paperTop},
                {1.0f, paperBottom},
            };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> noteFill;
            if (SUCCEEDED(target->CreateGradientStopCollection(stops, 2,
                                                               collection.GetAddressOf())) &&
                SUCCEEDED(target->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(pt(9.0f, 0.0f), pt(480.0f, 468.5f)),
                    collection.Get(), noteFill.GetAddressOf()))) {
                target->FillGeometry(notePath.Get(), noteFill.Get());
            }
        }
    }

    const D2D1_RECT_F header =
        D2D1::RectF(rect.left, rect.top, rect.right, rect.top + kFlatHeader * u);
    ComPtr<ID2D1SolidColorBrush> headerFill;
    ComPtr<ID2D1SolidColorBrush> titleInk;
    ComPtr<ID2D1SolidColorBrush> metaInk;
    ComPtr<ID2D1SolidColorBrush> bodyInkBrush;
    if (FAILED(target->CreateSolidColorBrush(headerColor, headerFill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(titleColor, titleInk.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(util::WithAlpha(titleColor, 0.50f),
                                             metaInk.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(bodyInk, bodyInkBrush.GetAddressOf()))) {
        return;
    }
    target->FillRectangle(header, headerFill.Get());
    target->DrawText(L"便签", 2, cardTitleFormat_.Get(),
                     D2D1::RectF(header.left + 11.0f * u, header.top + 4.0f * u,
                                 header.right - 11.0f * u, header.top + 19.0f * u),
                     titleInk.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const std::wstring time = card::FormatCardTime(item.createdAt);
    if (!time.empty()) {
        target->DrawText(time.c_str(), static_cast<UINT32>(time.size()), cardMetaFormat_.Get(),
                         D2D1::RectF(header.left + 11.0f * u, header.top + 19.0f * u,
                                     header.right - 11.0f * u, header.top + 31.0f * u),
                         metaInk.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    target->PushAxisAlignedClip(
        D2D1::RectF(rect.left + 2.0f * u, header.bottom, rect.right - 2.0f * u, rect.bottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->DrawText(item.note.c_str(), static_cast<UINT32>(item.note.size()),
                     cardBodyFormat_.Get(),
                     D2D1::RectF(rect.left + 11.0f * u, header.bottom + 8.0f * u,
                                 rect.right - 11.0f * u, rect.bottom - 10.0f * u),
                     bodyInkBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    target->PopAxisAlignedClip();
}

void RackView::DrawCard(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                        float s, float swayAngle, float hoverAmount, bool hovered,
                        bool pressed, bool hotLock, bool hotClose, bool hotLink,
                        bool hotColor)
{
    const float radius = 12.0f * s;
    const float cardScale =
        pressed ? 0.97f : (1.0f + 0.040f * rack::Clamp(hoverAmount, 0.0f, 1.05f));
    D2D1_RECT_F rect = box.card;
    if (pressed) {
        rect.top += 1.0f * s;
        rect.bottom += 1.0f * s;
    }
    const D2D1_POINT_2F pivot =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
    /* 以夹子为原点缩放：照片轻微抬起，夹子本身不动。 */
    target->SetTransform(
        D2D1::Matrix3x2F::Scale(cardScale, cardScale, pivot) *
        D2D1::Matrix3x2F::Rotation(box.angle + swayAngle, pivot));

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    ComPtr<ID2D1SolidColorBrush> imageBack;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                             fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.88f, 0.91f),
                                             border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.95f, 0.97f),
                                             imageBack.GetAddressOf()))) {
        return;
    }

    if (IsNewCard(item)) {
        if (item.cardType == card::Type::Note) {
            DrawPaperCard(target, item, rect, s);
        } else {
            DrawFlatCard(target, item, rect, s, hotLink, hotColor);
        }
    } else {
    /* 录屏无封面 / 文件：纯色卡片 + 文字 + 左下角类型图标。 */
    const bool isNoteCard = item.path.empty() && !item.note.empty();
    const bool plainCard = item.kind == 3 || (item.kind == 2 && !item.thumb.valid());
    if (plainCard) {
        const D2D1_COLOR_F cardColor =
            item.kind == 3 ? D2D1::ColorF(0.24f, 0.30f, 0.42f)
                           : D2D1::ColorF(0.31f, 0.29f, 0.25f);
        ComPtr<ID2D1SolidColorBrush> plain;
        ComPtr<ID2D1SolidColorBrush> ink;
        if (SUCCEEDED(target->CreateSolidColorBrush(cardColor, plain.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.97f),
                                                    ink.GetAddressOf()))) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                         fill.Get());
            /* 和图片卡一样留一圈同样粗细的白框，再填纯色内容。 */
            const D2D1_RECT_F inner = rack::Inset(rect, kFrame * s, kFrame * s);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(inner, std::max(2.0f * s, radius - kFrame * s),
                                  std::max(2.0f * s, radius - kFrame * s)),
                plain.Get());
            const std::wstring& text = item.label.empty() ? item.note : item.label;
            if (noteFormat_ && !text.empty()) {
                target->DrawText(
                    text.c_str(), static_cast<UINT32>(text.size()), noteFormat_.Get(),
                    rack::Inset(inner, 10.0f * s, 12.0f * s), ink.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            DrawFileBadge(target, inner, item, s);
        }
    } else {
        if (!isNoteCard) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                         fill.Get());
        }

        D2D1_RECT_F imageRect = rack::Inset(rect, kFrame * s, kFrame * s);
        /* SVG 预览图本身没有留白：再向内收一圈，避免顶到相框。 */
        const bool svgPreview = IsSvgPreviewPath(item.path);
        D2D1_RECT_F contentRect =
            svgPreview ? rack::Inset(imageRect, 12.0f * s, 12.0f * s) : imageRect;
    const bool textCard = item.path.empty() && !item.note.empty();
    if (textCard) {
        const float sw = (rect.right - rect.left) / 496.0f;
        const float sh = (rect.bottom - rect.top) / 503.0f;
        auto pt = [&](float x, float y) {
            return D2D1::Point2F(rect.left + x * sw, rect.top + y * sh);
        };
        ComPtr<ID2D1PathGeometry> notePath;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(notePath.GetAddressOf())) && notePath) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(notePath->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(pt(494.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(pt(0.0f, 0.0f));
                sink->AddLine(pt(0.0f, 494.5f));
                sink->AddBezier(D2D1::BezierSegment(pt(188.7f, 497.2f), pt(294.4f, 499.8f),
                                                    pt(486.0f, 478.5f)));
                sink->AddBezier(D2D1::BezierSegment(pt(493.7f, 297.8f), pt(495.4f, 192.9f),
                                                    pt(494.0f, 0.0f)));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                D2D1_GRADIENT_STOP stops[2] = {
                    {0.0f, NotePaperTop()},
                    {1.0f, NotePaperBottom()},
                };
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> noteFill;
                if (SUCCEEDED(target->CreateGradientStopCollection(
                        stops, 2, collection.GetAddressOf())) &&
                    SUCCEEDED(target->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(pt(9.0f, 0.0f), pt(480.0f, 468.5f)),
                        collection.Get(), noteFill.GetAddressOf()))) {
                    target->FillGeometry(notePath.Get(), noteFill.Get());
                }
            }
        }
        if (noteFormat_) {
            const D2D1_RECT_F textRect = rack::Inset(rect, 16.0f * s, 18.0f * s);
            ComPtr<ID2D1SolidColorBrush> ink;
            if (SUCCEEDED(target->CreateSolidColorBrush(NotePaperInk(),
                                                        ink.GetAddressOf()))) {
                target->DrawText(item.note.c_str(), static_cast<UINT32>(item.note.size()),
                                 noteFormat_.Get(), textRect, ink.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }    } else {
    target->FillRoundedRectangle(D2D1::RoundedRect(imageRect, 7.0f * s, 7.0f * s),
                                 imageBack.Get());

    if (item.thumb.valid()) {
        const D2D1_SIZE_F size = item.thumb.bitmap->GetSize();
        if (size.width > 0.0f && size.height > 0.0f) {
            const float fit = std::min((contentRect.right - contentRect.left) / size.width,
                                       (contentRect.bottom - contentRect.top) / size.height);
            const float w = size.width * fit;
            const float h = size.height * fit;
            const float cx = (contentRect.left + contentRect.right) * 0.5f;
            const float cy = (contentRect.top + contentRect.bottom) * 0.5f;
            D2D1_RECT_F dest = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                                           cy + h * 0.5f);

            ComPtr<ID2D1RoundedRectangleGeometry> geometry;
            graphics_->d2d()->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(imageRect, 7.0f * s, 7.0f * s),
                geometry.GetAddressOf());
            ComPtr<ID2D1Layer> layer;
            if (geometry && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
                target->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
                target->DrawBitmap(item.thumb.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                target->PopLayer();
            } else {
                target->DrawBitmap(item.thumb.bitmap.Get(), dest, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
    } else if (placeholderFormat_) {
        ComPtr<ID2D1SolidColorBrush> muted;
        if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.38f, 0.42f, 0.9f),
                                                    muted.GetAddressOf()))) {
            D2D1_RECT_F textRect = rack::Inset(imageRect, 6.0f * s, 6.0f * s);
            target->DrawText(L"无法读取", 4, placeholderFormat_.Get(), textRect, muted.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    }
    }
    if (item.kind == 2) {
        DrawTypeBadge(target, rect, item.kind, s);
    }
    if (!isNoteCard) {
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), border.Get(),
                                     1.0f * s);
    }
    } /* !IsNewCard */

    if (hovered && hoverAmount > 0.15f) {
        const float size = 20.0f * s;
        const D2D1_POINT_2F closeCenter =
            D2D1::Point2F(rect.right - 16.0f * s, rect.top + 16.0f * s);
        const D2D1_POINT_2F lockCenter =
            D2D1::Point2F(rect.left + 16.0f * s, rect.top + 16.0f * s);
        ComPtr<ID2D1SolidColorBrush> circle;
        ComPtr<ID2D1SolidColorBrush> ink;
        const Theme ui = Theme::MenuStyle(uiDark_);
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 0.94f),
                circle.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(ui.fg, ink.GetAddressOf()))) {
            /* 悬停反馈只改颜色，不改大小、也不描边：常态是透明度偏低的实心圆片，
               鼠标压上去就变实（锁定键回到卡片色，关闭键整颗变红）。 */
            auto glass = [&](const D2D1_POINT_2F& c, bool hot, bool danger) {
                const float radius = size * 0.5f;
                if (hot) {
                    if (danger) {
                        circle->SetColor(D2D1::ColorF(0.898f, 0.282f, 0.302f, 0.98f));
                    } else {
                        circle->SetColor(D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 1.0f));
                    }
                } else {
                    circle->SetColor(D2D1::ColorF(ui.bg.r, ui.bg.g, ui.bg.b, 0.70f));
                }
                target->FillEllipse(D2D1::Ellipse(c, radius, radius), circle.Get());
            };
            glass(closeCenter, hotClose, true);
            ink->SetColor(hotClose ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                   : D2D1::ColorF(ui.fg.r, ui.fg.g, ui.fg.b, 0.85f));
            if (iconClose_) {
                DrawIcon(target, iconClose_.Get(), closeCenter, 6.0f * s, ink.Get());
            } else {
                const float arm = 3.7f * s;
                target->DrawLine(D2D1::Point2F(closeCenter.x - arm, closeCenter.y - arm),
                                 D2D1::Point2F(closeCenter.x + arm, closeCenter.y + arm),
                                 ink.Get(), 1.4f * s);
                target->DrawLine(D2D1::Point2F(closeCenter.x + arm, closeCenter.y - arm),
                                 D2D1::Point2F(closeCenter.x - arm, closeCenter.y + arm),
                                 ink.Get(), 1.4f * s);
            }

            glass(lockCenter, hotLock, false);
            ink->SetColor(D2D1::ColorF(ui.fg.r, ui.fg.g, ui.fg.b, hotLock ? 1.0f : 0.85f));
            if (iconLock_ && iconUnlock_) {
                DrawIcon(target, item.locked ? iconLock_.Get() : iconUnlock_.Get(),
                         lockCenter, 11.0f * s, ink.Get());
            } else {
                const float lockW = 7.0f * s;
                const float lockH = 5.5f * s;
                const D2D1_RECT_F body = D2D1::RectF(
                    lockCenter.x - lockW * 0.5f, lockCenter.y - 0.5f * s,
                    lockCenter.x + lockW * 0.5f, lockCenter.y + lockH);
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(body, 1.5f * s, 1.5f * s), ink.Get());
                const float shackle = 2.5f * s;
                target->DrawLine(
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 0.5f * s),
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 3.0f * s),
                    ink.Get(), 1.2f * s);
                target->DrawLine(
                    D2D1::Point2F(lockCenter.x - shackle, lockCenter.y - 3.0f * s),
                    D2D1::Point2F(lockCenter.x + (item.locked ? shackle : 0.5f * s),
                                  lockCenter.y - 3.0f * s),
                    ink.Get(), 1.2f * s);
                if (item.locked) {
                    target->DrawLine(
                        D2D1::Point2F(lockCenter.x + shackle, lockCenter.y - 3.0f * s),
                        D2D1::Point2F(lockCenter.x + shackle, lockCenter.y - 0.5f * s),
                        ink.Get(), 1.2f * s);
                }
            }
        }
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RackView::DrawNote(ID2D1RenderTarget* target, const Item& item, const CardBox& box,
                        const Theme& theme, float s, float swayAngle)
{
    if (item.note.empty() || !box.hasNote) {
        return;
    }
    const NotePalette palette = NotePaletteFor(item.noteColor, theme.dark);
    const D2D1_POINT_2F center =
        D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);

    target->SetTransform(D2D1::Matrix3x2F::Rotation(box.angle + swayAngle, center));
    /* 便签条的外阴影同样不画：它是 alpha>0 的空白像素，
       会把卡片下方的点击一起吃掉。 */

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    ComPtr<ID2D1SolidColorBrush> ink;
    ComPtr<ID2D1SolidColorBrush> fold;
    if (FAILED(target->CreateSolidColorBrush(palette.fill, fill.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.edge, edge.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.ink, ink.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(palette.fold, fold.GetAddressOf()))) {
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }

    target->FillRectangle(box.note, fill.Get());
    target->DrawRectangle(box.note, edge.Get(), 1.0f * s);

    /* 右下角折角。 */
    const float foldSize = 8.0f * s;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(path.GetAddressOf())) &&
        SUCCEEDED(path->Open(sink.GetAddressOf()))) {
        sink->BeginFigure(D2D1::Point2F(box.note.right - foldSize, box.note.bottom),
                          D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(box.note.right, box.note.bottom - foldSize));
        sink->AddLine(D2D1::Point2F(box.note.right, box.note.bottom));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        target->FillGeometry(path.Get(), fold.Get());
    }

    if (noteFormat_) {
        const float padding = 5.0f * s;
        const float maxWidth = (box.note.right - box.note.left) - padding * 2.0f - foldSize;
        const float maxHeight = (box.note.bottom - box.note.top) - 4.0f * s;
        ComPtr<IDWriteTextLayout> textLayout;
        if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                item.note.c_str(), static_cast<UINT32>(item.note.size()),
                noteFormat_.Get(), maxWidth, maxHeight, textLayout.GetAddressOf()))) {
            target->DrawTextLayout(D2D1::Point2F(box.note.left + padding,
                                                 box.note.top + padding),
                                   textLayout.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

HBITMAP RackView::MakeDragGhost(const Item& item, int width, int height, SIZE* sizeOut)
{
    if (!graphics_ || !graphics_->d2d() || width < 8 || height < 8) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        return nullptr;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    ComPtr<ID2D1DCRenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    RECT bounds{0, 0, width, height};
    HRESULT hr = graphics_->d2d()->CreateDCRenderTarget(&properties, target.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = target->BindDC(dc, &bounds);
    }
    if (SUCCEEDED(hr)) {
        CardBox box;
        box.card = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                               static_cast<float>(height));
        box.image = box.card;
        box.pinTop = 0.0f;
        box.angle = 0.0f;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        /* 直接复用绳子上那套绘制：纸色渐变、形状、字号与内边距全都一致。 */
        DrawCard(target.Get(), item, box, scale_, 0.0f, 0.0f, false, false);
        hr = target->EndDraw();
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    if (FAILED(hr)) {
        DeleteObject(bitmap);
        return nullptr;
    }
    if (sizeOut) {
        sizeOut->cx = width;
        sizeOut->cy = height;
    }
    return bitmap;
}

HBITMAP RackView::MakeColorGhost(const card::Rgba& color, int width, int height,
                                 SIZE* sizeOut)
{
    if (!graphics_ || !graphics_->d2d() || width < 8 || height < 8) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    ComPtr<ID2D1DCRenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    RECT bounds{0, 0, width, height};
    HRESULT hr = graphics_->d2d()->CreateDCRenderTarget(&properties, target.GetAddressOf());
    if (SUCCEEDED(hr)) hr = target->BindDC(dc, &bounds);
    if (SUCCEEDED(hr)) {
        const float inset = 2.0f * scale_;
        const D2D1_RECT_F card =
            D2D1::RectF(inset, inset, width - inset, height - inset);
        const float radius = std::max(3.0f, 12.0f * scale_);
        ComPtr<ID2D1SolidColorBrush> fill;
        ComPtr<ID2D1SolidColorBrush> border;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                             color.a / 255.0f),
                fill.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f),
                                                    border.GetAddressOf()))) {
            target->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius), fill.Get());
            target->DrawRoundedRectangle(D2D1::RoundedRect(card, radius, radius), border.Get(),
                                         1.5f * scale_);
        }
        hr = target->EndDraw();
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    if (FAILED(hr)) {
        DeleteObject(bitmap);
        return nullptr;
    }
    if (sizeOut) {
        sizeOut->cx = width;
        sizeOut->cy = height;
    }
    return bitmap;
}

RackHit RackView::HitTest(float x, float y, float scale) const
{
    RackHit hit;
    const RackLayout& layout = layout_;
    const float s = std::max(0.5f, scale);

    const float lineHitBottom = layout.lineY + layout.sag + 8.0f * s;
    const bool lineArea = y >= 0.0f && y <= lineHitBottom &&
                          x >= layout.lineStart - 6.0f * s &&
                          x <= layout.lineEnd + 6.0f * s;
    if (!layout.expanded) {
        if (lineArea) {
            hit.part = RackPart::Line;
        }
        return hit;
    }

    for (int i = static_cast<int>(cards_.size()) - 1; i >= 0; --i) {
        if (i == editingCard_) continue; /* 编辑中的卡片已经藏起来了 */
        const CardBox& box = cards_[static_cast<size_t>(i)];
        const D2D1_POINT_2F pivot =
            D2D1::Point2F((box.card.left + box.card.right) * 0.5f, box.card.top);
        const D2D1_POINT_2F local = RotateAround(D2D1::Point2F(x, y), pivot, -box.angle);
        if (i == lastHovered_) {
            const D2D1_RECT_F lockRect = D2D1::RectF(
                box.card.left + 6.0f * s, box.card.top + 6.0f * s,
                box.card.left + 26.0f * s, box.card.top + 26.0f * s);
            if (rack::PointIn(lockRect, local.x, local.y)) {
                hit.part = RackPart::Lock;
                hit.index = i;
                return hit;
            }
            const D2D1_RECT_F closeRect = D2D1::RectF(
                box.card.right - 26.0f * s, box.card.top + 6.0f * s,
                box.card.right - 6.0f * s, box.card.top + 26.0f * s);
            if (rack::PointIn(closeRect, local.x, local.y)) {
                hit.part = RackPart::Close;
                hit.index = i;
                return hit;
            }
        }
        if (box.hasNote && rack::PointIn(box.note, local.x, local.y)) {
            hit.part = RackPart::Note;
            hit.index = i;
            return hit;
        }
        {
            /* 夹子：卡片顶部中间、绳子附近的一小块。图片/录屏点它把内容贴到桌面；
               便签没有可贴的东西——命中照样拦截（不然会传递到下层窗口），
               但光标和点击反馈在 RackWindow 里按卡片类型过滤。 */
            const float cx = (box.card.left + box.card.right) * 0.5f;
            const D2D1_RECT_F clipRect =
                D2D1::RectF(cx - 10.0f * s, box.pinTop - 10.0f * s, cx + 10.0f * s,
                            box.card.top + 2.0f * s);
            if (rack::PointIn(clipRect, local.x, local.y)) {
                hit.part = RackPart::Clip;
                hit.index = i;
                return hit;
            }
        }
        if (box.flat && rack::PointIn(box.body, local.x, local.y)) {
            if (box.cardType == card::Type::Link &&
                rack::PointIn(box.linkText, local.x, local.y)) {
                hit.part = RackPart::CardLink;
                hit.index = i;
                return hit;
            }
            if (box.cardType == card::Type::Color) {
                for (const ColorRowBox& row : box.colorRows) {
                    if (!rack::PointIn(box.body, local.x, local.y)) break;
                    if (rack::PointIn(row.swatch, local.x, local.y)) {
                        hit.part = RackPart::CardColor;
                        hit.index = i;
                        hit.colorIndex = row.index;
                        hit.colorPart = 0;
                        return hit;
                    }
                    if (rack::PointIn(row.hex, local.x, local.y)) {
                        hit.part = RackPart::CardColor;
                        hit.index = i;
                        hit.colorIndex = row.index;
                        hit.colorPart = 1;
                        return hit;
                    }
                    if (rack::PointIn(row.rgb, local.x, local.y)) {
                        hit.part = RackPart::CardColor;
                        hit.index = i;
                        hit.colorIndex = row.index;
                        hit.colorPart = 2;
                        return hit;
                    }
                }
            }
        }
        if (rack::PointIn(box.card, local.x, local.y)) {
            hit.part = RackPart::Card;
            hit.index = i;
            return hit;
        }
    }

    /* 信封画在篮子上面：两者在换槽位的动画里会重叠，命中也要按画面上
       谁在上面来判，否则垫在下面的篮子会抢走信封的点击。 */
    if (rack::PointIn(envelopeRect_, x, y)) {
        hit.part = RackPart::Envelope;
        return hit;
    }
    if (!historyDisabled_ && rack::PointIn(basketRect_, x, y)) {
        hit.part = RackPart::Basket;
        return hit;
    }
    if (lineArea) {
        hit.part = RackPart::Line;
        return hit;
    }
    if (rack::PointIn(layout.content, x, y)) {
        hit.part = RackPart::Background;
    }
    return hit;
}

const CardBox* RackView::Card(int index) const
{
    if (index < 0 || index >= static_cast<int>(cards_.size())) {
        return nullptr;
    }
    return &cards_[static_cast<size_t>(index)];
}


