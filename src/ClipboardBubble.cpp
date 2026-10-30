#include "ClipboardBubble.h"
#include "MiniJson.h"
#include "Util.h"

#include <commctrl.h>
#include <imm.h>

#include <chrono>
#include <ctime>
#include <cwctype>
#include <filesystem>

namespace {

constexpr float kBubbleW = 380.0f;
constexpr float kArrowH = 9.0f;
constexpr float kPad = 12.0f;
constexpr float kSearchH = 32.0f;
constexpr float kPlusW = 32.0f;
constexpr float kPlusGap = 8.0f;
constexpr float kChipH = 28.0f;
constexpr float kChipGap = 6.0f;
constexpr float kChipPadX = 10.0f;
constexpr float kGap = 10.0f;
constexpr float kCardGap = 8.0f;
constexpr float kCardPadX = 10.0f;
constexpr float kCardPadY = 10.0f;
constexpr float kCardHeaderH = 20.0f;
constexpr float kCardHeaderTop = 10.0f;
constexpr float kCardHeaderGap = 2.0f;
constexpr float kCardTagH = 16.0f;
constexpr float kCardTagGap = 12.0f;
constexpr float kTagChipGap = 6.0f;
constexpr float kActionSize = 20.0f;
constexpr float kActionGap = 4.0f;
constexpr float kThumbW = 96.0f;
constexpr float kThumbH = 48.0f;
constexpr float kPanelH = 550.0f;
constexpr UINT_PTR kCaretTimer = 0x51;
constexpr int kTagBuffer = 8192;

const wchar_t* const kFixedChips[] = {L"全部", L"截图", L"文本", L"录屏", L"图片", L"文件"};
constexpr int kFixedChipCount = 6;

std::wstring Lower(std::wstring text)
{
    for (auto& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
}

D2D1_COLOR_F Rgb(uint32_t rgb)
{
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
}

std::wstring Trim(std::wstring text)
{
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\r' || text.back() == L'\n'))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\r' || text[start] == L'\n'))
        ++start;
    return text.substr(start);
}

std::vector<std::wstring> SplitLines(const std::wstring& text)
{
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t pos = text.find(L'\n', start);
        if (pos == std::wstring::npos) pos = text.size();
        std::wstring part = Trim(text.substr(start, pos - start));
        if (!part.empty()) out.push_back(std::move(part));
        if (pos == text.size()) break;
        start = pos + 1;
    }
    return out;
}

std::wstring JoinLines(const std::vector<std::wstring>& items)
{
    std::wstring out;
    for (const auto& item : items) {
        if (item.empty()) continue;
        if (!out.empty()) out.push_back(L'\n');
        out += item;
    }
    return out;
}

std::wstring FormatWhen(long long ms)
{
    if (ms <= 0) return L"";
    const time_t tt = static_cast<time_t>(ms / 1000);
    struct tm local {};
    localtime_s(&local, &tt);
    const long long now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    wchar_t buf[64]{};
    if (now - ms < 365LL * 24 * 3600 * 1000) {
        wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &local);
    } else {
        wcsftime(buf, 64, L"%Y-%m-%d", &local);
    }
    return buf;
}

std::wstring FormatDuration(int seconds)
{
    if (seconds <= 0) return L"";
    wchar_t buf[32]{};
    if (seconds >= 3600) {
        swprintf_s(buf, L"%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
    } else {
        swprintf_s(buf, L"%02d:%02d", seconds / 60, seconds % 60);
    }
    return buf;
}

std::wstring FileNameOf(const std::wstring& path)
{
    if (path.empty()) return L"";
    return std::filesystem::path(path).filename().wstring();
}

std::wstring FolderNameOf(const std::wstring& path)
{
    if (path.empty()) return L"";
    const auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) return L"";
    return parent.filename().wstring();
}

void DrawTextLayoutBlock(IDWriteFactory* dwrite, ID2D1RenderTarget* target,
                         const std::wstring& text, IDWriteTextFormat* format,
                         const D2D1_RECT_F& rect, ID2D1Brush* brush, bool wrap)
{
    if (!dwrite || !format || text.empty()) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        format, rect.right - rect.left,
                                        rect.bottom - rect.top,
                                        layout.GetAddressOf())) ||
        !layout) {
        return;
    }
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf())) &&
        ellipsis) {
        layout->SetTrimming(&trimming, ellipsis.Get());
    }
    target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

} /* namespace */

HHOOK ClipboardBubble::mouseHook_ = nullptr;
ClipboardBubble* ClipboardBubble::hookOwner_ = nullptr;

ClipboardBubble::ClipboardBubble() = default;

ClipboardBubble::~ClipboardBubble()
{
    Close();
    DestroyModalEdit();
    DestroyNoteEdit();
    DestroySearchEdit();
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (hookOwner_ == this) hookOwner_ = nullptr;
}

LRESULT CALLBACK ClipboardBubble::MouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) &&
        hookOwner_ && hookOwner_->hwnd()) {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT rc{};
        GetWindowRect(hookOwner_->hwnd(), &rc);
        if (mouse && !PtInRect(&rc, mouse->pt)) {
            PostMessageW(hookOwner_->hwnd(), WM_APP + 1, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool ClipboardBubble::Init(Graphics* graphics, const SnowAirHost* host)
{
    host_ = host;
    if (!Create(graphics, util::GetDllModule(), L"SnowAirClipBubble", L"SnowAir Clipboard")) {
        return false;
    }
    LoadIcons();
    CreateSearchEdit();
    LoadCategories();
    return true;
}

void ClipboardBubble::LoadIcons()
{
    if (iconFace_ || !graphics_ || !graphics_->dwrite()) return;
    const std::wstring path =
        util::JoinPath(util::JoinPath(util::GetDllDir(), L"assets"), L"iconfont.ttf");
    ComPtr<IDWriteFontFile> file;
    if (FAILED(graphics_->dwrite()->CreateFontFileReference(path.c_str(), nullptr,
                                                            file.GetAddressOf())) ||
        !file) {
        return;
    }
    IDWriteFontFile* files[] = {file.Get()};
    if (FAILED(graphics_->dwrite()->CreateFontFace(
            DWRITE_FONT_FACE_TYPE_TRUETYPE, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE,
            iconFace_.GetAddressOf())) ||
        !iconFace_) {
        return;
    }
    BuildGlyph(0xE647, iconCopy_);    // copy
    BuildGlyph(0xE65B, iconDelete_);  // delete
    BuildGlyph(0xE612, iconPeg_);     // hang on the rack
    BuildGlyph(0xE61D, iconText_);    // text
    BuildGlyph(0xE675, iconImage_);   // image
    BuildGlyph(0xE61C, iconFiles_);   // folder
    BuildGlyph(0xE66D, iconGear_);    // filter（分类设置）
    BuildGlyph(0xE640, iconDrag_);    // drag handle
}

void ClipboardBubble::BuildGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out)
{
    if (!iconFace_ || !graphics_ || !graphics_->d2d()) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(graphics_->d2d()->CreatePathGeometry(geometry.GetAddressOf())) || !geometry) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.GetAddressOf())) || !sink) return;
    UINT16 glyph = 0;
    if (iconFace_->GetGlyphIndices(&codepoint, 1, &glyph) != S_OK) return;
    FLOAT advance = 0.0f;
    auto* writeSink = reinterpret_cast<IDWriteGeometrySink*>(sink.Get());
    const HRESULT hr = iconFace_->GetGlyphRunOutline(100.0f, &glyph, &advance, nullptr, 1,
                                                     FALSE, FALSE, writeSink);
    sink->Close();
    if (SUCCEEDED(hr)) out = geometry;
}

void ClipboardBubble::DrawGlyph(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                                D2D1_POINT_2F center, float size, ID2D1Brush* brush)
{
    if (!target || !glyph || !brush) return;
    D2D1_RECT_F bounds{};
    if (FAILED(glyph->GetBounds(D2D1::Matrix3x2F::Identity(), &bounds))) return;
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    const float scale = size / std::max(width, height);
    D2D1_MATRIX_3X2_F saved{};
    target->GetTransform(&saved);
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(
                             center.x - (bounds.left + bounds.right) * 0.5f * scale,
                             center.y - (bounds.top + bounds.bottom) * 0.5f * scale));
    target->FillGeometry(glyph, brush);
    target->SetTransform(saved);
}

/* ------------------------------------------------------------------ */
/* data                                                                */
/* ------------------------------------------------------------------ */

void ClipboardBubble::LoadEntries()
{
    entries_.clear();
    thumbs_.clear();
    if (!host_ || !host_->entry_count || !host_->entry_get) {
        RackLog(L"ClipboardBubble: host has no entry_* API");
        return;
    }
    host_->entry_refresh(host_->user);
    const int count = host_->entry_count(host_->user);
    if (count <= 0) return;
    entries_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        SnowAirEntry raw{};
        if (!host_->entry_get(host_->user, i, &raw)) continue;
        Entry entry;
        entry.label = raw.label;
        entry.path = raw.path;
        entry.thumb = raw.thumb;
        entry.kind = raw.kind;
        entry.source = raw.source;
        entry.width = raw.width;
        entry.height = raw.height;
        entry.duration = raw.duration_sec;
        entry.createdAt = raw.created_at;
        entry.time = FormatWhen(raw.created_at);
        entry.tags = SplitLines(raw.tags);
        entries_.push_back(std::move(entry));
    }
    MeasureCards();
}

std::wstring ClipboardBubble::EntryKey(const Entry& entry) const
{
    if (!entry.path.empty()) return entry.path;
    return L"text:" + entry.label;
}

void ClipboardBubble::RefreshVisibleCategories()
{
    visibleCats_.clear();
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        if (!categories_[static_cast<size_t>(i)].hidden) visibleCats_.push_back(i);
    }
    if (filter_ < 0 || filter_ >= static_cast<int>(visibleCats_.size())) filter_ = 0;
    LayoutChips();
}

bool ClipboardBubble::CategoryMatches(const Entry& entry, const Category& category) const
{
    switch (category.builtin) {
    case 0: return true;
    case 1: return entry.source == SNOWAIR_SOURCE_CAPTURE;
    case 2: return entry.kind == SNOWAIR_ENTRY_TEXT;
    case 3: return entry.source == SNOWAIR_SOURCE_RECORD;
    case 4: return entry.kind == SNOWAIR_ENTRY_IMAGE && entry.source != SNOWAIR_SOURCE_CAPTURE;
    case 5: return entry.kind == SNOWAIR_ENTRY_FILES;
    default: break;
    }
    const auto it = assignments_.find(EntryKey(entry));
    if (it == assignments_.end()) return false;
    for (const auto& name : it->second) {
        if (name == category.name) return true;
    }
    return false;
}

void ClipboardBubble::LoadCategories()
{
    categories_.clear();
    assignments_.clear();
    for (int i = 0; i < kFixedChipCount; ++i) {
        Category category;
        category.name = kFixedChips[i];
        category.builtin = i;
        categories_.push_back(std::move(category));
    }
    std::vector<BYTE> bytes;
    const std::wstring path = util::JoinPath(util::GetDataDir(), L"clip-categories.json");
    if (!util::ReadFileBytes(path, bytes) || bytes.empty()) {
        RefreshVisibleCategories();
        return;
    }
    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    json::Value root;
    if (!json::Parse(text, root)) {
        RefreshVisibleCategories();
        return;
    }
    const json::Value* list = root.Get("categories");
    if (list && list->type == json::Type::Array) {
        std::vector<Category> loaded;
        bool hasBuiltin = false;
        for (const auto& value : list->array) {
            if (value.type == json::Type::String) {
                const std::wstring name = Trim(util::Utf8ToWide(value.text));
                if (name.empty()) continue;
                Category category;
                category.name = name;
                loaded.push_back(std::move(category));
                continue;
            }
            if (value.type != json::Type::Object) continue;
            Category category;
            if (const json::Value* n = value.Get("name")) {
                if (n->type == json::Type::String) category.name = Trim(util::Utf8ToWide(n->text));
            }
            if (const json::Value* b = value.Get("builtin")) {
                if (b->type == json::Type::Number) category.builtin = static_cast<int>(b->number);
            }
            if (const json::Value* h = value.Get("hidden")) {
                if (h->type == json::Type::Bool) category.hidden = h->boolean;
            }
            if (category.builtin >= 0 && category.builtin < kFixedChipCount) {
                category.name = kFixedChips[category.builtin];
                hasBuiltin = true;
            }
            if (!category.name.empty()) loaded.push_back(std::move(category));
        }
        if (!loaded.empty()) {
            if (!hasBuiltin) {
                std::vector<Category> all;
                for (int i = 0; i < kFixedChipCount; ++i) {
                    Category category;
                    category.name = kFixedChips[i];
                    category.builtin = i;
                    all.push_back(std::move(category));
                }
                for (auto& category : loaded) {
                    if (category.builtin < 0) all.push_back(std::move(category));
                }
                loaded.swap(all);
            }
            categories_ = std::move(loaded);
        }
    }
    if (const json::Value* assigned = root.Get("assignments")) {
        if (assigned->type == json::Type::Object) {
            for (const auto& kv : assigned->object) {
                if (kv.second.type != json::Type::Array) continue;
                std::vector<std::wstring> names;
                for (const auto& v : kv.second.array) {
                    if (v.type != json::Type::String) continue;
                    std::wstring name = Trim(util::Utf8ToWide(v.text));
                    if (!name.empty()) names.push_back(std::move(name));
                }
                if (!names.empty()) {
                    assignments_[util::Utf8ToWide(kv.first)] = std::move(names);
                }
            }
        }
    }
    RefreshVisibleCategories();
}

void ClipboardBubble::SaveCategories()
{
    std::string output = "{\n  \"version\": 2,\n  \"categories\": [\n";
    bool first = true;
    for (const auto& category : categories_) {
        if (!first) output += ",\n";
        first = false;
        output += "    { \"name\": ";
        output += json::Quote(util::WideToUtf8(category.name));
        output += ", \"builtin\": ";
        output += std::to_string(category.builtin);
        output += ", \"hidden\": ";
        output += category.hidden ? "true" : "false";
        output += " }";
    }
    output += "\n  ],\n  \"assignments\": {";
    first = true;
    for (const auto& kv : assignments_) {
        if (kv.second.empty()) continue;
        if (!first) output += ",";
        first = false;
        output += "\n    ";
        output += json::Quote(util::WideToUtf8(kv.first));
        output += ": [";
        for (size_t i = 0; i < kv.second.size(); ++i) {
            if (i) output += ", ";
            output += json::Quote(util::WideToUtf8(kv.second[i]));
        }
        output += "]";
    }
    output += "\n  }\n}\n";
    const std::wstring path = util::JoinPath(util::GetDataDir(), L"clip-categories.json");
    if (!util::WriteFileBytes(path, output.data(), output.size())) {
        RackLog(L"ClipboardBubble: failed to write %s", path.c_str());
    }
}

void ClipboardBubble::RefreshCategories()
{
    hostTags_.clear();
    if (host_ && host_->entry_all_tags) {
        std::vector<wchar_t> buffer(kTagBuffer, L'\0');
        host_->entry_all_tags(host_->user, buffer.data(), kTagBuffer);
        hostTags_ = SplitLines(buffer.data());
    }
    RefreshVisibleCategories();
}

void ClipboardBubble::RebuildShown()
{
    shown_.clear();
    const std::wstring needle = Lower(query_);
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Entry& entry = entries_[static_cast<size_t>(i)];
        bool match = true;
        if (filter_ >= 0 && filter_ < static_cast<int>(visibleCats_.size())) {
            match = CategoryMatches(entry, categories_[static_cast<size_t>(visibleCats_[static_cast<size_t>(filter_)])]);
        }
        if (!match) continue;
        if (!needle.empty()) {
            bool hit = Lower(entry.label).find(needle) != std::wstring::npos;
            if (!hit) {
                for (const auto& tag : entry.tags) {
                    if (Lower(tag).find(needle) != std::wstring::npos) {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit) continue;
        }
        shown_.push_back(i);
    }
    hovered_ = -1;
    hoveredAction_ = -1;
    hoveredTag_ = -1;
}

void ClipboardBubble::MeasureCards()
{
    const float s = scale_;
    const float textWidth = CardWidth() - 2.0f * kCardPadX * s;
    for (auto& entry : entries_) {
        float contentH = kThumbH * s;
        if (entry.kind == SNOWAIR_ENTRY_TEXT) {
            float lineH = 17.0f * s;
            int lines = 1;
            if (textFormat_ && graphics_ && graphics_->dwrite() && !entry.label.empty()) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(graphics_->dwrite()->CreateTextLayout(
                        entry.label.c_str(), static_cast<UINT32>(entry.label.size()),
                        textFormat_.Get(), std::max(24.0f, textWidth), 3.0f * 20.0f * s,
                        layout.GetAddressOf())) &&
                    layout) {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    UINT32 lineCount = 0;
                    layout->GetLineMetrics(nullptr, 0, &lineCount);
                    if (lineCount > 0) {
                        std::vector<DWRITE_LINE_METRICS> metrics(lineCount);
                        if (SUCCEEDED(layout->GetLineMetrics(metrics.data(), lineCount,
                                                             &lineCount)) &&
                            !metrics.empty()) {
                            lineH = metrics.front().height;
                            titleLineHeight_ = lineH;
                        }
                        lines = static_cast<int>(std::min<UINT32>(lineCount, 3));
                    }
                }
            }
            contentH = static_cast<float>(std::max(lines, 1)) * lineH;
        } else if (entry.kind == SNOWAIR_ENTRY_FILES || entry.kind == SNOWAIR_ENTRY_MEDIA) {
            contentH = 40.0f * s;
        }
        const float cardH = (kCardHeaderTop + kCardHeaderH + kCardHeaderGap) * s + contentH +
                            (kCardTagGap + kCardTagH + kCardPadY) * s;
        entry.cardHeight = cardH;
    }
}

void ClipboardBubble::Reload(bool resetScroll)
{
    RebuildShown();
    rowTops_.clear();
    rowHeights_.clear();
    float y = 0.0f;
    for (int index : shown_) {
        const float h = entries_[static_cast<size_t>(index)].cardHeight;
        rowTops_.push_back(y);
        rowHeights_.push_back(h);
        y += h + kCardGap * scale_;
    }
    rowsHeight_ = rowTops_.empty() ? 0.0f : y - kCardGap * scale_;

    height_ = kPanelH * scale_;
    const float maxScroll = std::max(0.0f, rowsHeight_ - ListViewportHeight());
    if (resetScroll) scrollY_ = 0.0f;
    scrollY_ = rack::Clamp(scrollY_, 0.0f, maxScroll);
    ResizeAndMove();
}

void ClipboardBubble::ResizeAndMove()
{
    if (!hwnd()) return;
    const int w = static_cast<int>(std::lround(width_));
    const int h = static_cast<int>(std::lround(height_));
    MONITORINFO mi{sizeof(MONITORINFO)};
    const HMONITOR monitor = MonitorFromPoint(POINT{x_, y_}, MONITOR_DEFAULTTONEAREST);
    if (monitor) GetMonitorInfoW(monitor, &mi);
    if (mi.rcWork.bottom > mi.rcWork.top) {
        const int loX = static_cast<int>(mi.rcWork.left) + 8;
        const int hiX = static_cast<int>(mi.rcWork.right) - w - 8;
        const int loY = static_cast<int>(mi.rcWork.top) + 8;
        const int hiY = static_cast<int>(mi.rcWork.bottom) - h - 8;
        x_ = hiX >= loX ? std::clamp(x_, loX, hiX) : loX;
        y_ = hiY >= loY ? std::clamp(y_, loY, hiY) : loY;
    }
    MoveTo(x_, y_, w, h);
    Render();
    LayoutEditors();
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

float ClipboardBubble::SearchTop() const
{
    return (kArrowH + kPad) * scale_;
}

float ClipboardBubble::ChipsTop() const
{
    return SearchTop() + (kSearchH + kGap) * scale_;
}

float ClipboardBubble::ListTop() const
{
    return ChipsTop() + (kChipH + kGap) * scale_;
}

float ClipboardBubble::ListViewportHeight() const
{
    return std::max(0.0f, height_ - ListTop() - kPad * scale_);
}

float ClipboardBubble::CardWidth() const
{
    return width_ - 2.0f * kPad * scale_;
}

float ClipboardBubble::TextContentWidth() const
{
    return CardWidth() - 2.0f * kCardPadX * scale_;
}

float ClipboardBubble::RowTop(int row) const
{
    if (row < 0 || row >= static_cast<int>(rowTops_.size())) return 0.0f;
    return ListTop() + rowTops_[static_cast<size_t>(row)] - scrollY_;
}

void ClipboardBubble::LayoutChips()
{
    chipRects_.clear();
    const float s = scale_;
    const float h = kChipH * s;
    const float top = ChipsTop();
    const float left = kPad * s;
    float x = left - chipScrollX_;
    auto add = [&](const std::wstring& text) {
        const float w = TextWidth(text, chipFormat_.Get()) + 2.0f * kChipPadX * s;
        chipRects_.push_back(D2D1::RectF(x, top, x + w, top + h));
        x += w + kChipGap * s;
    };
    for (int index : visibleCats_) add(categories_[static_cast<size_t>(index)].name);
    chipContentWidth_ = std::max(0.0f, x - left + chipScrollX_ - kChipGap * s);
    const float available = width_ - 2.0f * kPad * s;
    const float maxScroll = std::max(0.0f, chipContentWidth_ - available);
    chipScrollX_ = rack::Clamp(chipScrollX_, 0.0f, maxScroll);
}

float ClipboardBubble::TextWidth(const std::wstring& text, IDWriteTextFormat* format) const
{
    if (text.empty()) return 0.0f;
    if (!format || !graphics_ || !graphics_->dwrite()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(graphics_->dwrite()->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f, 100.0f,
            layout.GetAddressOf())) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void ClipboardBubble::ScrollToShowChip(int chip)
{
    if (chip < 0 || chip >= static_cast<int>(chipRects_.size())) return;
    const float left = kPad * scale_;
    const float right = width_ - kPad * scale_;
    const D2D1_RECT_F rect = chipRects_[static_cast<size_t>(chip)];
    if (rect.left < left) chipScrollX_ -= (left - rect.left);
    if (rect.right > right) chipScrollX_ += (rect.right - right);
    const float available = right - left;
    const float maxScroll = std::max(0.0f, chipContentWidth_ - available);
    chipScrollX_ = rack::Clamp(chipScrollX_, 0.0f, maxScroll);
    LayoutChips();
}

void ClipboardBubble::EnsureRowVisible(int row)
{
    if (row < 0 || row >= static_cast<int>(rowTops_.size())) return;
    const float top = rowTops_[static_cast<size_t>(row)];
    const float height = rowHeights_[static_cast<size_t>(row)];
    const float viewport = ListViewportHeight();
    if (top < scrollY_) scrollY_ = top;
    if (top + height > scrollY_ + viewport) scrollY_ = top + height - viewport;
    const float maxScroll = std::max(0.0f, rowsHeight_ - viewport);
    scrollY_ = rack::Clamp(scrollY_, 0.0f, maxScroll);
}

/* ------------------------------------------------------------------ */
/* hit tests                                                           */
/* ------------------------------------------------------------------ */

int ClipboardBubble::HitRow(float x, float y) const
{
    const float left = kPad * scale_;
    const float right = width_ - kPad * scale_;
    if (x < left || x > right) return -1;
    const float local = y - ListTop() + scrollY_;
    if (local < 0.0f || local >= rowsHeight_) return -1;
    for (int i = 0; i < static_cast<int>(rowTops_.size()); ++i) {
        const float top = rowTops_[static_cast<size_t>(i)];
        if (local >= top && local < top + rowHeights_[static_cast<size_t>(i)]) return i;
    }
    return -1;
}

int ClipboardBubble::HitChip(float x, float y) const
{
    if (y < ChipsTop() || y > ChipsTop() + kChipH * scale_) return -1;
    if (x < kPad * scale_ || x > width_ - kPad * scale_) return -1;
    for (int i = 0; i < static_cast<int>(chipRects_.size()); ++i) {
        if (rack::PointIn(chipRects_[static_cast<size_t>(i)], x, y)) return i;
    }
    return -1;
}

int ClipboardBubble::HitPlus(float x, float y) const
{
    const D2D1_RECT_F rect = D2D1::RectF(
        width_ - (kPad + kPlusW) * scale_, SearchTop(),
        width_ - kPad * scale_, SearchTop() + kSearchH * scale_);
    return rack::PointIn(rect, x, y) ? 1 : 0;
}

bool ClipboardBubble::HitGear(float x, float y) const
{
    const float s = scale_;
    const float right = width_ - (kPad + 2.0f * kPlusW + kPlusGap) * s;
    const D2D1_RECT_F rect = D2D1::RectF(right, SearchTop(), right + kPlusW * s,
                                         SearchTop() + kSearchH * s);
    return rack::PointIn(rect, x, y);
}

int ClipboardBubble::HitModalButton(float x, float y) const
{
    const float s = scale_;
    /* 所有弹窗的按钮几何只在这里定义一次：绘制、悬停和点击共用同一份，
       避免改了尺寸之后按钮画在一处、命中在另一处。 */
    if (deleteConfirm_ >= 0) {
        const float w = 300.0f * s;
        const float h = 150.0f * s;
        const float left = (width_ - w) * 0.5f;
        const float top = (height_ - h) * 0.5f;
        const float btnY = top + h - 44.0f * s;
        if (rack::PointIn(D2D1::RectF(left + w - 148.0f * s, btnY, left + w - 84.0f * s,
                                      btnY + 28.0f * s), x, y)) {
            return 1;
        }
        if (rack::PointIn(D2D1::RectF(left + w - 76.0f * s, btnY, left + w - 16.0f * s,
                                      btnY + 28.0f * s), x, y)) {
            return 3;
        }
        return 0;
    }
    if (noteEditorOpen_) {
        const NoteEditorLayout layout = BuildNoteEditorLayout();
        if (rack::PointIn(layout.cancel, x, y)) return 1;
        if (rack::PointIn(layout.save, x, y)) return 2;
        return 0;
    }
    if (!tagEditorOpen_ && !categoryEditorOpen_) {
        return 0;
    }
    if (tagEditorOpen_) {
        const TagEditorLayout layout = BuildTagEditorLayout();
        if (rack::PointIn(layout.cancel, x, y)) return 1;
        if (rack::PointIn(layout.save, x, y)) return 2;
        return 0;
    }
    const float w = 320.0f * s;
    const float h = 132.0f * s;
    const float pad = 18.0f * s;
    const float left = (width_ - w) * 0.5f;
    const float top = (height_ - h) * 0.5f;
    const float right = left + w;
    const D2D1_RECT_F ok = D2D1::RectF(right - pad - 72.0f * s, top + 86.0f * s, right - pad,
                                       top + 116.0f * s);
    const D2D1_RECT_F cancel = D2D1::RectF(ok.left - 8.0f * s - 72.0f * s, top + 86.0f * s,
                                           ok.left - 8.0f * s, top + 116.0f * s);
    if (rack::PointIn(cancel, x, y)) return 1;
    if (rack::PointIn(ok, x, y)) return 2;
    return 0;
}

ClipboardBubble::TagEditorLayout ClipboardBubble::BuildTagEditorLayout() const
{
    /* 布局对齐 SnowAir 的「自定义标签」弹窗：
       标题 + 一条内联输入框（标签 chip 与输入光标同一行、自动换行）+ 右下角取消/确定。 */
    const float s = scale_;
    const float cardW = 320.0f * s;
    const float padX = 18.0f * s;
    const float padY = 16.0f * s;
    const float innerRight = cardW - padX;
    const float chipH = 20.0f * s;
    const float chipGap = 4.0f * s;
    const float labelH = 15.0f * s;
    const float buttonH = 30.0f * s;
    const float buttonW = 72.0f * s;
    const float fieldPadX = 6.0f * s;
    const float fieldPadY = 5.0f * s;
    const float editW = 96.0f * s;

    bool hasCats = false;
    for (const auto& category : categories_) {
        if (category.builtin < 0) {
            hasCats = true;
            break;
        }
    }

    TagEditorLayout layout;
    float y = padY;
    layout.title = D2D1::RectF(padX, y, innerRight, y + 20.0f * s);
    y = layout.title.bottom + 10.0f * s;

    if (hasCats) {
        layout.labelCats = D2D1::RectF(padX, y, innerRight, y + labelH);
        y = layout.labelCats.bottom + 4.0f * s;
        float x = padX;
        float rowTop = y;
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            const Category& category = categories_[static_cast<size_t>(i)];
            if (category.builtin >= 0) continue;
            const float w = TextWidth(category.name, chipFormat_.Get()) + 10.0f * s;
            if (x + w > innerRight) {
                x = padX;
                rowTop += chipH + chipGap;
            }
            TagChipRect chip;
            chip.rect = D2D1::RectF(x, rowTop, x + w, rowTop + chipH);
            chip.text = category.name;
            chip.index = i;
            layout.catChips.push_back(std::move(chip));
            x += w + chipGap;
        }
        y = rowTop + chipH + 10.0f * s;
    }

    layout.labelTags = D2D1::RectF(padX, y, innerRight, y + labelH);
    const float fieldTop = layout.labelTags.bottom + 4.0f * s;
    float x = padX + fieldPadX;
    float rowTop = fieldTop + fieldPadY;
    for (const auto& tag : tagSelection_) {
        const float w = TextWidth(tag, chipFormat_.Get()) + 10.0f * s;
        if (x + w > innerRight - fieldPadX) {
            x = padX + fieldPadX;
            rowTop += chipH + chipGap;
        }
        TagChipRect chip;
        chip.rect = D2D1::RectF(x, rowTop, x + w, rowTop + chipH);
        chip.text = tag;
        layout.tagChips.push_back(std::move(chip));
        x += w + chipGap;
    }
    if (x + editW > innerRight - fieldPadX) {
        x = padX + fieldPadX;
        rowTop += chipH + chipGap;
    }
    layout.edit = D2D1::RectF(x, rowTop, x + editW, rowTop + chipH);
    const float fieldBottom = rowTop + chipH + fieldPadY;
    layout.field = D2D1::RectF(padX, fieldTop, innerRight, fieldBottom);

    y = fieldBottom + 10.0f * s;
    layout.save = D2D1::RectF(innerRight - buttonW, y, innerRight, y + buttonH);
    layout.cancel = D2D1::RectF(layout.save.left - 8.0f * s - buttonW, y,
                                layout.save.left - 8.0f * s, y + buttonH);
    y += buttonH;
    const float cardH = y + padY;

    const float left = (width_ - cardW) * 0.5f;
    const float top = (height_ - cardH) * 0.5f;
    auto shift = [&](D2D1_RECT_F& r) {
        r.left += left;
        r.right += left;
        r.top += top;
        r.bottom += top;
    };
    shift(layout.title);
    shift(layout.labelCats);
    shift(layout.labelTags);
    shift(layout.field);
    shift(layout.edit);
    shift(layout.cancel);
    shift(layout.save);
    for (auto& chip : layout.catChips) shift(chip.rect);
    for (auto& chip : layout.tagChips) shift(chip.rect);
    layout.card = D2D1::RectF(left, top, left + cardW, top + cardH);
    return layout;
}

std::wstring ClipboardBubble::TooltipAt(float x, float y, D2D1_RECT_F* anchor) const
{
    const float s = scale_;
    if (anchor) *anchor = D2D1::RectF(x, y, x, y);
    /* 弹窗按钮都写着取消/确定/完成/删除，不再额外弹提示。 */
    if (noteEditorOpen_ || categoryEditorOpen_ || deleteConfirm_ >= 0) {
        return L"";
    }
    if (tagEditorOpen_) {
        const TagEditorLayout layout = BuildTagEditorLayout();
        for (const auto& chip : layout.catChips) {
            if (rack::PointIn(chip.rect, x, y)) {
                if (anchor) *anchor = chip.rect;
                return L"点选分类";
            }
        }
        for (const auto& chip : layout.tagChips) {
            if (rack::PointIn(chip.rect, x, y)) {
                if (anchor) *anchor = chip.rect;
                return L"移除标签";
            }
        }
        return L"";
    }
    const D2D1_RECT_F plus =
        D2D1::RectF(width_ - (kPad + kPlusW) * s, SearchTop(), width_ - kPad * s,
                    SearchTop() + kSearchH * s);
    const float gearRight = width_ - (kPad + 2.0f * kPlusW + kPlusGap) * s;
    const D2D1_RECT_F gear = D2D1::RectF(gearRight, SearchTop(), gearRight + kPlusW * s,
                                         SearchTop() + kSearchH * s);
    if (settingsOpen_) {
        if (HitPlus(x, y)) {
            if (anchor) *anchor = plus;
            return L"新建便签";
        }
        if (HitGear(x, y)) {
            if (anchor) *anchor = gear;
            return L"退出分类设置";
        }
        const int row = ManagerRowAt(x, y);
        if (row >= 0 && row < static_cast<int>(categories_.size()) &&
            categories_[static_cast<size_t>(row)].builtin < 0) {
            const D2D1_RECT_F rr = ManagerRowRect(row);
            const float cx = rr.right - 4.0f * s;
            const float cy = rr.top + 4.0f * s;
            const float dx = x - cx;
            const float dy = y - cy;
            if (dx * dx + dy * dy <= 100.0f * s * s) {
                if (anchor) *anchor = D2D1::RectF(cx - 10.0f * s, cy - 10.0f * s,
                                                  cx + 10.0f * s, cy + 10.0f * s);
                return L"删除分类";
            }
        }
        return L"";
    }
    if (HitPlus(x, y)) {
        if (anchor) *anchor = plus;
        return L"新建便签";
    }
    if (HitGear(x, y)) {
        if (anchor) *anchor = gear;
        return L"分类设置";
    }
    const int row = HitRow(x, y);
    if (row >= 0) {
        const int action = HitAction(x, y, row);
        if (action >= 0 && row < static_cast<int>(shown_.size())) {
            const Entry& entry =
                entries_[static_cast<size_t>(shown_[static_cast<size_t>(row)])];
            const std::vector<int> actions = ActionsFor(entry);
            if (action < static_cast<int>(actions.size())) {
                const float top = RowTop(row) + kCardHeaderTop * s +
                                  (kCardHeaderH - kActionSize) * 0.5f * s;
                const float right = width_ - kPad * s - kCardPadX * s;
                const int count = static_cast<int>(actions.size());
                const float groupW =
                    count * kActionSize * s + (count - 1) * kActionGap * s;
                const float bx =
                    right - groupW + action * (kActionSize + kActionGap) * s;
                if (anchor) *anchor = D2D1::RectF(bx, top, bx + kActionSize * s,
                                                  top + kActionSize * s);
                switch (actions[static_cast<size_t>(action)]) {
                case 0: return L"复制";
                case 1: return L"在文件夹中显示";
                case 2: return L"挂到绳子上";
                case 3: return L"删除";
                default: break;
                }
            }
        }
        const int tagIndex = HitTagChip(x, y, row);
        if (tagIndex >= 0) {
            const Entry& entry =
                entries_[static_cast<size_t>(shown_[static_cast<size_t>(row)])];
            const float bottom =
                RowTop(row) + rowHeights_[static_cast<size_t>(row)] - kCardPadY * s;
            const float top = bottom - kCardTagH * s;
            float cx = kPad * s + kCardPadX * s;
            const float typeW = TextWidth(TypeTagFor(entry), tagFormat_.Get()) + 8.0f * s;
            if (tagIndex == 0 && anchor) {
                *anchor = D2D1::RectF(cx, top, cx + typeW, bottom);
            }
            cx += typeW + 6.0f * s;
            for (int i = 0; i < static_cast<int>(entry.tags.size()); ++i) {
                const float w =
                    TextWidth(entry.tags[static_cast<size_t>(i)], tagFormat_.Get()) +
                    8.0f * s;
                if (tagIndex == i + 1 && anchor) {
                    *anchor = D2D1::RectF(cx, top, cx + w, bottom);
                }
                cx += w + 6.0f * s;
            }
            if (tagIndex == static_cast<int>(entry.tags.size()) + 1 && anchor) {
                const float plusW = 16.0f * s;
                *anchor = D2D1::RectF(cx, top, cx + plusW, bottom);
            }
            return L"新增标签";
        }
    }
    return L"";
}

void ClipboardBubble::ClearHover()
{
    hovered_ = -1;
    hoveredAction_ = -1;
    hoveredChip_ = -1;
    hoveredTag_ = -1;
    hoveredPlus_ = false;
    hoveredModalButton_ = 0;
    settingsHover_ = -1;
    hoveredSettingsAdd_ = false;
}

D2D1_RECT_F ClipboardBubble::SettingsCardRect() const
{
    const float s = scale_;
    const float w = 340.0f * s;
    const float h = 430.0f * s;
    return D2D1::RectF((width_ - w) * 0.5f, (height_ - h) * 0.5f,
                       (width_ + w) * 0.5f, (height_ + h) * 0.5f);
}

D2D1_RECT_F ClipboardBubble::SettingsRowRect(int row) const
{
    const float s = scale_;
    const D2D1_RECT_F card = SettingsCardRect();
    const float top = card.top + 46.0f * s + static_cast<float>(row) * 44.0f * s;
    return D2D1::RectF(card.left + 14.0f * s, top, card.right - 14.0f * s, top + 38.0f * s);
}

int ClipboardBubble::SettingsRowAt(float x, float y) const
{
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        if (rack::PointIn(SettingsRowRect(i), x, y)) return i;
    }
    return -1;
}

void ClipboardBubble::OpenSettings()
{
    settingsOpen_ = true;
    settingsDrag_ = -1;
    settingsHover_ = -1;
    hoveredGear_ = true;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    ActivateForInput();
    Render();
}

void ClipboardBubble::CloseSettings()
{
    settingsOpen_ = false;
    settingsDrag_ = -1;
    if (edit_) ShowWindow(edit_, SW_SHOWNOACTIVATE);

    LayoutEditors();
    Render();
    SetFocus(hwnd());
}

void ClipboardBubble::ToggleCategoryHidden(int index)
{
    if (index < 0 || index >= static_cast<int>(categories_.size())) return;
    categories_[static_cast<size_t>(index)].hidden = !categories_[static_cast<size_t>(index)].hidden;
    SaveCategories();
    RefreshVisibleCategories();
    RebuildShown();
    Reload(true);
}

void ClipboardBubble::DeleteCategory(int index)
{
    if (index < 0 || index >= static_cast<int>(categories_.size())) return;
    if (categories_[static_cast<size_t>(index)].builtin >= 0) return;
    const std::wstring name = categories_[static_cast<size_t>(index)].name;
    categories_.erase(categories_.begin() + index);
    for (auto& kv : assignments_) {
        auto& names = kv.second;
        names.erase(std::remove(names.begin(), names.end(), name), names.end());
    }
    SaveCategories();
    RefreshVisibleCategories();
    RebuildShown();
    Reload(true);
}

void ClipboardBubble::OpenRenameCategory(int index)
{
    if (index < 0 || index >= static_cast<int>(categories_.size())) return;
    if (categories_[static_cast<size_t>(index)].builtin >= 0) return;
    settingsOpen_ = false;
    renamingCategory_ = index;
    categoryEditorOpen_ = true;
    tagEditorOpen_ = false;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    CreateModalEdit();
    if (modalEdit_) {
        SetWindowTextW(modalEdit_, categories_[static_cast<size_t>(index)].name.c_str());
        ActivateForInput();
        SetFocus(modalEdit_);
        SendMessageW(modalEdit_, EM_SETSEL, 0, -1);
    }
    LayoutEditors();
    Render();
}

void ClipboardBubble::PaintSettings(ID2D1RenderTarget* target, const Brushes& b)
{
    const float s = scale_;
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.0f, kArrowH * s, width_, height_), 10.0f * s,
                          10.0f * s),
        b.dim.Get());
    const D2D1_RECT_F card = SettingsCardRect();
    target->FillRoundedRectangle(D2D1::RoundedRect(card, 16.0f * s, 16.0f * s),
                                 b.card.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(card, 16.0f * s, 16.0f * s),
                                 b.border.Get(), 1.0f * s);
    if (titleFormat_) {
        target->DrawText(L"分类设置", 4, titleFormat_.Get(),
                         D2D1::RectF(card.left + 16.0f * s, card.top + 10.0f * s,
                                     card.right - 16.0f * s, card.top + 32.0f * s),
                         b.fg.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        const D2D1_RECT_F row = SettingsRowRect(i);
        if (row.bottom > card.bottom - 54.0f * s) break;
        const bool hidden = categories_[static_cast<size_t>(i)].hidden;
        const bool custom = categories_[static_cast<size_t>(i)].builtin < 0;
        target->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f * s, 8.0f * s),
                                     b.secondary.Get());
        if (iconDrag_) {
            DrawGlyph(target, iconDrag_.Get(),
                      D2D1::Point2F(row.left + 14.0f * s, (row.top + row.bottom) * 0.5f),
                      12.0f * s, b.muted.Get());
        }
        if (textFormat_) {
            target->DrawText(categories_[static_cast<size_t>(i)].name.c_str(),
                             static_cast<UINT32>(categories_[static_cast<size_t>(i)].name.size()),
                             textFormat_.Get(),
                             D2D1::RectF(row.left + 30.0f * s, row.top,
                                         row.right - 112.0f * s, row.bottom),
                             hidden ? b.muted.Get() : b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        /* toggle */
        const D2D1_RECT_F track = D2D1::RectF(row.right - 106.0f * s,
                                              (row.top + row.bottom) * 0.5f - 9.0f * s,
                                              row.right - 72.0f * s,
                                              (row.top + row.bottom) * 0.5f + 9.0f * s);
        target->FillRoundedRectangle(D2D1::RoundedRect(track, 9.0f * s, 9.0f * s),
                                     hidden ? b.secondaryFg.Get() : b.success.Get());
        const float knobX = hidden ? track.left + 7.0f * s : track.right - 7.0f * s;
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, (track.top + track.bottom) * 0.5f),
                                          7.0f * s, 7.0f * s),
                            b.card.Get());
        if (custom) {
            if (buttonFormat_) {
                target->DrawText(L"改", 1, buttonFormat_.Get(),
                                 D2D1::RectF(row.right - 66.0f * s, row.top,
                                             row.right - 44.0f * s, row.bottom),
                                 b.secondaryFg.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (iconDelete_) {
                DrawGlyph(target, iconDelete_.Get(),
                          D2D1::Point2F(row.right - 26.0f * s, (row.top + row.bottom) * 0.5f),
                          12.0f * s, b.secondaryFg.Get());
            }
        }
    }
    if (buttonFormat_) {
        const D2D1_RECT_F done = D2D1::RectF(card.right - 84.0f * s,
                                             card.bottom - 42.0f * s,
                                             card.right - 14.0f * s,
                                             card.bottom - 12.0f * s);
        target->FillRoundedRectangle(D2D1::RoundedRect(done, 8.0f * s, 8.0f * s),
                                     b.primary.Get());
        target->DrawText(L"完成", 2, buttonFormat_.Get(), done, b.primaryFg.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

D2D1_RECT_F ClipboardBubble::SettingsAddRect() const
{
    const float s = scale_;
    const D2D1_RECT_F card = ManagerCardRect(false);
    const float h = 34.0f * s;
    return D2D1::RectF(card.left + 6.0f * s, card.bottom - 6.0f * s - h,
                       card.right - 6.0f * s, card.bottom - 6.0f * s);
}

D2D1_RECT_F ClipboardBubble::ManagerCardRect(bool hidden) const
{
    const float s = scale_;
    const float top = SearchTop() + kSearchH * s + 10.0f * s;
    const float bottom = height_ - kPad * s;
    const float gap = 10.0f * s;
    const float total = bottom - top - gap;
    const float topH = total * 0.4f;
    return hidden ? D2D1::RectF(kPad * s, top + topH + gap, width_ - kPad * s, bottom)
                  : D2D1::RectF(kPad * s, top, width_ - kPad * s, top + topH);
}

D2D1_RECT_F ClipboardBubble::ManagerRowRect(int index) const
{
    if (index < 0 || index >= static_cast<int>(categories_.size())) return D2D1::RectF();
    if (categories_[static_cast<size_t>(index)].builtin == 0) return D2D1::RectF();
    const float s = scale_;
    const bool hidden = categories_[static_cast<size_t>(index)].hidden;
    const D2D1_RECT_F card = ManagerCardRect(hidden);
    const float pad = 6.0f * s;
    const float gap = 6.0f * s;
    const float tileH = 34.0f * s;
    /* 可见分类卡底部留给「+ 新建分类」按钮。 */
    const float reserve = hidden ? 0.0f : 44.0f * s;
    float x = card.left + pad;
    float y = card.top + pad;
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        const Category& category = categories_[static_cast<size_t>(i)];
        if (category.builtin == 0) continue;
        if (category.hidden != hidden) continue;
        const float w = TextWidth(category.name, chipFormat_.Get()) + 24.0f * s;
        if (x + w > card.right - pad) {
            x = card.left + pad;
            y += tileH + gap;
        }
        if (y + tileH > card.bottom - pad - reserve) return D2D1::RectF();
        if (i == index) return D2D1::RectF(x, y, x + w, y + tileH);
        x += w + gap;
    }
    return D2D1::RectF();
}

int ClipboardBubble::ManagerDropAt(float x, float y, bool* after) const
{
    if (after) *after = false;
    const D2D1_RECT_F bottomCard = ManagerCardRect(true);
    const bool hidden = y >= bottomCard.top - 6.0f * scale_;
    int best = -1;
    int last = -1;
    float bestDist = 1e9f;
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        const Category& category = categories_[static_cast<size_t>(i)];
        if (category.builtin == 0) continue;
        if (category.hidden != hidden) continue;
        const D2D1_RECT_F row = ManagerRowRect(i);
        if (row.right <= row.left) continue;
        const float cx = (row.left + row.right) * 0.5f;
        const float cy = (row.top + row.bottom) * 0.5f;
        const float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
        last = i;
    }
    if (last >= 0 && after) {
        const D2D1_RECT_F tail = ManagerRowRect(last);
        if (tail.right > tail.left && (x > tail.right - 8.0f * scale_ ||
                                       y > tail.bottom - 8.0f * scale_)) {
            *after = true;
            return last;
        }
    }
    return best;
}

int ClipboardBubble::ManagerRowAt(float x, float y) const
{
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        const D2D1_RECT_F row = ManagerRowRect(i);
        if (row.right <= row.left) continue;
        if (rack::PointIn(row, x, y)) return i;
    }
    return -1;
}

bool ClipboardBubble::CategoryHasItems(int index) const
{
    if (index < 0 || index >= static_cast<int>(categories_.size())) return false;
    const std::wstring& name = categories_[static_cast<size_t>(index)].name;
    for (const auto& kv : assignments_) {
        for (const auto& assigned : kv.second) {
            if (assigned == name) return true;
        }
    }
    return false;
}

void ClipboardBubble::PaintCategoryManager(ID2D1RenderTarget* target, const Brushes& b)
{
    const float s = scale_;
    for (int cardIndex = 0; cardIndex < 2; ++cardIndex) {
        const bool hiddenCard = cardIndex == 1;
        const D2D1_RECT_F card = ManagerCardRect(hiddenCard);
        target->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f * s, 8.0f * s),
                                     b.cardBg.Get());
    }
    bool anyHidden = false;
    for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
        const Category& category = categories_[static_cast<size_t>(i)];
        if (category.builtin == 0) continue;
        if (category.hidden) anyHidden = true;
        const D2D1_RECT_F row = ManagerRowRect(i);
        if (row.right <= row.left) continue;
        const bool custom = category.builtin < 0;
        const bool active = i == settingsHover_;
        /* 深色：行用 #262626、卡片 #1A1A1A；浅色对应地行用白色、卡片 #F5F4F4，
           让分类行和卡片底色拉开一层。 */
        ID2D1Brush* rowFill =
            active ? b.hoverBg.Get()
                   : (theme_.dark ? b.secondary.Get() : b.card.Get());
        target->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f * s, 8.0f * s), rowFill);
        if (chipFormat_) {
            target->DrawText(category.name.c_str(),
                             static_cast<UINT32>(category.name.size()), chipFormat_.Get(),
                             D2D1::RectF(row.left + 6.0f * s, row.top,
                                         row.right - 6.0f * s, row.bottom),
                             category.hidden ? b.muted.Get() : b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (custom && i == settingsHover_) {
            const float bx = row.right - 4.0f * s;
            const float by = row.top + 4.0f * s;
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), 8.0f * s, 8.0f * s),
                                b.danger.Get());
            target->DrawLine(D2D1::Point2F(bx - 3.0f * s, by - 3.0f * s),
                             D2D1::Point2F(bx + 3.0f * s, by + 3.0f * s),
                             b.onDanger.Get(), 1.4f * s);
            target->DrawLine(D2D1::Point2F(bx - 3.0f * s, by + 3.0f * s),
                             D2D1::Point2F(bx + 3.0f * s, by - 3.0f * s),
                             b.onDanger.Get(), 1.4f * s);
        }
    }
    if (settingsDrag_ >= 0 && settingsDrag_ < static_cast<int>(categories_.size())) {
        D2D1_RECT_F slot = settingsDrop_ >= 0 ? ManagerRowRect(settingsDrop_)
                                                  : D2D1::RectF();
        if (slot.right <= slot.left) {
            const D2D1_RECT_F card = ManagerCardRect(settingsDropHidden_);
            slot = D2D1::RectF(card.left + 6.0f * s, card.top + 6.0f * s,
                               card.left + 66.0f * s, card.top + 40.0f * s);
        }
        if (slot.right > slot.left) {
            float barX = settingsDropAfter_ ? slot.right + 1.0f * s : slot.left - 3.0f * s;
            const D2D1_RECT_F bar = D2D1::RectF(barX, slot.top + 3.0f * s, barX + 2.0f * s,
                                                slot.top + 31.0f * s);
            target->FillRoundedRectangle(D2D1::RoundedRect(bar, 6.0f * s, 6.0f * s),
                                         b.insertion.Get());
        }
        float w = slot.right - slot.left;
        float h = slot.bottom - slot.top;
        if (w <= 0.0f) { w = 110.0f * s; h = 34.0f * s; }
        const D2D1_RECT_F ghost = D2D1::RectF(dragX_ - w * 0.5f, dragY_ - h * 0.5f,
                                              dragX_ + w * 0.5f, dragY_ + h * 0.5f);
        target->FillRoundedRectangle(D2D1::RoundedRect(ghost, 8.0f * s, 8.0f * s),
                                     b.plusBg.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(ghost, 8.0f * s, 8.0f * s),
                                     b.plusHover.Get(), 1.0f * s);
        if (chipFormat_) {
            const std::wstring& name = categories_[static_cast<size_t>(settingsDrag_)].name;
            target->DrawText(name.c_str(), static_cast<UINT32>(name.size()),
                             chipFormat_.Get(), ghost, b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    {
        const D2D1_RECT_F add = SettingsAddRect();
        target->FillRoundedRectangle(
            D2D1::RoundedRect(add, 8.0f * s, 8.0f * s),
            hoveredSettingsAdd_ ? b.hoverBg.Get() : b.card.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(add, 8.0f * s, 8.0f * s),
                                     b.border.Get(), 1.0f * s);
        const float cx = add.left + 16.0f * s;
        const float cy = (add.top + add.bottom) * 0.5f;
        ID2D1Brush* ink = hoveredSettingsAdd_ ? b.fg.Get() : b.secondaryFg.Get();
        target->DrawLine(D2D1::Point2F(cx - 5.0f * s, cy), D2D1::Point2F(cx + 5.0f * s, cy),
                         ink, 1.4f * s);
        target->DrawLine(D2D1::Point2F(cx, cy - 5.0f * s), D2D1::Point2F(cx, cy + 5.0f * s),
                         ink, 1.4f * s);
        if (buttonFormat_) {
            target->DrawText(L"新建分类", 4, buttonFormat_.Get(),
                             D2D1::RectF(add.left + 30.0f * s, add.top, add.right, add.bottom),
                             ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (!anyHidden) {
        const D2D1_RECT_F card = ManagerCardRect(true);
        if (bigHintFormat_) {
            const wchar_t* hint = L"拖拽排序，拖入此处隐藏；双击自定义分类可重命名";
            target->DrawText(hint, static_cast<UINT32>(wcslen(hint)), bigHintFormat_.Get(), card,
                             b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

std::vector<int> ClipboardBubble::ActionsFor(const Entry& entry) const
{
    std::vector<int> actions;
    actions.push_back(0); /* copy */
    const bool reveal = entry.kind == SNOWAIR_ENTRY_FILES ||
                        entry.kind == SNOWAIR_ENTRY_MEDIA ||
                        (entry.kind == SNOWAIR_ENTRY_IMAGE &&
                         entry.source != SNOWAIR_SOURCE_CLIPBOARD);
    if (reveal) actions.push_back(1);
    /* 所有类型都能上绳：文字/图片/录屏/文件各有自己的卡片形态。 */
    actions.push_back(2);
    actions.push_back(3); /* delete */
    return actions;
}

int ClipboardBubble::HitAction(float x, float y, int row) const
{
    if (row < 0 || row >= static_cast<int>(shown_.size())) return -1;
    const Entry& entry = entries_[static_cast<size_t>(shown_[static_cast<size_t>(row)])];
    const std::vector<int> actions = ActionsFor(entry);
    if (actions.empty()) return -1;
    const float s = scale_;
    const float top = RowTop(row) + kCardHeaderTop * s + (kCardHeaderH - kActionSize) * 0.5f * s;
    const float right = width_ - kPad * s - kCardPadX * s;
    const int count = static_cast<int>(actions.size());
    const float groupW = count * kActionSize * s + (count - 1) * kActionGap * s;
    const float start = right - groupW;
    if (y < top || y > top + kActionSize * s) return -1;
    for (int i = 0; i < count; ++i) {
        const float bx = start + i * (kActionSize + kActionGap) * s;
        if (x >= bx && x <= bx + kActionSize * s) return i;
    }
    return -1;
}

int ClipboardBubble::HitTagChip(float x, float y, int row) const
{
    if (row < 0 || row >= static_cast<int>(shown_.size())) return -1;
    const Entry& entry = entries_[static_cast<size_t>(shown_[static_cast<size_t>(row)])];
    const float s = scale_;
    const float bottom = RowTop(row) + rowHeights_[static_cast<size_t>(row)] - kCardPadY * s;
    const float top = bottom - kCardTagH * s;
    if (y < top || y > bottom) return -1;
    float cx = kPad * s + kCardPadX * s;
    const float right = width_ - kPad * s - kCardPadX * s;
    const float plusW = 16.0f * s;
    const float tagLimit = right - plusW - 6.0f * s;
    const std::wstring typeTag = TypeTagFor(entry);
    float w = TextWidth(typeTag, tagFormat_.Get()) + 8.0f * s;
    if (cx + w > tagLimit) return -1;
    if (x >= cx && x <= cx + w) return 0;
    cx += w + 6.0f * s;
    for (int i = 0; i < static_cast<int>(entry.tags.size()); ++i) {
        w = TextWidth(entry.tags[static_cast<size_t>(i)], tagFormat_.Get()) + 8.0f * s;
        if (cx + w > tagLimit) return -1;
        if (x >= cx && x <= cx + w) return i + 1;
        cx += w + 6.0f * s;
    }
    if (cx + plusW <= right && x >= cx && x <= cx + plusW) {
        return static_cast<int>(entry.tags.size()) + 1;
    }
    return -1;
}

std::wstring ClipboardBubble::TypeTagFor(const Entry& entry) const
{
    switch (entry.kind) {
    case SNOWAIR_ENTRY_TEXT:
        return L"文字";
    case SNOWAIR_ENTRY_MEDIA:
        return L"录屏";
    case SNOWAIR_ENTRY_FILES:
        return L"文件";
    default:
        return entry.source == SNOWAIR_SOURCE_CAPTURE ? L"截图" : L"图片";
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

void ClipboardBubble::UpdateSearchEditVisibility()
{
    if (!edit_) return;
    const bool modal = noteEditorOpen_ || categoryEditorOpen_ || tagEditorOpen_ ||
                       deleteConfirm_ >= 0;
    const bool show = !modal && !settingsOpen_;
    if (show == searchEditVisible_) return;
    searchEditVisible_ = show;
    ShowWindow(edit_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void ClipboardBubble::Paint(ID2D1RenderTarget* target)
{
    UpdateSearchEditVisibility();
    if (!target) return;
    const float s = scale_;
    const float w = width_;
    const float h = height_;

    Brushes b;
    auto make = [&](const D2D1_COLOR_F& color, ComPtr<ID2D1SolidColorBrush>& out) {
        target->CreateSolidColorBrush(color, out.GetAddressOf());
    };
    make(theme_.fg, b.fg);
    make(theme_.fgTertiary, b.muted);
    make(theme_.fgSecondary, b.secondaryFg);
    make(theme_.border, b.border);
    make(theme_.accent, b.accent);
    make(theme_.primary, b.primary);
    make(theme_.primaryFg, b.primaryFg);
    make(theme_.secondary, b.secondary);
    make(theme_.card, b.card);
    make(theme_.bg, b.panel);
    make(theme_.canvas.a > 0.01f ? theme_.canvas : theme_.bg, b.canvas);
    make(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f), b.dim);
    make(theme_.contentBorder.a > 0.01f ? theme_.contentBorder : theme_.border,
         b.contentBorder);
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xF5F4F4), b.chipBg);
    /* 搜索框是主输入区，比面板明显一档；右侧 +/齿轮是辅助按钮，浅色下压到接近面板。 */
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xEDEDED), b.searchBg);
    make(theme_.dark ? Rgb(0x3A3A3A) : Rgb(0xF5F4F4), b.iconBg);
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xE5E5E5), b.iconHover);
    /* 分类设置处于选中态时常驻一个更明显的底，图标同时变绿。 */
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xE5E5E5), b.iconSelected);
    make(theme_.dark ? Rgb(0x3A3A3A) : Rgb(0xE5E5E5), b.hoverBg);
    /* 深色：面板 #262626、卡片 #1A1A1A，靠深浅差分层；浅色对应地用
       比白面板深一档的灰（#F5F4F4）填充卡片，而不是和面板同色只剩描边。 */
    make(theme_.dark ? Rgb(0x1A1A1A) : Rgb(0xF5F4F4), b.cardBg);
    make(theme_.dark ? Rgb(0x3A3A3A) : Rgb(0xE5E5E5), b.tagBg);
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xD4D4D4), b.tagHover);
    make(theme_.dark ? Rgb(0x3A3A3A) : Rgb(0xE5E5E5), b.plusBg);
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xD4D4D4), b.plusHover);
    make(theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xE5E5E5), b.selectedBg);
    make(Rgb(0x34C759), b.success);
    make(Rgb(0xEF4444), b.danger);
    auto lighten = [](D2D1_COLOR_F color, float delta) {
        color.r = std::min(1.0f, color.r + delta);
        color.g = std::min(1.0f, color.g + delta);
        color.b = std::min(1.0f, color.b + delta);
        return color;
    };
    /* 弹窗实心按钮的悬停色：在本体色上提亮一点，避免再用一套颜色。 */
    make(lighten(theme_.primary, 0.09f), b.primaryHover);
    make(lighten(Rgb(0xEF4444), 0.09f), b.dangerHover);
    /* 弹窗（新建/重命名/删除分类）用独立底色，和面板拉开层次。 */
    make(theme_.dark ? Rgb(0x262626) : Rgb(0xFFFFFF), b.modalCard);
    make(theme_.dark ? Rgb(0x4A4A4A) : theme_.border, b.modalBorder);
    make(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f), b.modalShadow);
    make(Rgb(0x34C759), b.insertion);
    make(theme_.dark ? Rgb(0x333333) : Rgb(0xEFEFEF), b.disabledBg);
    make(Rgb(0xFFFFFF), b.onDanger);

    const float oy = kArrowH * s;
    if (graphics_ && graphics_->d2d()) {
        ComPtr<ID2D1PathGeometry> shape;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(shape.GetAddressOf())) && shape) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(shape->Open(sink.GetAddressOf())) && sink) {
                const float r = 16.0f * s;
                const float left = 1.0f * s;
                const float right = w - 1.0f * s;
                const float top = oy + 1.0f * s;
                const float bottom = h - 1.0f * s;
                const float half = 8.0f * s;
                const float apexY = 0.5f * s;
                auto arc = [&](D2D1_POINT_2F to) {
                    D2D1_ARC_SEGMENT segment{};
                    segment.point = to;
                    segment.size = D2D1::SizeF(r, r);
                    segment.rotationAngle = 0.0f;
                    segment.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
                    segment.arcSize = D2D1_ARC_SIZE_SMALL;
                    sink->AddArc(segment);
                };
                sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                sink->BeginFigure(D2D1::Point2F(left + r, top), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(anchorX_ - half, top));
                sink->AddLine(D2D1::Point2F(anchorX_, apexY));
                sink->AddLine(D2D1::Point2F(anchorX_ + half, top));
                sink->AddLine(D2D1::Point2F(right - r, top));
                arc(D2D1::Point2F(right, top + r));
                sink->AddLine(D2D1::Point2F(right, bottom - r));
                arc(D2D1::Point2F(right - r, bottom));
                sink->AddLine(D2D1::Point2F(left + r, bottom));
                arc(D2D1::Point2F(left, bottom - r));
                sink->AddLine(D2D1::Point2F(left, top + r));
                arc(D2D1::Point2F(left + r, top));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->FillGeometry(shape.Get(), b.panel.Get());
                target->DrawGeometry(shape.Get(), b.border.Get(), 1.0f * s);
            }
        }
    }

    PaintHeader(target, b);
    if (settingsOpen_) {
        PaintCategoryManager(target, b);
    } else {
        target->PushAxisAlignedClip(
            D2D1::RectF(0.0f, ListTop() - 2.0f * s, w, h - kPad * s),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        PaintCards(target, b);
        target->PopAxisAlignedClip();
        if (shown_.empty()) PaintEmptyState(target, b);
    }
    PaintModals(target, b);

    if (!tooltipText_.empty() && chipFormat_) {
        const float padX = 8.0f * s;
        const float tipH = 22.0f * s;
        const float triH = 6.0f * s;
        const float triHalf = 5.5f * s;
        const float gap = 4.0f * s;
        const float tipW = TextWidth(tooltipText_, chipFormat_.Get()) + 2.0f * padX;
        const D2D1_RECT_F anchor = tooltipAnchor_;
        const float centerX = (anchor.left + anchor.right) * 0.5f;
        float x = rack::Clamp(centerX - tipW * 0.5f, 4.0f * s,
                              std::max(4.0f * s, width_ - tipW - 4.0f * s));
        const bool above = anchor.top - gap - triH - tipH >= 4.0f * s;
        float y = above ? anchor.top - gap - triH - tipH : anchor.bottom + gap + triH;
        y = rack::Clamp(y, 4.0f * s,
                        std::max(4.0f * s, height_ - tipH - triH - 4.0f * s));
        const float margin = 6.0f * s + triHalf + 2.0f * s;
        float apexX = centerX;
        if (x + margin <= x + tipW - margin) {
            apexX = rack::Clamp(centerX, x + margin, x + tipW - margin);
        } else {
            apexX = x + tipW * 0.5f;
        }
        const float l = x;
        const float t = y;
        const float r = x + tipW;
        const float b = y + tipH;
        const float rad = 6.0f * s;

        ComPtr<ID2D1SolidColorBrush> fill;
        ComPtr<ID2D1SolidColorBrush> edge;
        ComPtr<ID2D1SolidColorBrush> ink;
        if (FAILED(target->CreateSolidColorBrush(
                theme_.dark ? D2D1::ColorF(0.149f, 0.149f, 0.149f)
                            : D2D1::ColorF(1.0f, 1.0f, 1.0f),
                fill.GetAddressOf())) ||
            FAILED(target->CreateSolidColorBrush(theme_.border, edge.GetAddressOf())) ||
            FAILED(target->CreateSolidColorBrush(theme_.fg, ink.GetAddressOf()))) {
            return;
        }

        /* 气泡和三角合成一条路径，再统一描边，避免三角底座多出一条横线。 */
        ComPtr<ID2D1PathGeometry> bubble;
        bool built = false;
        if (SUCCEEDED(graphics_->d2d()->CreatePathGeometry(bubble.GetAddressOf())) &&
            bubble) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(bubble->Open(sink.GetAddressOf())) && sink) {
                const auto arcTo = [&](float px, float py) {
                    D2D1_ARC_SEGMENT arc{};
                    arc.point = D2D1::Point2F(px, py);
                    arc.size = D2D1::SizeF(rad, rad);
                    arc.rotationAngle = 0.0f;
                    arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
                    arc.arcSize = D2D1_ARC_SIZE_SMALL;
                    sink->AddArc(arc);
                };
                sink->BeginFigure(D2D1::Point2F(l + rad, t), D2D1_FIGURE_BEGIN_FILLED);
                if (above) {
                    sink->AddLine(D2D1::Point2F(r - rad, t));
                    arcTo(r, t + rad);
                    sink->AddLine(D2D1::Point2F(r, b - rad));
                    arcTo(r - rad, b);
                    sink->AddLine(D2D1::Point2F(apexX + triHalf, b));
                    sink->AddLine(D2D1::Point2F(apexX, b + triH));
                    sink->AddLine(D2D1::Point2F(apexX - triHalf, b));
                    sink->AddLine(D2D1::Point2F(l + rad, b));
                    arcTo(l, b - rad);
                    sink->AddLine(D2D1::Point2F(l, t + rad));
                    arcTo(l + rad, t);
                } else {
                    sink->AddLine(D2D1::Point2F(apexX - triHalf, t));
                    sink->AddLine(D2D1::Point2F(apexX, t - triH));
                    sink->AddLine(D2D1::Point2F(apexX + triHalf, t));
                    sink->AddLine(D2D1::Point2F(r - rad, t));
                    arcTo(r, t + rad);
                    sink->AddLine(D2D1::Point2F(r, b - rad));
                    arcTo(r - rad, b);
                    sink->AddLine(D2D1::Point2F(l + rad, b));
                    arcTo(l, b - rad);
                    sink->AddLine(D2D1::Point2F(l, t + rad));
                    arcTo(l + rad, t);
                }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                built = true;
            }
        }
        if (built) {
            target->FillGeometry(bubble.Get(), fill.Get());
            target->DrawGeometry(bubble.Get(), edge.Get(), 1.0f * s);
        } else {
            const D2D1_ROUNDED_RECT rr =
                D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad);
            target->FillRoundedRectangle(rr, fill.Get());
            target->DrawRoundedRectangle(rr, edge.Get(), 1.0f * s);
        }

        target->DrawText(tooltipText_.c_str(),
                         static_cast<UINT32>(tooltipText_.size()), chipFormat_.Get(),
                         D2D1::RectF(l, t, r, b), ink.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void ClipboardBubble::PaintHeader(ID2D1RenderTarget* target, const Brushes& b)
{
    const float s = scale_;
    const float w = width_;
    const float searchTop = SearchTop();
    const float searchH = kSearchH * s;

    const D2D1_RECT_F searchRect = D2D1::RectF(
        kPad * s, searchTop,
        w - (kPad + 2.0f * kPlusW + 2.0f * kPlusGap) * s, searchTop + searchH);
    target->FillRoundedRectangle(D2D1::RoundedRect(searchRect, 8.0f * s, 8.0f * s),
                                 b.searchBg.Get());
    const bool modalOpen = noteEditorOpen_ || categoryEditorOpen_ || tagEditorOpen_ ||
                           deleteConfirm_ >= 0;
    if ((settingsOpen_ || modalOpen) && hintFormat_ && query_.empty()) {
        target->DrawText(L"请输入关键词...", 9, hintFormat_.Get(),
                         D2D1::RectF(searchRect.left + 10.0f * s, searchRect.top,
                                     searchRect.right - 10.0f * s, searchRect.bottom),
                         b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const D2D1_RECT_F plusRect = D2D1::RectF(
        w - (kPad + kPlusW) * s, searchTop, w - kPad * s, searchTop + searchH);
    target->FillRoundedRectangle(D2D1::RoundedRect(plusRect, 8.0f * s, 8.0f * s),
                                 hoveredPlus_ ? b.iconHover.Get() : b.iconBg.Get());

    const D2D1_RECT_F gearRect = D2D1::RectF(
        plusRect.left - kPlusGap * s - kPlusW * s, searchTop,
        plusRect.left - kPlusGap * s, searchTop + searchH);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(gearRect, 8.0f * s, 8.0f * s),
        settingsOpen_ ? b.iconSelected.Get()
                      : (hoveredGear_ ? b.iconHover.Get() : b.iconBg.Get()));
    {
        const D2D1_POINT_2F center = D2D1::Point2F(
            (gearRect.left + gearRect.right) * 0.5f, (gearRect.top + gearRect.bottom) * 0.5f);
        ID2D1Brush* ink = settingsOpen_ ? b.success.Get()
                                         : (hoveredGear_ ? b.fg.Get() : b.secondaryFg.Get());
        if (iconGear_) {
            DrawGlyph(target, iconGear_.Get(), center, 14.0f * s, ink);
        }
    }
    const float cx = (plusRect.left + plusRect.right) * 0.5f;
    const float cy = (plusRect.top + plusRect.bottom) * 0.5f;
    ID2D1Brush* plusInk = hoveredPlus_ ? b.fg.Get() : b.secondaryFg.Get();
    target->DrawLine(D2D1::Point2F(cx - 5.5f * s, cy), D2D1::Point2F(cx + 5.5f * s, cy),
                     plusInk, 1.4f * s);
    target->DrawLine(D2D1::Point2F(cx, cy - 5.5f * s), D2D1::Point2F(cx, cy + 5.5f * s),
                     plusInk, 1.4f * s);

    if (!settingsOpen_) {
        LayoutChips();
        const float left = kPad * s;
        const float right = w - kPad * s;
        target->PushAxisAlignedClip(D2D1::RectF(left, ChipsTop() - 2.0f * s, right,
                                                ChipsTop() + kChipH * s + 2.0f * s),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int i = 0; i < static_cast<int>(chipRects_.size()); ++i) {
        const D2D1_RECT_F rect = chipRects_[static_cast<size_t>(i)];
        const bool selected = i == filter_;
        const bool hovered = i == hoveredChip_;
        const wchar_t* label =
            (i >= 0 && i < static_cast<int>(visibleCats_.size()))
                ? categories_[static_cast<size_t>(visibleCats_[static_cast<size_t>(i)])].name.c_str()
                : L"";
        if (selected) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 8.0f * s, 8.0f * s),
                                         b.selectedBg.Get());
        } else if (hovered) {
            target->FillRoundedRectangle(D2D1::RoundedRect(rect, 8.0f * s, 8.0f * s),
                                         b.hoverBg.Get());
        }
        if (chipFormat_) {
            target->DrawText(label, static_cast<UINT32>(wcslen(label)), chipFormat_.Get(),
                             rect, b.fg.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    target->PopAxisAlignedClip();
    }
}

void ClipboardBubble::PaintCards(ID2D1RenderTarget* target, const Brushes& b)
{
    const float s = scale_;
    for (int row = 0; row < static_cast<int>(shown_.size()); ++row) {
        const float top = RowTop(row);
        const float height = rowHeights_[static_cast<size_t>(row)];
        if (top + height < ListTop() - 4.0f * s || top > height_ - kPad * s) continue;
        const Entry& entry = entries_[static_cast<size_t>(shown_[static_cast<size_t>(row)])];
        const D2D1_RECT_F card =
            D2D1::RectF(kPad * s, top, width_ - kPad * s, top + height);
        target->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f * s, 8.0f * s),
                                     b.cardBg.Get());

        const float headerTop = top + kCardHeaderTop * s;
        if (metaFormat_ && !entry.time.empty()) {
            // Draw the time with its line box pinned to the top: centering it in
            // the 22-DIP header leaves too much air above the text.
            const D2D1_RECT_F timeRect = D2D1::RectF(
                card.left + kCardPadX * s, headerTop - 2.0f * s,
                card.right - kCardPadX * s - 110.0f * s, headerTop + 16.0f * s);
            DrawTextLayoutBlock(graphics_ ? graphics_->dwrite() : nullptr, target,
                                entry.time, metaFormat_.Get(), timeRect, b.muted.Get(),
                                false);
        }

        if (row == hovered_) {
            const std::vector<int> actions = ActionsFor(entry);
            const int count = static_cast<int>(actions.size());
            const float right = card.right - kCardPadX * s;
            const float groupW = count * kActionSize * s + (count - 1) * kActionGap * s;
            const float start = right - groupW;
            const float iconTop =
                headerTop + (kCardHeaderH - kActionSize) * 0.5f * s;
            for (int i = 0; i < count; ++i) {
                const D2D1_RECT_F btn = D2D1::RectF(
                    start + i * (kActionSize + kActionGap) * s, iconTop,
                    start + i * (kActionSize + kActionGap) * s + kActionSize * s,
                    iconTop + kActionSize * s);
                target->FillRoundedRectangle(
                    D2D1::RoundedRect(btn, 6.0f * s, 6.0f * s),
                    i == hoveredAction_ ? b.plusHover.Get() : b.plusBg.Get());
                ID2D1PathGeometry* glyph = nullptr;
                switch (actions[static_cast<size_t>(i)]) {
                case 0: glyph = iconCopy_.Get(); break;
                case 1: glyph = iconFiles_.Get(); break;
                case 2: glyph = iconPeg_.Get(); break;
                default: glyph = iconDelete_.Get(); break;
                }
                const D2D1_POINT_2F center =
                    D2D1::Point2F((btn.left + btn.right) * 0.5f, (btn.top + btn.bottom) * 0.5f);
                if (glyph) {
                    DrawGlyph(target, glyph, center, 12.0f * s,
                              i == hoveredAction_ ? b.fg.Get() : b.muted.Get());
                }
            }
        }

        const float contentTop = headerTop + (kCardHeaderH + kCardHeaderGap) * s;
        const float contentLeft = card.left + kCardPadX * s;
        const float contentWidth = card.right - card.left - 2.0f * kCardPadX * s;
        if (entry.kind == SNOWAIR_ENTRY_TEXT) {
            const float contentH = height - (kCardHeaderTop + kCardHeaderH + kCardHeaderGap +
                                             kCardTagGap + kCardTagH + kCardPadY) * s;
            DrawTextLayoutBlock(
                graphics_ ? graphics_->dwrite() : nullptr, target, entry.label,
                textFormat_.Get(),
                D2D1::RectF(contentLeft, contentTop, contentLeft + contentWidth,
                            contentTop + std::max(contentH, 16.0f * s)),
                b.fg.Get(), true);
        } else if (entry.kind == SNOWAIR_ENTRY_IMAGE) {
            const std::wstring thumbPath = entry.thumb.empty() ? entry.path : entry.thumb;
            const D2D1_RECT_F thumbRect = D2D1::RectF(
                contentLeft, contentTop, contentLeft + kThumbW * s, contentTop + kThumbH * s);
            target->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 6.0f * s, 6.0f * s),
                                         b.accent.Get());
            ImageBitmap* bitmap = nullptr;
            if (!thumbPath.empty() && graphics_) {
                auto it = thumbs_.find(thumbPath);
                if (it == thumbs_.end()) {
                    ImageBitmap loaded;
                    graphics_->images().Load(thumbPath, 160.0f * s, target, loaded);
                    it = thumbs_.emplace(thumbPath, std::move(loaded)).first;
                }
                if (it->second.valid()) bitmap = &it->second;
            }
            if (bitmap) {
                ComPtr<ID2D1RoundedRectangleGeometry> clip;
                if (SUCCEEDED(graphics_->d2d()->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(thumbRect, 6.0f * s, 6.0f * s),
                        clip.GetAddressOf())) &&
                    clip) {
                    target->PushLayer(
                        D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
                }
                const UINT iw = bitmap->width;
                const UINT ih = bitmap->height;
                float cropW = std::min(static_cast<float>(iw), ih * 2.0f);
                float cropH = cropW * 0.5f;
                if (cropH > ih) {
                    cropH = static_cast<float>(ih);
                    cropW = cropH * 2.0f;
                }
                const D2D1_RECT_F src{(iw - cropW) * 0.5f, (ih - cropH) * 0.5f,
                                      (iw + cropW) * 0.5f, (ih + cropH) * 0.5f};
                target->DrawBitmap(bitmap->bitmap.Get(), thumbRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
                if (clip) target->PopLayer();
            } else if (iconImage_) {
                DrawGlyph(target, iconImage_.Get(),
                          D2D1::Point2F((thumbRect.left + thumbRect.right) * 0.5f,
                                        (thumbRect.top + thumbRect.bottom) * 0.5f),
                          18.0f * s, b.muted.Get());
            }
            const float textLeft = thumbRect.right + 10.0f * s;
            const float textW = card.right - kCardPadX * s - textLeft;
            const float lineH = 17.0f * s;
            const float subH = 14.0f * s;
            const float blockH = lineH + 3.0f * s + subH;
            const float blockTop = contentTop + (kThumbH * s - blockH) * 0.5f;
            DrawTextLayoutBlock(graphics_ ? graphics_->dwrite() : nullptr, target,
                                entry.label, textFormat_.Get(),
                                D2D1::RectF(textLeft, blockTop, textLeft + textW,
                                            blockTop + lineH),
                                b.fg.Get(), false);
            if (entry.width > 0 && entry.height > 0 && smallFormat_) {
                const std::wstring dims = std::to_wstring(entry.width) + L"×" +
                                          std::to_wstring(entry.height);
                target->DrawText(dims.c_str(), static_cast<UINT32>(dims.size()),
                                 smallFormat_.Get(),
                                 D2D1::RectF(textLeft, blockTop + lineH + 3.0f * s,
                                             textLeft + textW,
                                             blockTop + lineH + 3.0f * s + subH),
                                 b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            const float lineH = 17.0f * s;
            const float subH = 14.0f * s;
            const float blockH = lineH + 3.0f * s + subH;
            const float blockTop = contentTop + (40.0f * s - blockH) * 0.5f;
            DrawTextLayoutBlock(graphics_ ? graphics_->dwrite() : nullptr, target,
                                entry.label, textFormat_.Get(),
                                D2D1::RectF(contentLeft, blockTop, contentLeft + contentWidth,
                                            blockTop + lineH),
                                b.fg.Get(), false);
            std::wstring sub;
            if (entry.kind == SNOWAIR_ENTRY_MEDIA) {
                sub = FormatDuration(entry.duration);
                if (sub.empty() && entry.width > 0 && entry.height > 0) {
                    sub = std::to_wstring(entry.width) + L"×" + std::to_wstring(entry.height);
                }
            } else {
                sub = FolderNameOf(entry.path);
            }
            if (!sub.empty() && smallFormat_) {
                target->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                                 smallFormat_.Get(),
                                 D2D1::RectF(contentLeft, blockTop + lineH + 3.0f * s,
                                             contentLeft + contentWidth,
                                             blockTop + lineH + 3.0f * s + subH),
                                 b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        /* tags: history-style chips (type tag, custom tags, then "+") */
        const float tagTop = card.bottom - kCardPadY * s - kCardTagH * s;
        const float tagRight = card.right - kCardPadX * s;
        const float plusW = 16.0f * s;
        const float tagLimit = tagRight - plusW - 6.0f * s;
        float cx = card.left + kCardPadX * s;
        const float tagH = kCardTagH * s;
        const bool sameRow = row == hovered_;
        auto drawTagChip = [&](const std::wstring& text, bool hovered) {
            const float w = TextWidth(text, tagFormat_.Get()) + 8.0f * s;
            if (cx + w > tagLimit) return;
            const D2D1_RECT_F chip = D2D1::RectF(cx, tagTop, cx + w, tagTop + tagH);
            target->FillRoundedRectangle(D2D1::RoundedRect(chip, 4.0f * s, 4.0f * s),
                                         hovered ? b.tagHover.Get() : b.tagBg.Get());
            target->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                             tagFormat_.Get(), chip,
                             hovered ? b.fg.Get() : b.muted.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            cx += w + 6.0f * s;
        };
        drawTagChip(TypeTagFor(entry), sameRow && hoveredTag_ == 0);
        for (int i = 0; i < static_cast<int>(entry.tags.size()); ++i) {
            drawTagChip(entry.tags[static_cast<size_t>(i)], sameRow && hoveredTag_ == i + 1);
        }
        if (cx + plusW <= tagRight) {
            const D2D1_RECT_F chip = D2D1::RectF(cx, tagTop, cx + plusW, tagTop + tagH);
            const bool hovered =
                sameRow && hoveredTag_ == static_cast<int>(entry.tags.size()) + 1;
            target->FillRoundedRectangle(D2D1::RoundedRect(chip, 4.0f * s, 4.0f * s),
                                         hovered ? b.tagHover.Get() : b.tagBg.Get());
            target->DrawText(L"+", 1, tagFormat_.Get(), chip,
                             hovered ? b.fg.Get() : b.muted.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    /* slim scroll indicator */
    const float viewport = ListViewportHeight();
    if (rowsHeight_ > viewport + 1.0f && viewport > 10.0f) {
        const float trackTop = ListTop();
        const float trackH = viewport;
        float thumbH = std::max(24.0f * s, trackH * viewport / rowsHeight_);
        const float maxScroll = rowsHeight_ - viewport;
        const float t = maxScroll > 0.0f ? scrollY_ / maxScroll : 0.0f;
        const float thumbTop = trackTop + t * (trackH - thumbH);
        const D2D1_RECT_F thumb = D2D1::RectF(width_ - 4.0f * s, thumbTop,
                                              width_ - 2.0f * s, thumbTop + thumbH);
        target->FillRoundedRectangle(D2D1::RoundedRect(thumb, 1.0f * s, 1.0f * s),
                                     b.border.Get());
    }
}

void ClipboardBubble::PaintEmptyState(ID2D1RenderTarget* target, const Brushes& b)
{
    const wchar_t* text = entries_.empty() ? L"还没有内容" : L"这个分类下还没有内容";
    const D2D1_RECT_F rect = D2D1::RectF(kPad * scale_, ListTop(),
                                         width_ - kPad * scale_,
                                         height_ - kPad * scale_);
    target->DrawText(text, static_cast<UINT32>(wcslen(text)), chipFormat_.Get(), rect,
                     b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void ClipboardBubble::PaintModals(ID2D1RenderTarget* target, const Brushes& b)
{
    const float s = scale_;
    /* 弹窗统一底色 + 柔和投影，让卡片从变暗的面板上明显浮起来。 */
    auto paintModalCard = [&](const D2D1_RECT_F& card) {
        const float radius = 16.0f * s;
        target->FillRoundedRectangle(D2D1::RoundedRect(card, radius, radius),
                                     b.modalCard.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(card, radius, radius),
                                     b.modalBorder.Get(), 1.0f * s);
    };

    if (noteEditorOpen_) {
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.0f, kArrowH * s, width_, height_), 16.0f * s,
                              16.0f * s),
            b.dim.Get());
        const NoteEditorLayout layout = BuildNoteEditorLayout();
        paintModalCard(layout.card);
        if (titleFormat_) {
            target->DrawText(L"新建便签", 4, titleFormat_.Get(), layout.title, b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        target->FillRoundedRectangle(D2D1::RoundedRect(layout.field, 6.0f * s, 6.0f * s),
                                     b.modalCard.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(layout.field, 6.0f * s, 6.0f * s),
                                     b.modalBorder.Get(), 1.0f * s);
        target->FillRoundedRectangle(
            D2D1::RoundedRect(layout.cancel, 6.0f * s, 6.0f * s),
            hoveredModalButton_ == 1 ? b.tagHover.Get() : b.card.Get());
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(layout.cancel, 6.0f * s, 6.0f * s), b.modalBorder.Get(),
            1.0f * s);
        target->FillRoundedRectangle(
            D2D1::RoundedRect(layout.save, 6.0f * s, 6.0f * s),
            hoveredModalButton_ == 2 ? b.primaryHover.Get() : b.primary.Get());
        if (buttonFormat_) {
            target->DrawText(L"取消", 2, buttonFormat_.Get(), layout.cancel, b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target->DrawText(L"完成", 2, buttonFormat_.Get(), layout.save, b.primaryFg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }

    if (deleteConfirm_ >= 0) {
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.0f, kArrowH * s, width_, height_), 10.0f * s,
                              10.0f * s),
            b.dim.Get());
        const float w = 300.0f * s;
        const float h = 150.0f * s;
        const float left = (width_ - w) * 0.5f;
        const float top = (height_ - h) * 0.5f;
        const D2D1_RECT_F card = D2D1::RectF(left, top, left + w, top + h);
        paintModalCard(card);
        if (titleFormat_) {
            target->DrawText(L"删除分类", 4, titleFormat_.Get(),
                             D2D1::RectF(left + 16.0f * s, top + 12.0f * s,
                                         left + w - 16.0f * s, top + 34.0f * s),
                             b.fg.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (smallFormat_) {
            target->DrawText(L"该分类下还有条目，删除后这些条目会移出该分类。", 23,
                             smallFormat_.Get(),
                             D2D1::RectF(left + 16.0f * s, top + 44.0f * s,
                                         left + w - 16.0f * s, top + 80.0f * s),
                             b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (buttonFormat_) {
            const D2D1_RECT_F cancel = D2D1::RectF(left + w - 148.0f * s, top + h - 44.0f * s,
                                                   left + w - 84.0f * s, top + h - 16.0f * s);
            const D2D1_RECT_F del = D2D1::RectF(left + w - 76.0f * s, top + h - 44.0f * s,
                                                left + w - 16.0f * s, top + h - 16.0f * s);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(cancel, 8.0f * s, 8.0f * s),
                hoveredModalButton_ == 1 ? b.tagHover.Get() : b.card.Get());
            target->DrawRoundedRectangle(D2D1::RoundedRect(cancel, 8.0f * s, 8.0f * s),
                                         b.modalBorder.Get(), 1.0f * s);
            target->FillRoundedRectangle(D2D1::RoundedRect(del, 8.0f * s, 8.0f * s),
                                         hoveredModalButton_ == 3 ? b.dangerHover.Get()
                                                                  : b.danger.Get());
            target->DrawText(L"取消", 2, buttonFormat_.Get(), cancel, b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target->DrawText(L"删除", 2, buttonFormat_.Get(), del, b.primaryFg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }
    if (!tagEditorOpen_ && !categoryEditorOpen_) return;
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.0f, kArrowH * s, width_, height_), 16.0f * s,
                          16.0f * s),
        b.dim.Get());

    if (categoryEditorOpen_) {
        const float w = 320.0f * s;
        const float h = 132.0f * s;
        const float pad = 18.0f * s;
        const float left = (width_ - w) * 0.5f;
        const float top = (height_ - h) * 0.5f;
        const float right = left + w;
        const D2D1_RECT_F card = D2D1::RectF(left, top, right, top + h);
        paintModalCard(card);
        if (titleFormat_) {
            target->DrawText(renamingCategory_ >= 0 ? L"重命名分类" : L"新建分类", 4, titleFormat_.Get(),
                             D2D1::RectF(left + pad, top + 16.0f * s, right - pad,
                                         top + 36.0f * s),
                             b.fg.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        const D2D1_RECT_F field = D2D1::RectF(left + pad, top + 46.0f * s, right - pad,
                                              top + 76.0f * s);
        const D2D1_RECT_F ok = D2D1::RectF(right - pad - 72.0f * s, top + 86.0f * s, right - pad,
                                           top + 116.0f * s);
        const D2D1_RECT_F cancel = D2D1::RectF(ok.left - 8.0f * s - 72.0f * s,
                                               top + 86.0f * s, ok.left - 8.0f * s,
                                               top + 116.0f * s);
        target->FillRoundedRectangle(D2D1::RoundedRect(field, 6.0f * s, 6.0f * s),
                                     b.modalCard.Get());
        target->DrawRoundedRectangle(D2D1::RoundedRect(field, 6.0f * s, 6.0f * s),
                                     b.modalBorder.Get(), 1.0f * s);
        target->FillRoundedRectangle(
            D2D1::RoundedRect(cancel, 6.0f * s, 6.0f * s),
            hoveredModalButton_ == 1 ? b.tagHover.Get() : b.card.Get());
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(cancel, 6.0f * s, 6.0f * s), b.modalBorder.Get(), 1.0f * s);
        target->FillRoundedRectangle(
            D2D1::RoundedRect(ok, 6.0f * s, 6.0f * s),
            hoveredModalButton_ == 2 ? b.primaryHover.Get() : b.primary.Get());
        if (buttonFormat_) {
            target->DrawText(L"取消", 2, buttonFormat_.Get(), cancel, b.fg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            target->DrawText(L"确定", 2, buttonFormat_.Get(), ok, b.primaryFg.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }

    const TagEditorLayout layout = BuildTagEditorLayout();
    paintModalCard(layout.card);
    if (titleFormat_) {
        target->DrawText(L"分类与标签", 5, titleFormat_.Get(), layout.title, b.fg.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    auto label = [&](const D2D1_RECT_F& r, const wchar_t* text) {
        if (r.right <= r.left || !smallFormat_) return;
        target->DrawText(text, static_cast<UINT32>(wcslen(text)), smallFormat_.Get(), r,
                         b.muted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    label(layout.labelCats, L"分类");
    label(layout.labelTags, L"标签");

    auto chipText = [&](const TagChipRect& c, ID2D1Brush* ink) {
        if (!chipFormat_) return;
        target->DrawText(c.text.c_str(), static_cast<UINT32>(c.text.size()),
                         chipFormat_.Get(), c.rect, ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    /* 分类：选中=实心主色，未选中=描边（SnowAir 选中胶囊的写法），一眼能区分。 */
    for (const auto& c : layout.catChips) {
        const bool selected =
            std::find(editCats_.begin(), editCats_.end(), c.index) != editCats_.end();
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(c.rect, 4.0f * s, 4.0f * s);
        if (selected) {
            target->FillRoundedRectangle(rounded, b.primary.Get());
            chipText(c, b.primaryFg.Get());
        } else {
            target->FillRoundedRectangle(rounded, b.tagBg.Get());
            chipText(c, b.secondaryFg.Get());
        }
    }

    /* 内联输入框：标签 chip 与输入光标共用一条流；输入框本体由 modalEdit_ 覆盖。 */
    target->FillRoundedRectangle(D2D1::RoundedRect(layout.field, 6.0f * s, 6.0f * s),
                                 b.modalCard.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(layout.field, 6.0f * s, 6.0f * s),
                                 b.modalBorder.Get(), 1.0f * s);
    for (const auto& c : layout.tagChips) {
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(c.rect, 4.0f * s, 4.0f * s);
        target->FillRoundedRectangle(rounded, b.tagBg.Get());
        chipText(c, b.fg.Get());
    }

    target->FillRoundedRectangle(
        D2D1::RoundedRect(layout.cancel, 6.0f * s, 6.0f * s),
        hoveredModalButton_ == 1 ? b.tagHover.Get() : b.card.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(layout.cancel, 6.0f * s, 6.0f * s),
                                 b.modalBorder.Get(), 1.0f * s);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(layout.save, 6.0f * s, 6.0f * s),
        hoveredModalButton_ == 2 ? b.primaryHover.Get() : b.primary.Get());
    if (buttonFormat_) {
        target->DrawText(L"取消", 2, buttonFormat_.Get(), layout.cancel, b.fg.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        target->DrawText(L"确定", 2, buttonFormat_.Get(), layout.save, b.primaryFg.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

void ClipboardBubble::Open(int screenX, int screenY, float scale, const Theme& theme)
{
    if (!hwnd()) return;
    theme_ = theme;
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    scale_ = std::max(0.75f, scale);
    filter_ = 0;
    hovered_ = -1;
    hoveredAction_ = -1;
    hoveredChip_ = -1;
    hoveredTag_ = -1;
    hoveredPlus_ = false;
    hoveredModalButton_ = 0;
    query_.clear();
    searchFocused_ = false;
    caretVisible_ = true;
    chipScrollX_ = 0.0f;
    chipPress_ = -1;
    chipDragging_ = false;
    tooltipText_.clear();
    trackingLeave_ = false;
    scrollY_ = 0.0f;
    tagEditorOpen_ = false;
    categoryEditorOpen_ = false;
    noteEditorOpen_ = false;
    if (noteEdit_) ShowWindow(noteEdit_, SW_HIDE);
    tagEditorEntry_ = -1;
    tagSelection_.clear();
    settingsOpen_ = false;
    settingsDrag_ = -1;
    settingsDrop_ = -1;
    settingsHover_ = -1;
    hoveredSettingsAdd_ = false;
    deleteConfirm_ = -1;
    renamingCategory_ = -1;
    editCats_.clear();
    hoveredGear_ = false;
    gearPressed_ = false;
    if (edit_) {
        syncingEdit_ = true;
        SetWindowTextW(edit_, L"");
        syncingEdit_ = false;
    }
    if (modalEdit_) ShowWindow(modalEdit_, SW_HIDE);

    width_ = kBubbleW * scale_;
    EnsureFormats(scale_);
    LoadEntries();
    RefreshCategories();
    RebuildShown();

    const int w = static_cast<int>(std::lround(width_));
    MONITORINFO mi{sizeof(MONITORINFO)};
    const HMONITOR monitor = MonitorFromPoint(POINT{screenX, screenY}, MONITOR_DEFAULTTONEAREST);
    if (monitor) GetMonitorInfoW(monitor, &mi);
    x_ = screenX - w + static_cast<int>(80.0f * scale_);
    y_ = screenY + static_cast<int>(5.0f * scale_);
    if (mi.rcWork.right > mi.rcWork.left) {
        x_ = std::clamp(x_, static_cast<int>(mi.rcWork.left) + 8,
                        static_cast<int>(mi.rcWork.right) - w - 8);
        y_ = std::max(y_, static_cast<int>(mi.rcWork.top) + 8);
    }
    anchorX_ = rack::Clamp(static_cast<float>(screenX - x_), 24.0f * scale_,
                           width_ - 24.0f * scale_);
    Reload(true);
    ShowNoActivate();
    LONG_PTR ex = GetWindowLongPtrW(hwnd(), GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd(), GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    ShowWindow(hwnd(), SW_SHOW);
    ActivateForInput();
    ImmAssociateContextEx(hwnd(), nullptr, IACE_DEFAULT);
    captured_ = true;
    hookOwner_ = this;
    if (!mouseHook_) {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr),
                                       0);
    }
    Render();
    syncingEdit_ = true;
    SetWindowTextW(edit_, L"");
    syncingEdit_ = false;
    LayoutEditors();
    ShowWindow(edit_, SW_SHOWNOACTIVATE);
    PositionIme();
}

void ClipboardBubble::ApplyTheme(const Theme& theme)
{
    theme_ = theme;
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    if (visible()) {
        Render();
        if (edit_) InvalidateRect(edit_, nullptr, TRUE);
        if (modalEdit_) InvalidateRect(modalEdit_, nullptr, TRUE);
    }
}

void ClipboardBubble::Close()
{
    const bool wasVisible = visible();
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (hookOwner_ == this) hookOwner_ = nullptr;
    if (hwnd()) KillTimer(hwnd(), kCaretTimer);
    searchFocused_ = false;
    caretVisible_ = false;
    tagEditorOpen_ = false;
    categoryEditorOpen_ = false;
    noteEditorOpen_ = false;
    tagEditorEntry_ = -1;
    tagSelection_.clear();
    chipPress_ = -1;
    chipDragging_ = false;
    tooltipText_.clear();
    trackingLeave_ = false;
    ClearHover();
    if (captured_ && hwnd()) ReleaseCapture();
    captured_ = false;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    if (modalEdit_) ShowWindow(modalEdit_, SW_HIDE);
    if (noteEdit_) ShowWindow(noteEdit_, SW_HIDE);
    entries_.clear();
    shown_.clear();
    rowTops_.clear();
    rowHeights_.clear();
    thumbs_.clear();
    rowsHeight_ = 0.0f;
    scrollY_ = 0.0f;
    Hide();
    if (wasVisible && onClosed_) onClosed_();
}

void ClipboardBubble::ActivateForInput()
{
    if (!hwnd()) return;
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    const bool attach = foregroundThread && foregroundThread != currentThread;
    if (attach) AttachThreadInput(currentThread, foregroundThread, TRUE);
    BringWindowToTop(hwnd());
    SetForegroundWindow(hwnd());
    SetActiveWindow(hwnd());
    SetFocus(hwnd());
    if (attach) AttachThreadInput(currentThread, foregroundThread, FALSE);
}

void ClipboardBubble::PositionIme()
{
    if (!hwnd() || positioningIme_) return;
    HWND source = nullptr;
    POINT pt{};
    if (tagEditorOpen_ || categoryEditorOpen_) {
        if (!modalEdit_) return;
        source = modalEdit_;
        RECT rc{};
        GetWindowRect(modalEdit_, &rc);
        pt = POINT{rc.left, rc.bottom};
    } else {
        if (!edit_) return;
        source = edit_;
        RECT rc{};
        GetWindowRect(edit_, &rc);
        pt = POINT{rc.left, rc.bottom};
    }
    if (!source) return;
    positioningIme_ = true;
    HIMC imc = ImmGetContext(hwnd());
    if (!imc) {
        positioningIme_ = false;
        return;
    }
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = pt;
    ImmSetCompositionWindow(imc, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = pt;
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(hwnd(), imc);
    positioningIme_ = false;
}

/* ------------------------------------------------------------------ */
/* edits                                                               */
/* ------------------------------------------------------------------ */

void ClipboardBubble::CreateSearchEdit()
{
    if (edit_ || !hwnd()) return;
    edit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                            WS_POPUP | ES_LEFT | ES_AUTOHSCROLL | ES_NOHIDESEL, 0, 0, 100, 32,
                            hwnd(), nullptr, util::GetDllModule(), nullptr);
    if (!edit_) return;
    SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    editOldProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc)));
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
}

void ClipboardBubble::DestroySearchEdit()
{
    if (edit_) {
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
}

void ClipboardBubble::CreateModalEdit()
{
    if (modalEdit_ || !hwnd()) return;
    modalEdit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                                 WS_POPUP | ES_LEFT | ES_AUTOHSCROLL | ES_NOHIDESEL, 0, 0, 100,
                                 28, hwnd(), nullptr, util::GetDllModule(), nullptr);
    if (!modalEdit_) return;
    SetWindowLongPtrW(modalEdit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    modalEditOldProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        modalEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ModalEditProc)));
    SendMessageW(modalEdit_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"输入后回车"));
    SendMessageW(modalEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
}

void ClipboardBubble::DestroyModalEdit()
{
    if (modalEdit_) {
        DestroyWindow(modalEdit_);
        modalEdit_ = nullptr;
    }
}

void ClipboardBubble::LayoutEditors()
{
    if (!hwnd()) return;
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    editFont_ = CreateFontW(-static_cast<int>(std::lround(12.0f * scale_)), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    if (edit_ && !tagEditorOpen_ && !categoryEditorOpen_ && !noteEditorOpen_ &&
        deleteConfirm_ < 0 && !settingsOpen_) {
        const float s = scale_;
        const float editH = 20.0f * s;
        const float editTop = SearchTop() + (kSearchH * s - editH) * 0.5f;
        const D2D1_RECT_F pill = D2D1::RectF(
            kPad * s + 2.0f * s, editTop,
            width_ - (kPad + 2.0f * kPlusW + 2.0f * kPlusGap) * s - 2.0f * s,
            editTop + editH);
        POINT pt{static_cast<LONG>(std::lround(pill.left)),
                 static_cast<LONG>(std::lround(pill.top))};
        ClientToScreen(hwnd(), &pt);
        const int ew = static_cast<int>(std::lround(pill.right - pill.left));
        const int eh = static_cast<int>(std::lround(pill.bottom - pill.top));
        SetWindowPos(edit_, HWND_TOPMOST, pt.x, pt.y, ew, eh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowRgn(edit_, CreateRoundRectRgn(0, 0, ew + 1, eh + 1, 16, 16), TRUE);
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(static_cast<int>(std::lround(8.0f * s)),
                                static_cast<int>(std::lround(8.0f * s))));
    }

    if (modalEdit_) {
        float left = 0.0f;
        float top = 0.0f;
        float ew = 0.0f;
        float eh = 0.0f;
        if (categoryEditorOpen_) {
            const float s = scale_;
            const float w = 320.0f * s;
            const float h = 132.0f * s;
            const float cardLeft = (width_ - w) * 0.5f;
            const float cardTop = (height_ - h) * 0.5f;
            const float fieldTop = cardTop + 46.0f * s;
            left = cardLeft + 18.0f * s + 2.0f * s;
            top = fieldTop + 5.0f * s;
            ew = w - 36.0f * s - 4.0f * s;
            eh = 20.0f * s;
        } else if (tagEditorOpen_) {
            const TagEditorLayout layout = BuildTagEditorLayout();
            const float s = scale_;
            left = layout.edit.left + 1.0f * s;
            top = layout.edit.top;
            ew = (layout.edit.right - layout.edit.left) - 2.0f * s;
            eh = layout.edit.bottom - layout.edit.top;
        }
        if (ew > 4.0f && eh > 4.0f) {
            POINT pt{static_cast<LONG>(std::lround(left)), static_cast<LONG>(std::lround(top))};
            ClientToScreen(hwnd(), &pt);
            SetWindowPos(modalEdit_, HWND_TOPMOST, pt.x, pt.y, static_cast<int>(std::lround(ew)),
                         static_cast<int>(std::lround(eh)), SWP_NOACTIVATE | SWP_SHOWWINDOW);
            SetWindowRgn(modalEdit_,
                         CreateRoundRectRgn(0, 0, static_cast<int>(std::lround(ew)) + 1,
                                            static_cast<int>(std::lround(eh)) + 1, 16, 16),
            TRUE);
            SendMessageW(modalEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
            const int margin = static_cast<int>(std::lround((tagEditorOpen_ ? 2.0f : 8.0f) * scale_));
            SendMessageW(modalEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(margin, margin));
            if (tagEditorOpen_ || categoryEditorOpen_) {
                ShowWindow(modalEdit_, SW_SHOWNOACTIVATE);
            }
        }
    }

    if (noteEdit_ && noteEditorOpen_) {
        const NoteEditorLayout layout = BuildNoteEditorLayout();
        const float s = scale_;
        const float left = layout.field.left + 4.0f * s;
        const float top = layout.field.top + 4.0f * s;
        const float ew = (layout.field.right - layout.field.left) - 8.0f * s;
        const float eh = (layout.field.bottom - layout.field.top) - 8.0f * s;
        POINT pt{static_cast<LONG>(std::lround(left)), static_cast<LONG>(std::lround(top))};
        ClientToScreen(hwnd(), &pt);
        SetWindowPos(noteEdit_, HWND_TOPMOST, pt.x, pt.y, static_cast<int>(std::lround(ew)),
                     static_cast<int>(std::lround(eh)), SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SendMessageW(noteEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
        SendMessageW(noteEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(static_cast<int>(std::lround(4.0f * s)),
                                static_cast<int>(std::lround(4.0f * s))));
        ShowWindow(noteEdit_, SW_SHOWNOACTIVATE);
    }
}

/* ------------------------------------------------------------------ */
/* interaction                                                         */
/* ------------------------------------------------------------------ */

void ClipboardBubble::OpenTagEditor(int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= static_cast<int>(entries_.size())) return;
    ClearHover();
    noteEditorOpen_ = false;
    if (noteEdit_) ShowWindow(noteEdit_, SW_HIDE);
    tagEditorOpen_ = true;
    categoryEditorOpen_ = false;
    tagEditorEntry_ = entryIndex;
    tagSelection_ = entries_[static_cast<size_t>(entryIndex)].tags;
    editCats_.clear();
    const std::wstring key = EntryKey(entries_[static_cast<size_t>(entryIndex)]);
    const auto assigned = assignments_.find(key);
    if (assigned != assignments_.end()) {
        for (int i = 0; i < static_cast<int>(categories_.size()); ++i) {
            if (categories_[static_cast<size_t>(i)].builtin >= 0) continue;
            if (std::find(assigned->second.begin(), assigned->second.end(),
                          categories_[static_cast<size_t>(i)].name) != assigned->second.end()) {
                editCats_.push_back(i);
            }
        }
    }
    if (edit_) ShowWindow(edit_, SW_HIDE);
    CreateModalEdit();
    if (modalEdit_) {
        SetWindowTextW(modalEdit_, L"");
        SendMessageW(modalEdit_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"输入标签，空格分隔"));
        ActivateForInput();
        SetFocus(modalEdit_);
    }
    LayoutEditors();
    Render();
}

void ClipboardBubble::CloseTagEditor(bool save)
{
    if (!tagEditorOpen_) return;
    if (save) CommitTagText();
    const int entryIndex = tagEditorEntry_;
    if (save && entryIndex >= 0 && entryIndex < static_cast<int>(entries_.size())) {
        const std::wstring joined = JoinLines(tagSelection_);
        if (host_ && host_->entry_tags_set) {
            host_->entry_tags_set(host_->user, entryIndex, joined.c_str());
        }
        entries_[static_cast<size_t>(entryIndex)].tags = tagSelection_;
        {
            const std::wstring key = EntryKey(entries_[static_cast<size_t>(entryIndex)]);
            std::vector<std::wstring> names;
            for (int index : editCats_) {
                if (index >= 0 && index < static_cast<int>(categories_.size()) &&
                    categories_[static_cast<size_t>(index)].builtin < 0) {
                    names.push_back(categories_[static_cast<size_t>(index)].name);
                }
            }
            if (names.empty()) assignments_.erase(key);
            else assignments_[key] = names;
            SaveCategories();
        }
        RefreshCategories();
    }
    tagEditorOpen_ = false;
    tagEditorEntry_ = -1;
    tagSelection_.clear();
    hoveredModalButton_ = 0;
    if (modalEdit_) {
        ShowWindow(modalEdit_, SW_HIDE);
        SetWindowTextW(modalEdit_, L"");
    }

    Reload(false);
    SetFocus(hwnd());
}

void ClipboardBubble::OpenCategoryEditor()
{
    ClearHover();
    noteEditorOpen_ = false;
    if (noteEdit_) ShowWindow(noteEdit_, SW_HIDE);
    categoryEditorOpen_ = true;
    tagEditorOpen_ = false;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    CreateModalEdit();
    if (modalEdit_) {
        SetWindowTextW(modalEdit_, L"");
        SendMessageW(modalEdit_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"输入分类名称"));
        ActivateForInput();
        SetFocus(modalEdit_);
    }
    LayoutEditors();
    Render();
}

void ClipboardBubble::CloseCategoryEditor(bool save)
{
    if (!categoryEditorOpen_) return;
    if (renamingCategory_ >= 0) {
        const int index = renamingCategory_;
        renamingCategory_ = -1;
        if (save && modalEdit_ && index >= 0 && index < static_cast<int>(categories_.size())) {
            const int length = GetWindowTextLengthW(modalEdit_);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            if (length > 0) GetWindowTextW(modalEdit_, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            text = Trim(text);
            if (!text.empty()) {
                const std::wstring old = categories_[static_cast<size_t>(index)].name;
                bool exists = false;
                for (const auto& category : categories_) {
                    if (category.name == text) {
                        exists = true;
                        break;
                    }
                }
                if (!exists && text != old) {
                    categories_[static_cast<size_t>(index)].name = text;
                    for (auto& kv : assignments_) {
                        for (auto& name : kv.second) {
                            if (name == old) name = text;
                        }
                    }
                    SaveCategories();
                    RefreshVisibleCategories();
                    RebuildShown();
                }
            }
        }
        categoryEditorOpen_ = false;
        settingsOpen_ = true;
        hoveredModalButton_ = 0;
        if (modalEdit_) {
            ShowWindow(modalEdit_, SW_HIDE);
            SetWindowTextW(modalEdit_, L"");
        }
        if (edit_) ShowWindow(edit_, SW_HIDE);
        Render();
        return;
    }
    if (save && modalEdit_) {
        const int length = GetWindowTextLengthW(modalEdit_);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        if (length > 0) GetWindowTextW(modalEdit_, text.data(), length + 1);
        text.resize(static_cast<size_t>(length));
        text = Trim(text);
        if (!text.empty()) {
            bool exists = false;
            for (const auto& known : categories_) {
                if (known.name == text) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                Category category;
                category.name = text;
                categories_.push_back(std::move(category));
                SaveCategories();
                RefreshCategories();
                for (int i = 0; i < static_cast<int>(visibleCats_.size()); ++i) {
                    if (categories_[static_cast<size_t>(visibleCats_[static_cast<size_t>(i)])].name == text) {
                        ScrollToShowChip(i);
                        break;
                    }
                }
            }
        }
    }
    categoryEditorOpen_ = false;
    hoveredModalButton_ = 0;
    if (modalEdit_) {
        ShowWindow(modalEdit_, SW_HIDE);
        SetWindowTextW(modalEdit_, L"");
    }

    LayoutEditors();
    Render();
}

ClipboardBubble::NoteEditorLayout ClipboardBubble::BuildNoteEditorLayout() const
{
    const float s = scale_;
    const float w = 320.0f * s;
    const float h = 222.0f * s;
    const float pad = 18.0f * s;
    const float left = (width_ - w) * 0.5f;
    const float top = (height_ - h) * 0.5f;
    const float right = left + w;

    NoteEditorLayout layout;
    layout.card = D2D1::RectF(left, top, right, top + h);
    layout.title = D2D1::RectF(left + pad, top + 16.0f * s, right - pad, top + 36.0f * s);
    layout.field = D2D1::RectF(left + pad, top + 46.0f * s, right - pad, top + 166.0f * s);
    layout.save = D2D1::RectF(right - pad - 72.0f * s, top + 176.0f * s, right - pad,
                              top + 206.0f * s);
    layout.cancel = D2D1::RectF(layout.save.left - 8.0f * s - 72.0f * s, top + 176.0f * s,
                                layout.save.left - 8.0f * s, top + 206.0f * s);
    return layout;
}

void ClipboardBubble::CreateNoteEdit()
{
    if (noteEdit_ || !hwnd()) return;
    noteEdit_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                                WS_POPUP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                    ES_NOHIDESEL | ES_WANTRETURN,
                                0, 0, 100, 80, hwnd(), nullptr, util::GetDllModule(),
                                nullptr);
    if (!noteEdit_) return;
    SetWindowLongPtrW(noteEdit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    noteEditOldProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        noteEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NoteEditProc)));
    SendMessageW(noteEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
}

void ClipboardBubble::DestroyNoteEdit()
{
    if (noteEdit_) {
        DestroyWindow(noteEdit_);
        noteEdit_ = nullptr;
    }
}

LRESULT CALLBACK ClipboardBubble::NoteEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam)
{
    auto* self = reinterpret_cast<ClipboardBubble*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        self->CloseNoteEditor(false);
        return 0;
    }
    return CallWindowProcW(self->noteEditOldProc_ ? self->noteEditOldProc_ : DefWindowProcW,
                           hwnd, message, wParam, lParam);
}

void ClipboardBubble::OpenNoteEditor()
{
    ClearHover();
    noteEditorOpen_ = true;
    categoryEditorOpen_ = false;
    tagEditorOpen_ = false;
    if (edit_) ShowWindow(edit_, SW_HIDE);
    if (modalEdit_) ShowWindow(modalEdit_, SW_HIDE);
    CreateNoteEdit();
    if (noteEdit_) {
        SetWindowTextW(noteEdit_, L"");
        ActivateForInput();
        SetFocus(noteEdit_);
    }
    LayoutEditors();
    Render();
    if (noteEdit_) SetFocus(noteEdit_);
}

void ClipboardBubble::CloseNoteEditor(bool save)
{
    if (!noteEditorOpen_) return;
    std::wstring text;
    if (save && noteEdit_) {
        const int length = GetWindowTextLengthW(noteEdit_);
        if (length > 0) {
            text.resize(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(noteEdit_, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            text = Trim(text);
        }
    }
    noteEditorOpen_ = false;
    hoveredModalButton_ = 0;
    if (noteEdit_) {
        ShowWindow(noteEdit_, SW_HIDE);
        SetWindowTextW(noteEdit_, L"");
    }
    Render();
    if (save) {
        if (!text.empty() && onHangText_) onHangText_(text);
        Close();
    }
}

void ClipboardBubble::CommitTagText()
{
    /* 与 SnowAir 自定义标签弹窗一致：空格把当前输入变成一个 chip。 */
    if (!tagEditorOpen_ || !modalEdit_) return;
    const int length = GetWindowTextLengthW(modalEdit_);
    if (length <= 0) return;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(modalEdit_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    text = Trim(text);
    SetWindowTextW(modalEdit_, L"");
    if (text.empty()) {
        LayoutEditors();
        return;
    }
    for (const auto& tag : tagSelection_) {
        if (_wcsicmp(tag.c_str(), text.c_str()) == 0) {
            LayoutEditors();
            return;
        }
    }
    tagSelection_.push_back(text);
    Render();
    LayoutEditors();
}

/* ------------------------------------------------------------------ */
/* window message handling                                             */
/* ------------------------------------------------------------------ */

LRESULT CALLBACK ClipboardBubble::SearchEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                                 LPARAM lParam)
{
    auto* self = reinterpret_cast<ClipboardBubble*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (self) self->Close();
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    if (message == WM_PAINT) {
        const LRESULT result =
            CallWindowProcW(self ? self->editOldProc_ : DefWindowProcW, hwnd, message, wParam,
                            lParam);
        if (self && GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
            HDC dc = GetDC(hwnd);
            if (dc) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                rc.left += static_cast<LONG>(std::lround(8.0f * self->scale_));
                rc.right -= static_cast<LONG>(std::lround(8.0f * self->scale_));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(static_cast<BYTE>(self->theme_.fgTertiary.r * 255.0f),
                                     static_cast<BYTE>(self->theme_.fgTertiary.g * 255.0f),
                                     static_cast<BYTE>(self->theme_.fgTertiary.b * 255.0f)));
                HGDIOBJ old = SelectObject(dc, self->editFont_);
                DrawTextW(dc, L"请输入关键词...", -1, &rc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, old);
                ReleaseDC(hwnd, dc);
            }
        }
        return result;
    }
    return CallWindowProcW(self ? self->editOldProc_ : DefWindowProcW, hwnd, message, wParam,
                           lParam);
}

LRESULT CALLBACK ClipboardBubble::ModalEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                                LPARAM lParam)
{
    auto* self = reinterpret_cast<ClipboardBubble*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            if (self->tagEditorOpen_) self->CloseTagEditor(false);
            else if (self->categoryEditorOpen_) self->CloseCategoryEditor(false);
            return 0;
        }
        if (self->tagEditorOpen_) {
            /* 空格成 chip；输入为空时退格删掉最后一个 chip —— 与 SnowAir 一致。 */
            if (wParam == VK_SPACE) {
                if (GetWindowTextLengthW(hwnd) > 0) {
                    self->CommitTagText();
                }
                return 0;
            }
            if ((wParam == VK_BACK || wParam == VK_DELETE) &&
                GetWindowTextLengthW(hwnd) == 0) {
                if (!self->tagSelection_.empty()) {
                    self->tagSelection_.pop_back();
                    self->Render();
                    self->LayoutEditors();
                }
                return 0;
            }
            if (wParam == VK_RETURN) {
                self->CommitTagText();
                self->CloseTagEditor(true);
                return 0;
            }
        } else if (wParam == VK_RETURN) {
            self->CloseCategoryEditor(true);
            return 0;
        }
    }
    return CallWindowProcW(self->modalEditOldProc_ ? self->modalEditOldProc_ : DefWindowProcW,
                           hwnd, message, wParam, lParam);
}

LRESULT ClipboardBubble::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CTLCOLOREDIT: {
        const HWND source = reinterpret_cast<HWND>(lParam);
        if (source == edit_ || source == modalEdit_ || source == noteEdit_) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(static_cast<BYTE>(theme_.fg.r * 255.0f),
                                 static_cast<BYTE>(theme_.fg.g * 255.0f),
                                 static_cast<BYTE>(theme_.fg.b * 255.0f)));
            const bool modalField = (source == modalEdit_ || source == noteEdit_);
            const D2D1_COLOR_F fill = modalField
                                          ? (theme_.dark ? Rgb(0x262626) : Rgb(0xFFFFFF))
                                          : (theme_.dark ? Rgb(0x4A4A4A) : Rgb(0xEDEDED));
            const COLORREF bg = RGB(static_cast<BYTE>(fill.r * 255.0f),
                                    static_cast<BYTE>(fill.g * 255.0f),
                                    static_cast<BYTE>(fill.b * 255.0f));
            SetBkColor(dc, bg);
            if (!editBrush_ || editBrushColor_ != bg) {
                if (editBrush_) DeleteObject(editBrush_);
                editBrush_ = CreateSolidBrush(bg);
                editBrushColor_ = bg;
            }
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        return LayeredWindow::HandleMessage(message, wParam, lParam);
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam) == edit_ && HIWORD(wParam) == EN_CHANGE &&
            !syncingEdit_ && !settingsOpen_) {
            const int length = GetWindowTextLengthW(edit_);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            if (length > 0) GetWindowTextW(edit_, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            query_ = std::move(text);
            Reload(true);
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        const float dblX = static_cast<float>(GET_X_LPARAM(lParam));
        const float dblY = static_cast<float>(GET_Y_LPARAM(lParam));
        if (HitGear(dblX, dblY)) {
            if (settingsOpen_) CloseSettings();
            else OpenSettings();
            return 0;
        }
        if (settingsOpen_) {
            const int row = ManagerRowAt(dblX, dblY);
            if (row >= 0 && categories_[static_cast<size_t>(row)].builtin < 0) {
                OpenRenameCategory(row);
            }
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));
        if (!tooltipText_.empty()) {
            tooltipText_.clear();
            Render();
        }
        if (mx < 0 || my < 0 || mx > width_ || my > height_) {
            Close();
            return 0;
        }

        if (deleteConfirm_ >= 0) {
            const float s = scale_;
            const float w = 300.0f * s;
            const float h = 150.0f * s;
            const float left = (width_ - w) * 0.5f;
            const float top = (height_ - h) * 0.5f;
            const int button = HitModalButton(mx, my);
            if (button == 3) {
                DeleteCategory(deleteConfirm_);
                deleteConfirm_ = -1;
                ClearHover();
                Render();
                return 0;
            }
            if (button == 1 || !rack::PointIn(D2D1::RectF(left, top, left + w, top + h), mx,
                                              my)) {
                deleteConfirm_ = -1;
                ClearHover();
                Render();
            }
            return 0;
        }

        if (settingsOpen_ && !categoryEditorOpen_ && !tagEditorOpen_ && !noteEditorOpen_) {
            if (HitGear(mx, my)) {
                gearPressed_ = true;
                return 0;
            }
            if (HitPlus(mx, my)) {
                OpenNoteEditor();
                return 0;
            }
            if (rack::PointIn(SettingsAddRect(), mx, my)) {
                OpenCategoryEditor();
                return 0;
            }
            const int row = ManagerRowAt(mx, my);
            if (row >= 0 && row < static_cast<int>(categories_.size())) {
                const D2D1_RECT_F rr = ManagerRowRect(row);
                const bool custom = categories_[static_cast<size_t>(row)].builtin < 0;
                if (custom) {
                    const float dx = mx - (rr.right - 4.0f * scale_);
                    const float dy = my - (rr.top + 4.0f * scale_);
                    if (dx * dx + dy * dy <= 100.0f * scale_ * scale_) {
                    if (CategoryHasItems(row)) {
                        deleteConfirm_ = row;
                        ClearHover();
                        Render();
                    } else {
                        DeleteCategory(row);
                    }
                    return 0;
                    }
                }
                const ULONGLONG now = GetTickCount64();
                if (custom && row == lastCategoryRow_ && now - lastCategoryClick_ < 400) {
                    lastCategoryRow_ = -1;
                    OpenRenameCategory(row);
                    return 0;
                }
                lastCategoryClick_ = now;
                lastCategoryRow_ = row;
                settingsDrag_ = row;
                SetCapture(hwnd());
                settingsDrop_ = row;
                settingsDropHidden_ = categories_[static_cast<size_t>(row)].hidden;
                dragX_ = mx;
                dragY_ = my;
                return 0;
            }
            return 0;
        }

        if (HitGear(mx, my)) {
            gearPressed_ = true;
            return 0;
        }

        if (tagEditorOpen_ || categoryEditorOpen_ || noteEditorOpen_) {
            const float s = scale_;
            D2D1_RECT_F card{};
            TagEditorLayout tagLayout;
            NoteEditorLayout noteLayout;
            if (noteEditorOpen_) {
                noteLayout = BuildNoteEditorLayout();
                card = noteLayout.card;
            } else if (tagEditorOpen_) {
                tagLayout = BuildTagEditorLayout();
                card = tagLayout.card;
            } else {
                const float w = 320.0f * s;
                const float h = 132.0f * s;
                const float left = (width_ - w) * 0.5f;
                const float top = (height_ - h) * 0.5f;
                card = D2D1::RectF(left, top, left + w, top + h);
            }
            if (!rack::PointIn(card, mx, my)) {
                if (noteEditorOpen_) CloseNoteEditor(false);
                else if (tagEditorOpen_) CloseTagEditor(false);
                else CloseCategoryEditor(false);
                return 0;
            }
            const int button = HitModalButton(mx, my);
            if (button == 2) {
                if (noteEditorOpen_) CloseNoteEditor(true);
                else if (tagEditorOpen_) CloseTagEditor(true);
                else CloseCategoryEditor(true);
                return 0;
            }
            if (button == 1) {
                if (noteEditorOpen_) CloseNoteEditor(false);
                else if (tagEditorOpen_) CloseTagEditor(false);
                else CloseCategoryEditor(false);
                return 0;
            }
            if (tagEditorOpen_) {
                for (const auto& chip : tagLayout.catChips) {
                    if (!rack::PointIn(chip.rect, mx, my)) continue;
                    auto it = std::find(editCats_.begin(), editCats_.end(), chip.index);
                    if (it == editCats_.end()) editCats_.push_back(chip.index);
                    else editCats_.erase(it);
                    Render();
                    return 0;
                }
                for (const auto& chip : tagLayout.tagChips) {
                    if (!rack::PointIn(chip.rect, mx, my)) continue;
                    auto it = std::find(tagSelection_.begin(), tagSelection_.end(), chip.text);
                    if (it != tagSelection_.end()) tagSelection_.erase(it);
                    Render();
                    LayoutEditors();
                    return 0;
                }
            }
            return 0;
        }

        if (HitPlus(mx, my)) {
            OpenNoteEditor();
            return 0;
        }
        const D2D1_RECT_F pill = D2D1::RectF(
            kPad * scale_, SearchTop(), width_ - (kPad + 2.0f * kPlusW + 2.0f * kPlusGap) * scale_,
            SearchTop() + kSearchH * scale_);
        if (rack::PointIn(pill, mx, my)) {
            if (edit_) {
                ActivateForInput();
                SetFocus(edit_);
            }
            return 0;
        }

        const int chip = HitChip(mx, my);
        if (chip >= 0) {
            /* 先不急着切分类：左右拖动是滚动分类条，松手没拖动才算点击。 */
            chipPress_ = chip;
            chipDragging_ = false;
            chipDragStartX_ = mx;
            chipDragScrollStart_ = chipScrollX_;
            SetCapture(hwnd());
            return 0;
        }

        const int row = HitRow(mx, my);
        if (row >= 0) {
            const int action = HitAction(mx, my, row);
            if (action >= 0) {
                const int entryIndex = shown_[static_cast<size_t>(row)];
                const Entry& entry = entries_[static_cast<size_t>(entryIndex)];
                const std::vector<int> actions = ActionsFor(entry);
                if (action < static_cast<int>(actions.size())) {
                    switch (actions[static_cast<size_t>(action)]) {
                    case 0:
                        if (host_ && host_->entry_copy) host_->entry_copy(host_->user, entryIndex);
                        Close();
                        break;
                    case 1:
                        if (host_ && host_->entry_reveal) host_->entry_reveal(host_->user, entryIndex);
                        break;
                    case 2:
                        if (onHangEntry_) {
                            onHangEntry_(entry.kind, entry.label, entry.path, entry.thumb);
                            Close();
                        } else if (entry.kind == SNOWAIR_ENTRY_TEXT && onHangText_) {
                            onHangText_(entry.label);
                            Close();
                        } else if (entry.kind == SNOWAIR_ENTRY_IMAGE && onHangImage_ &&
                                   !entry.path.empty()) {
                            onHangImage_(entry.path);
                            Close();
                        } else if (host_ && host_->entry_hang &&
                                   host_->entry_hang(host_->user, entryIndex)) {
                            Close();
                        }
                        break;
                    default:
                        if (host_ && host_->entry_delete) host_->entry_delete(host_->user, entryIndex);
                        LoadEntries();
                        RefreshCategories();
                        Reload(true);
                        break;
                    }
                }
                return 0;
            }
            const int tag = HitTagChip(mx, my, row);
            if (tag >= 0) {
                OpenTagEditor(shown_[static_cast<size_t>(row)]);
                return 0;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));
        if (chipPress_ >= 0) {
            const bool wasDragging = chipDragging_;
            chipPress_ = -1;
            chipDragging_ = false;
            if (GetCapture() == hwnd()) ReleaseCapture();
            if (!wasDragging) {
                const int chip = HitChip(mx, my);
                if (chip >= 0) {
                    filter_ = chip;
                    scrollY_ = 0.0f;
                    ScrollToShowChip(chip);
                    RebuildShown();
                    Reload(true);
                }
            } else {
                Render();
            }
            return 0;
        }
        if (gearPressed_) {
            gearPressed_ = false;
            if (HitGear(mx, my)) {
                if (settingsOpen_) CloseSettings();
                else OpenSettings();
            }
            return 0;
        }
        if (settingsOpen_ && !categoryEditorOpen_ && HitPlus(mx, my)) {
            OpenNoteEditor();
            return 0;
        }
        if (settingsDrag_ >= 0) {
            const int drag = settingsDrag_;
            const int target = settingsDrop_;
            const bool toHidden = settingsDropHidden_;
            settingsDrag_ = -1;
            settingsDrop_ = -1;
            ReleaseCapture();
            if (drag >= 0 && drag < static_cast<int>(categories_.size())) {
                Category moved = categories_[static_cast<size_t>(drag)];
                moved.hidden = toHidden;
                int insert = target;
                if (insert < 0) {
                    insert = 0;
                    for (int i = static_cast<int>(categories_.size()) - 1; i >= 0; --i) {
                        if (categories_[static_cast<size_t>(i)].hidden == toHidden) {
                            insert = i + 1;
                            break;
                        }
                    }
                } else if (settingsDropAfter_) {
                    insert = target + 1;
                }
                categories_.erase(categories_.begin() + drag);
                if (insert > drag) --insert;
                if (insert < 0) insert = 0;
                if (insert > static_cast<int>(categories_.size()))
                    insert = static_cast<int>(categories_.size());
                categories_.insert(categories_.begin() + insert, std::move(moved));
                SaveCategories();
                RefreshVisibleCategories();
                RebuildShown();
                Reload(true);
            }
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float mx = static_cast<float>(GET_X_LPARAM(lParam));
        const float my = static_cast<float>(GET_Y_LPARAM(lParam));
        {
            D2D1_RECT_F anchor{};
            const std::wstring tip = TooltipAt(mx, my, &anchor);
            if (tip != tooltipText_) {
                tooltipText_ = tip;
                tooltipAnchor_ = anchor;
                tooltipX_ = mx;
                tooltipY_ = my;
                Render();
            }
        }
        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd();
            if (TrackMouseEvent(&tme)) trackingLeave_ = true;
        }
        if (deleteConfirm_ >= 0 || tagEditorOpen_ || categoryEditorOpen_ ||
            noteEditorOpen_) {
            /* 弹窗打开时鼠标只在弹窗上活动：下面的列表不再跟着悬停，
               否则会隔着遮罩亮起来，看起来像是点穿到了下层。 */
            const int button = HitModalButton(mx, my);
            if (button != hoveredModalButton_) {
                hoveredModalButton_ = button;
                Render();
            }
            return 0;
        }
        if (settingsOpen_) {
            if (settingsDrag_ >= 0) {
                dragX_ = mx;
                dragY_ = my;
                bool dropAfter = false;
                settingsDrop_ = ManagerDropAt(mx, my, &dropAfter);
                settingsDropAfter_ = dropAfter;
                settingsDropHidden_ = my >= ManagerCardRect(true).top - 6.0f * scale_;
                Render();
                return 0;
            }
            const int hover = ManagerRowAt(mx, my);
            const bool plus = HitPlus(mx, my) != 0;
            const bool gear = HitGear(mx, my) != 0;
            const bool add = rack::PointIn(SettingsAddRect(), mx, my);
            if (hover != settingsHover_ || plus != hoveredPlus_ || gear != hoveredGear_ ||
                add != hoveredSettingsAdd_) {
                settingsHover_ = hover;
                hoveredPlus_ = plus;
                hoveredGear_ = gear;
                hoveredSettingsAdd_ = add;
                Render();
            }
            return 0;
        }
        if (chipPress_ >= 0) {
            const float dx = mx - chipDragStartX_;
            if (!chipDragging_ && std::abs(dx) > 6.0f * scale_) {
                chipDragging_ = true;
            }
            if (chipDragging_) {
                const float available = width_ - 2.0f * kPad * scale_;
                const float maxScroll = std::max(0.0f, chipContentWidth_ - available);
                chipScrollX_ =
                    rack::Clamp(chipDragScrollStart_ - dx, 0.0f, maxScroll);
                LayoutChips();
                Render();
            }
            return 0;
        }
        const bool gear = HitGear(mx, my) != 0;
        const int row = HitRow(mx, my);
        const int action = HitAction(mx, my, row);
        const int chip = HitChip(mx, my);
        const int tag = HitTagChip(mx, my, row);
        const bool plus = HitPlus(mx, my) != 0;
        if (row != hovered_ || action != hoveredAction_ || chip != hoveredChip_ ||
            tag != hoveredTag_ || plus != hoveredPlus_ || gear != hoveredGear_) {
            hovered_ = row;
            hoveredAction_ = action;
            hoveredChip_ = chip;
            hoveredTag_ = (row >= 0) ? tag : -1;
            hoveredPlus_ = plus;
            hoveredGear_ = gear;
            Render();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (tagEditorOpen_ || categoryEditorOpen_) return 0;
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd(), &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        const float step = 56.0f * scale_;
        if (pt.y >= ChipsTop() - 4.0f * scale_ &&
            pt.y <= ChipsTop() + (kChipH + 4.0f) * scale_) {
            chipScrollX_ -= delta * step;
            const float available = width_ - 2.0f * kPad * scale_;
            const float maxScroll = std::max(0.0f, chipContentWidth_ - available);
            chipScrollX_ = rack::Clamp(chipScrollX_, 0.0f, maxScroll);
            LayoutChips();
        } else {
            scrollY_ -= delta * step;
            const float maxScroll = std::max(0.0f, rowsHeight_ - ListViewportHeight());
            scrollY_ = rack::Clamp(scrollY_, 0.0f, maxScroll);
        }
        Render();
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (!tooltipText_.empty()) {
            tooltipText_.clear();
            Render();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd(), &pt);
        bool hand = false;
        if (tagEditorOpen_) {
            const TagEditorLayout layout = BuildTagEditorLayout();
            hand = rack::PointIn(layout.cancel, static_cast<float>(pt.x), static_cast<float>(pt.y)) ||
                   rack::PointIn(layout.save, static_cast<float>(pt.x), static_cast<float>(pt.y));
            for (const auto& chip : layout.catChips) {
                if (rack::PointIn(chip.rect, static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                    hand = true;
                    break;
                }
            }
            for (const auto& chip : layout.tagChips) {
                if (rack::PointIn(chip.rect, static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                    hand = true;
                    break;
                }
            }
        } else if (deleteConfirm_ >= 0 || categoryEditorOpen_ || noteEditorOpen_) {
            hand = HitModalButton(static_cast<float>(pt.x), static_cast<float>(pt.y)) != 0;
        } else if (!settingsOpen_) {
            hand = HitPlus(static_cast<float>(pt.x), static_cast<float>(pt.y)) != 0 ||
                   HitGear(static_cast<float>(pt.x), static_cast<float>(pt.y)) ||
                   HitChip(static_cast<float>(pt.x), static_cast<float>(pt.y)) >= 0 ||
                   HitRow(static_cast<float>(pt.x), static_cast<float>(pt.y)) >= 0;
        } else {
            hand = HitPlus(static_cast<float>(pt.x), static_cast<float>(pt.y)) != 0 ||
                   HitGear(static_cast<float>(pt.x), static_cast<float>(pt.y)) ||
                   rack::PointIn(SettingsAddRect(), static_cast<float>(pt.x),
                                 static_cast<float>(pt.y));
        }
        if (chipDragging_) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        } else {
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        }
        return TRUE;
    }
    case WM_SETFOCUS:
        return 0;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        PositionIme();
        return LayeredWindow::HandleMessage(message, wParam, lParam);
    case WM_TIMER:
        if (wParam == kCaretTimer) {
            if (searchFocused_) {
                caretVisible_ = !caretVisible_;
                Render();
            }
            return 0;
        }
        return LayeredWindow::HandleMessage(message, wParam, lParam);
    case WM_KILLFOCUS:
        searchFocused_ = false;
        caretVisible_ = false;
        KillTimer(hwnd(), kCaretTimer);
        Render();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (noteEditorOpen_) CloseNoteEditor(false);
            else if (tagEditorOpen_) CloseTagEditor(false);
            else if (categoryEditorOpen_) CloseCategoryEditor(false);
            else if (settingsOpen_) CloseSettings();
            else Close();
        }
        return 0;
    case WM_APP + 1:
        Close();
        return 0;
    case WM_CAPTURECHANGED:
        captured_ = false;
        if (chipPress_ >= 0) {
            chipPress_ = -1;
            chipDragging_ = false;
            Render();
        }
        if (settingsDrag_ >= 0) {
            settingsDrag_ = -1;
            settingsDrop_ = -1;
            Render();
        }
        return 0;
    default:
        return LayeredWindow::HandleMessage(message, wParam, lParam);
    }
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

void ClipboardBubble::EnsureFormats(float scale)
{
    if (!graphics_ || !graphics_->dwrite()) return;
    if (titleFormat_ && std::abs(formatScale_ - scale) < 0.001f) return;
    auto make = [&](float size) {
        ComPtr<IDWriteTextFormat> fmt;
        graphics_->dwrite()->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                              DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size * scale, L"zh-CN",
                                              fmt.GetAddressOf());
        if (fmt) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return fmt;
    };
    titleFormat_ = make(14.0f);
    textFormat_ = make(12.5f);
    smallFormat_ = make(10.0f);
    chipFormat_ = make(12.0f);
    metaFormat_ = make(11.0f);
    tagFormat_ = make(11.0f);
    buttonFormat_ = make(12.0f);
    hintFormat_ = make(12.0f);
    if (hintFormat_) hintFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    bigHintFormat_ = make(13.5f);
    if (bigHintFormat_) {
        bigHintFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bigHintFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (chipFormat_) {
        chipFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        chipFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (metaFormat_) metaFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (tagFormat_) {
        tagFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tagFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (smallFormat_) smallFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (buttonFormat_) {
        buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    titleLineHeight_ = 17.0f * scale;
    formatScale_ = scale;
}
