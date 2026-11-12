#include "Plugin.h"

#include "Clipboard.h"
#include "NoteStore.h"
#include "PinWindow.h"
#include "RackWindow.h"
#include "Util.h"

#include <chrono>
#include <cstdarg>
#include <filesystem>

namespace {

const wchar_t* kLabelEnabled = L"启用晾衣架";
const wchar_t* kDescEnabled = L"截图完成后自动挂到屏幕顶端";
const wchar_t* kLabelHover = L"悬停展开延迟";
const wchar_t* kDescHover = L"鼠标触碰细线后等待多久展开（毫秒）";
const wchar_t* kLabelAutoHide = L"自动收起延迟";
const wchar_t* kDescAutoHide = L"鼠标离开后等待多久收起（毫秒）";
const wchar_t* kLabelAutoRetract = L"自动收回";
const wchar_t* kDescAutoRetract = L"关闭后晾衣架常驻显示，不再自动收起";
const wchar_t* kLabelMaxItems = L"最多挂载数量";
const wchar_t* kDescMaxItems = L"超出后最早的一张会自动落下";

/* 便签在轻雪剪切板里的标签，同时也用它把便签和普通文本区分开。 */
constexpr wchar_t kNoteTag[] = L"便签";

bool IsTextCardType(card::Type type)
{
    return type == card::Type::Note || type == card::Type::Text ||
           type == card::Type::Link || type == card::Type::Color;
}

bool SamePathList(const std::vector<std::wstring>& left,
                  const std::vector<std::wstring>& right)
{
    if (left.size() != right.size()) return false;
    std::vector<bool> used(right.size(), false);
    for (const auto& value : left) {
        bool found = false;
        for (size_t i = 0; i < right.size(); ++i) {
            if (!used[i] && util::SamePath(value, right[i])) {
                used[i] = true;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* 颜色去重键：只忽略 hex 的 # 前缀、大小写和空白，不把 rgb() 和 hex 混为一谈。 */
std::wstring ColorDedupKey(const std::wstring& text)
{
    std::wstring out;
    bool pendingSpace = false;
    for (wchar_t ch : text) {
        if (std::iswspace(ch)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (ch == L'#') continue;
        if (pendingSpace) {
            out.push_back(L' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return out;
}

std::wstring TrimNoteText(std::wstring text)
{
    while (!text.empty() &&
           (text.back() == L'\0' || text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

const SnowAirPlugin kDescriptor = {
    SNOWAIR_PLUGIN_API,
    L"晾衣架",
    SA_RACK_VERSION,
    L"rack",
    [](const SnowAirHost* host) -> bool { return Plugin::Instance().Init(host); },
    []() { Plugin::Instance().Shutdown(); },
    [](const wchar_t* path) { Plugin::Instance().OnCapture(path); },
    [](const wchar_t* path) { Plugin::Instance().OnQuickHang(path); },
    []() -> int { return Plugin::Instance().SettingsCount(); },
    [](int index, SnowAirSettingDesc* out) -> bool {
        return Plugin::Instance().SettingsGet(index, out);
    },
    [](const wchar_t* key) { Plugin::Instance().OnSettingChanged(key); },
};

} /* namespace */

/* HostSim 预览用的调试入口；不参与正常宿主流程。 */
extern "C" __declspec(dllexport) void SnowAirRackDebugHang(int kind, const wchar_t* value)
{
    if (!value) return;
    if (kind == 1) {
        Plugin::Instance().DebugHangFile(value);
    } else {
        Plugin::Instance().DebugHangText(value, kind == 2);
    }
}

Plugin& Plugin::Instance()
{
    static Plugin instance;
    return instance;
}

Plugin::~Plugin()
{
    Shutdown();
}

bool Plugin::Init(const SnowAirHost* host)
{
    if (initialized_) {
        Shutdown();
    }
    if (!host || host->api_version != SNOWAIR_PLUGIN_API) {
        return false;
    }

    host_ = host;
    itemStore_.SetHost(host);
    notes_.SetHost(host);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        RackLog(L"Plugin: CoInitializeEx failed 0x%08X", static_cast<unsigned>(hr));
        host_ = nullptr;
        return false;
    } else {
        RackLog(L"Plugin: host UI thread is not STA; continuing without drag-out support");
    }

    hr = OleInitialize(nullptr);
    if (SUCCEEDED(hr)) {
        oleInitialized_ = true;
    } else {
        RackLog(L"Plugin: OleInitialize failed 0x%08X", static_cast<unsigned>(hr));
    }

    graphics_ = std::make_unique<Graphics>();
    if (!graphics_->Init()) {
        Shutdown();
        return false;
    }

    notes_.Load();
    config_.Load(host_, host_->user);
    theme_.Load(host_, host_->user);
    itemStore_.Load(items_);
    /* 便签正文在轻雪：读完卡片引用后同步正文、迁移旧数据、移除已删除的记录。 */
    SyncNotes();
    nextId_ = 1;
    for (const auto& item : items_) {
        nextId_ = std::max(nextId_, item.id + 1);
    }
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        items_.erase(items_.begin());
    }
    /* 启动时把历史遗留的重复卡片收一收：同样的内容只留最早那张。 */
    {
        std::vector<Item> unique;
        unique.reserve(items_.size());
        int dropped = 0;
        for (auto& item : items_) {
            bool duplicated = false;
            for (const auto& kept : unique) {
                if (kept.kind != item.kind) continue;
                if (item.kind == 1) {
                    duplicated = !item.note.empty() && kept.note == item.note;
                } else if (!item.path.empty()) {
                    duplicated = util::SamePath(kept.path, item.path) ||
                                 (item.contentHash != 0 && kept.contentHash == item.contentHash);
                } else {
                    duplicated = !item.label.empty() && kept.label == item.label;
                }
                if (duplicated) break;
            }
            if (duplicated) {
                ++dropped;
            } else {
                unique.push_back(std::move(item));
            }
        }
        /* 注意：上面的 move 已经把 items_ 里的元素搬空了，这里必须无条件换回来，
           否则剩下的会是「空壳」，保存出去就等于把卡片全删了。 */
        items_ = std::move(unique);
        if (dropped > 0) {
            RackLog(L"启动去重：移除 %d 张重复卡片", dropped);
        }
    }
    /* 老数据没有内容指纹：把补算的结果写回去，下次启动就不用再算。 */
    SaveItems();
    PruneRackQuickCache();

    window_ = std::make_unique<RackWindow>();
    if (!window_->Init(this, graphics_.get())) {
        RackLog(L"Plugin: rack window initialization failed");
        Shutdown();
        return false;
    }
    clipboard_ = std::make_unique<ClipboardBubble>();
    if (!clipboard_->Init(graphics_.get(), host_)) {
        clipboard_.reset();
    } else {
        clipboard_->SetOnClosed([this]() {
            clipboardClosedTick_ = rack::NowMs();
            /* 面板开着时宿主也可能改过计数：收起时补一次重绘，角标不会停在旧数字。 */
            if (window_) window_->RenderNow();
        });
        clipboard_->SetOnEntriesChanged([this]() {
            /* 面板里删了剪切板记录：立刻重画晾衣架，让信封角标上的数字同步减一。 */
            if (window_) window_->RenderNow();
        });
        clipboard_->SetOnHangText([this](const std::wstring& text) { OnHangText(text); });
        clipboard_->SetOnHangImage([this](const std::wstring& path) { OnHangImage(path); });
        clipboard_->SetOnHangEntry(
            [this](int kind, const std::wstring& label, const std::wstring& path,
                   const std::wstring& thumb, const std::wstring& entryId, long long createdAt,
                   const std::vector<std::wstring>& paths, bool isNote) {
                OnHangEntry(kind, label, path, thumb, entryId, createdAt, paths, isNote);
            });
    }

    initialized_ = true;
    window_->OnConfigChanged();
    /* 正常启动不打扰用户：只有宿主刚放好插件时才会请求一次预览。 */
    RackLog(L"晾衣架 %s 已就绪（API %d）", SA_RACK_VERSION, SNOWAIR_PLUGIN_API);
    return true;
}

void Plugin::Shutdown()
{
    if (clipboard_) {
        clipboard_->Close();
        clipboard_.reset();
    }
    if (window_) {
        notes_.Save();
        SaveItems();
        window_->Shutdown();
        window_.reset();
    }
    if (pinWindow_) {
        pinWindow_->ClosePin();
        pinWindow_.reset();
    }
    items_.clear();
    if (graphics_) {
        graphics_->Shutdown();
        graphics_.reset();
    }
    if (oleInitialized_) {
        OleUninitialize();
        oleInitialized_ = false;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    host_ = nullptr;
    initialized_ = false;
}

void Plugin::OnCapture(const wchar_t* path)
{
    if (!initialized_ || !path || !*path) {
        return;
    }
    if (!config_.enabled || !config_.autoCapture) {
        return;
    }
    const std::wstring full = util::FullPath(path);
    if (!util::FileExists(full)) {
        RackLog(L"Capture ignored, file not found: %s", full.c_str());
        return;
    }

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& existing = items_[static_cast<size_t>(i)];
        if (util::SamePath(existing.path, full) || util::SamePath(existing.sourcePath, full)) {
            NudgeExistingItem(i);
            return;
        }
    }

    UINT width = 0, height = 0;
    if (FAILED(graphics_->images().GetSize(full, width, height)) || width == 0 || height == 0) {
        RackLog(L"Capture ignored, cannot read image: %s", full.c_str());
        return;
    }

    Item item;
    item.id = nextId_++;
    item.kind = 0;
    item.cardType = card::Type::Image;
    item.path = full;
    item.paths.push_back(full);
    item.sourcePath = full;
    item.aspect = static_cast<float>(width) / static_cast<float>(height);
    // 截图入场动画由 arrival 负责，不再叠加旧的落位弹簧。
    item.addedMs = 0;
    if (host_->capture_rect && !manualHang_) {
        int cx = 0, cy = 0, cw = 0, ch = 0;
        if (host_->capture_rect(host_->user, &cx, &cy, &cw, &ch) && cw > 0 && ch > 0) {
            item.captureX = cx;
            item.captureY = cy;
            item.captureW = cw;
            item.captureH = ch;
        }
    }
    item.mtime = util::FileWriteTime(full);
    item.contentHash = util::FileContentHash(full);
    if (const NoteEntry* note = notes_.Find(full)) {
        item.note = note->text;
        item.noteColor = note->color;
    }

    if (window_) {
        window_->DismissPreview();
    }
    /* 同一个内容已经在绳子上：不再挂第二张，仅使原卡片轻微晃动作为提示。 */
    if (const int duplicate = FindDuplicateItem(item); duplicate >= 0) {
        NudgeExistingItem(duplicate);
        return;
    }

    items_.push_back(std::move(item));
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        RemoveItemData(0);
    }
    if (window_) {
        window_->OnItemsChanged(true);
    }
    SaveItems();
    RackLog(L"Capture added: %s (items=%zu)", full.c_str(), items_.size());
}

void Plugin::OnHangImage(const std::wstring& path, const std::wstring& svgId)
{
    if (!initialized_ || path.empty()) return;
    const std::wstring full = util::FullPath(path);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (util::SamePath(items_[static_cast<size_t>(i)].path, full)) {
            if (!svgId.empty() && items_[static_cast<size_t>(i)].svgId.empty()) {
                items_[static_cast<size_t>(i)].svgId = svgId;
                SaveItems();
            }
            NudgeExistingItem(i);
            return;
        }
    }
    UINT width = 0, height = 0;
    float aspect = 1.4f;
    if (graphics_ && SUCCEEDED(graphics_->images().GetSize(full, width, height)) && width > 0 &&
        height > 0) {
        aspect = static_cast<float>(width) / static_cast<float>(height);
    }
    Item item;
    item.id = nextId_++;
    item.kind = 0;
    item.cardType = svgId.empty() ? card::Type::Image : card::Type::Svg;
    item.path = full;
    item.paths.push_back(full);
    item.sourcePath = full;
    item.svgId = svgId;
    // 快速上绳的 RackQuick 临时图归插件所有：卡片取下时直接删文件，不留在缓存目录里。
    if (_wcsicmp(std::filesystem::path(full).parent_path().filename().c_str(), L"RackQuick") == 0) {
        item.ownsFile = true;
    }
    item.aspect = aspect;
    item.addedMs = 0;
    item.mtime = util::FileWriteTime(full);
    item.contentHash = util::FileContentHash(full);
    /* 同一个内容已经在绳子上：不再挂第二张，仅使原卡片轻微晃动作为提示。 */
    if (const int duplicate = FindDuplicateItem(item); duplicate >= 0) {
        if (!svgId.empty() && items_[static_cast<size_t>(duplicate)].svgId.empty()) {
            items_[static_cast<size_t>(duplicate)].svgId = svgId;
            SaveItems();
        }
        NudgeExistingItem(duplicate);
        return;
    }

    items_.push_back(std::move(item));
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        RemoveItemData(0);
    }
    if (window_) {
        /* OnItemsChanged 会启动出场动画并顺带展开绳子。之前先展开的话，新卡片会
           有一帧出现在终点、再被拽回光标位置，看起来像闪了一下。 */
        window_->OnItemsChanged(true);
    }
    SaveItems();
}

void Plugin::OnHangEntry(int kind, const std::wstring& label, const std::wstring& path,
                         const std::wstring& thumb, const std::wstring& entryId,
                         long long createdAt, const std::vector<std::wstring>& paths, bool isNote)
{
    if (!initialized_) return;
    /* 轻雪把剪切板里的 SVG 代码渲染成了 PNG（thumb）；上绳按图片处理，
       和轻雪历史卡片看到的是同一张图。普通文字没有 thumb，走文本/链接/颜色/便签。 */
    if (kind == SNOWAIR_ENTRY_TEXT && !thumb.empty() && util::FileExists(thumb)) {
        /* 轻雪已经渲染好 PNG 预览；卡片只记记录 id，复制/拖拽时按 id 读回 SVG 代码。 */
        OnHangImage(thumb, entryId);
        return;
    }
    if (kind == SNOWAIR_ENTRY_TEXT) {
        /* 文本条目本身就是轻雪的记录：卡片只记 id，正文从轻雪读，不另存一份。 */
        if (entryId.empty()) {
            OnHangText(label);
            return;
        }
        if (const int existing = FindNoteItem(entryId); existing >= 0) {
            /* 记录没变，但类型可能在旧版本里被当成便签；同时纠正。 */
            Item& item = items_[static_cast<size_t>(existing)];
            const card::Type type = isNote ? card::Type::Note : card::TextType(item.note);
            if (item.cardType != type || (createdAt > 0 && item.createdAt <= 0)) {
                item.cardType = type;
                if (createdAt > 0 && item.createdAt <= 0) item.createdAt = createdAt;
                SaveItems();
            }
            NudgeExistingItem(existing);
            return;
        }
        std::wstring text;
        if (!ReadNoteRecord(entryId, text) || text.empty()) return;
        const card::Type type = isNote ? card::Type::Note : card::TextType(text);
        AddNoteItem(text, entryId, type, createdAt);
        return;
    }

    Item item;
    item.id = nextId_++;
    item.addedMs = 0;
    item.label = label;
    item.createdAt = createdAt;
    for (const auto& file : paths) {
        if (!file.empty()) item.paths.push_back(util::FullPath(file));
    }
    if (item.paths.empty() && !path.empty()) item.paths.push_back(util::FullPath(path));
    item.path = item.paths.empty() ? std::wstring() : item.paths.front();
    if (kind == SNOWAIR_ENTRY_IMAGE) {
        if (item.path.empty()) return;
        item.kind = 0;
        item.cardType = card::Type::Image;
        item.sourcePath = item.path;
        item.mtime = util::FileWriteTime(item.path);
        item.contentHash = util::FileContentHash(item.path);

        UINT width = 0, height = 0;
        if (graphics_ && SUCCEEDED(graphics_->images().GetSize(item.path, width, height)) &&
            width > 0 && height > 0) {
            item.aspect = static_cast<float>(width) / static_cast<float>(height);
        } else {
            item.aspect = 1.4f;
        }
    } else if (kind == SNOWAIR_ENTRY_MEDIA) {
        /* 录屏：有封面沿用照片卡；没封面用新的视频卡。 */
        item.kind = 2;
        item.aspect = 16.0f / 9.0f;
        if (!item.path.empty()) {
            item.sourcePath = item.path;
            item.mtime = util::FileWriteTime(item.path);
            item.contentHash = util::FileContentHash(item.path);
        }
        item.cardType = card::Type::Video;
        const std::wstring cover = thumb.empty() ? std::wstring() : util::FullPath(thumb);
        if (!cover.empty() && util::FileExists(cover)) {
            item.thumbPath = cover;
            item.cardType = card::Type::Recording;
            UINT width = 0, height = 0;
            if (graphics_ &&
                SUCCEEDED(graphics_->images().GetSize(cover, width, height)) &&
                width > 0 && height > 0) {
                item.aspect = static_cast<float>(width) / static_cast<float>(height);
            }
        }

    } else {
        /* 文件 / 音频 / 视频：新卡片；混搭一律按文件。 */
        item.kind = 3;
        item.aspect = 1.0f;
        item.cardType = card::FileType(item.paths);
        if (!item.path.empty()) {
            item.sourcePath = item.path;
            item.mtime = util::FileWriteTime(item.path);
            item.contentHash = util::FileContentHash(item.path);
        }
        if (item.label.empty() && !item.path.empty()) {
            item.label = std::filesystem::path(item.path).filename().wstring();
        }
        if (item.label.empty() && item.paths.empty()) return;
    }

    /* 同一个内容已经在绳子上：不再挂第二张，仅使原卡片轻微晃动作为提示。 */
    if (const int duplicate = FindDuplicateItem(item); duplicate >= 0) {
        NudgeExistingItem(duplicate);
        return;
    }

    items_.push_back(std::move(item));
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        RemoveItemData(0);
    }
    if (window_) {
        window_->OnItemsChanged(true);
    }
    SaveItems();
}

void Plugin::PinImageFile(const std::wstring& path)
{
    if (path.empty()) {
        return;
    }
    using PinImageFileFn = void (*)(const wchar_t*);
    static PinImageFileFn hostPin = []() -> PinImageFileFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinImageFileFn>(
                          GetProcAddress(host, "SnowAirPinImageFile"))
                    : nullptr;
    }();
    if (hostPin) {
        hostPin(path.c_str());
        return;
    }
    if (!graphics_) {
        return;
    }
    if (!pinWindow_) {
        pinWindow_ = std::make_unique<PinWindow>();
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    pinWindow_->Open(graphics_.get(), path, cursor);
}

void Plugin::PinTextToDesktop(const std::wstring& text)
{
    if (text.empty()) return;
    using PinTextFn = void (*)(const wchar_t*);
    static PinTextFn hostPin = []() -> PinTextFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinTextFn>(
                          GetProcAddress(host, "SnowAirPinText"))
                    : nullptr;
    }();
    if (hostPin) hostPin(text.c_str());
}

void Plugin::PinFilesToDesktop(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return;
    using PinFilesFn = void (*)(const wchar_t* const*, int);
    static PinFilesFn hostPin = []() -> PinFilesFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinFilesFn>(
                          GetProcAddress(host, "SnowAirPinFiles"))
                    : nullptr;
    }();
    if (!hostPin) return;
    std::vector<const wchar_t*> raw;
    raw.reserve(paths.size());
    for (const auto& path : paths) {
        if (!path.empty()) raw.push_back(path.c_str());
    }
    if (!raw.empty()) hostPin(raw.data(), static_cast<int>(raw.size()));
}

void Plugin::PinImageFileAt(const std::wstring& path, int centerX, int topY)
{
    using PinAtFn = void (*)(const wchar_t*, int, int);
    static PinAtFn hostPin = []() -> PinAtFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinAtFn>(
                          GetProcAddress(host, "SnowAirPinImageFileAt"))
                    : nullptr;
    }();
    if (hostPin) {
        hostPin(path.c_str(), centerX, topY);
    } else {
        PinImageFile(path);
    }
}

void Plugin::PinTextToDesktopAt(const std::wstring& text, int centerX, int topY)
{
    using PinAtFn = void (*)(const wchar_t*, int, int);
    static PinAtFn hostPin = []() -> PinAtFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinAtFn>(
                          GetProcAddress(host, "SnowAirPinTextAt"))
                    : nullptr;
    }();
    if (hostPin) {
        hostPin(text.c_str(), centerX, topY);
    } else {
        PinTextToDesktop(text);
    }
}

void Plugin::PinFilesToDesktopAt(const std::vector<std::wstring>& paths, int centerX, int topY)
{
    using PinAtFn = void (*)(const wchar_t* const*, int, int, int);
    static PinAtFn hostPin = []() -> PinAtFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinAtFn>(
                          GetProcAddress(host, "SnowAirPinFilesAt"))
                    : nullptr;
    }();
    if (!hostPin) {
        PinFilesToDesktop(paths);
        return;
    }
    std::vector<const wchar_t*> raw;
    raw.reserve(paths.size());
    for (const auto& path : paths) {
        if (!path.empty()) raw.push_back(path.c_str());
    }
    if (!raw.empty()) {
        hostPin(raw.data(), static_cast<int>(raw.size()), centerX, topY);
    }
}

void Plugin::DebugHangText(const std::wstring& text, bool note)
{
    if (!initialized_ || text.empty()) return;
    const card::Type type = note ? card::Type::Note : card::TextType(text);
    AddNoteItem(text, std::wstring(), type,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
    if (window_) window_->OnItemsChanged(true);
}

void Plugin::DebugHangFile(const std::wstring& path)
{
    if (!initialized_ || path.empty()) return;
    Item item;
    item.id = nextId_++;
    item.kind = 3;
    item.path = util::FullPath(path);
    item.paths.push_back(item.path);
    item.sourcePath = item.path;
    item.label = std::filesystem::path(item.path).filename().wstring();
    item.cardType = card::FileType(item.paths);
    item.createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    item.aspect = 1.0f;
    item.mtime = util::FileWriteTime(item.path);
    item.contentHash = util::FileContentHash(item.path);
    items_.push_back(std::move(item));
    if (window_) window_->OnItemsChanged(true);
    SaveItems();
}

void Plugin::OnHangText(const std::wstring& text)
{
    if (!initialized_) return;
    const std::wstring body = TrimNoteText(text);
    if (body.empty()) return;
    /* 同样内容的便签已在绳子上：轻微晃动提示，不再挂第二张。 */
    {
        Item probe;
        probe.kind = 1;
        probe.note = body;
        if (const int existing = FindDuplicateItem(probe); existing >= 0) {
            NudgeExistingItem(existing);
            return;
        }
    }
    std::wstring noteId;
    if (!CreateNoteRecord(body, noteId)) {
        RackLog(L"便签没能写进轻雪，已放弃上绳");
        return;
    }
    AddNoteItem(body, noteId);
}

void Plugin::AddNoteItem(const std::wstring& text, const std::wstring& noteId, card::Type type,
                         long long createdAt)
{
    Item item;
    item.id = nextId_++;
    item.kind = 1; /* 文字类 */
    item.cardType = type;
    item.note = text;
    item.noteId = noteId;
    item.noteColor = type == card::Type::Note ? 2 : 0;
    item.createdAt = createdAt;
    if (item.createdAt <= 0) {
        item.createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    }
    item.aspect = 1.0f;
    item.addedMs = 0;   /* no drop-in spring: avoids a flicker on landing */
    /* 同一个内容已经在绳子上：不再挂第二张，仅使原卡片轻微晃动作为提示。 */
    if (const int duplicate = FindDuplicateItem(item); duplicate >= 0) {
        NudgeExistingItem(duplicate);
        return;
    }

    items_.push_back(std::move(item));
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        /* 超出上限：卡片落下，但轻雪里的记录保留 */
        RemoveItemData(0);
    }
    if (window_) {
        window_->OnItemsChanged(true);
    }
    SaveItems();
    RackLog(L"便签上绳 (items=%zu)", items_.size());
}

bool Plugin::CreateNoteRecord(const std::wstring& text, std::wstring& noteId)
{
    noteId.clear();
    if (!host_ || !host_->entry_create_text) return false;
    wchar_t id[128]{};
    if (!host_->entry_create_text(host_->user, text.c_str(), kNoteTag, id,
                                  static_cast<int>(std::size(id)))) {
        return false;
    }
    noteId = id;
    if (host_->entry_refresh) host_->entry_refresh(host_->user);
    return !noteId.empty();
}

bool Plugin::ReadNoteRecord(const std::wstring& noteId, std::wstring& text)
{
    text.clear();
    if (!host_ || !host_->entry_text || noteId.empty()) return false;
    wchar_t probe[512]{};
    const int length = host_->entry_text(host_->user, noteId.c_str(), probe,
                                         static_cast<int>(std::size(probe)));
    if (length < 0) return false;
    if (length < static_cast<int>(std::size(probe))) {
        text.assign(probe, static_cast<size_t>(length));
        return true;
    }
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    const int got = host_->entry_text(host_->user, noteId.c_str(), buffer.data(), length + 1);
    if (got < 0) return false;
    buffer.resize(static_cast<size_t>(std::min(got, length)));
    text = std::move(buffer);
    return true;
}

bool Plugin::UpdateNoteRecord(const std::wstring& noteId, const std::wstring& text)
{
    if (!host_ || !host_->entry_update_text || noteId.empty()) return false;
    return host_->entry_update_text(host_->user, noteId.c_str(), text.c_str(), nullptr) != 0;
}

std::wstring Plugin::SvgText(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) return {};
    const Item& item = items_[static_cast<size_t>(index)];
    if (item.svgId.empty()) return {};
    std::wstring text;
    if (!ReadNoteRecord(item.svgId, text)) return {};
    return text;
}

int Plugin::FindHostEntryById(const std::wstring& id) const
{
    if (!host_ || id.empty() || !host_->entry_count || !host_->entry_id) return -1;
    const int count = host_->entry_count(host_->user);
    for (int i = 0; i < count; ++i) {
        wchar_t buffer[128]{};
        if (host_->entry_id(host_->user, i, buffer, static_cast<int>(std::size(buffer))) &&
            id == buffer) {
            return i;
        }
    }
    return -1;
}

bool Plugin::CopyCardText(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size()) || !host_) return false;
    const Item& item = items_[static_cast<size_t>(index)];
    /* 单色颜色卡拖出：复制该颜色本身（带 alpha 用 rgba），不是整段原文。 */
    if (item.cardType == card::Type::Color) {
        std::vector<card::Rgba> colors;
        if (card::ParseColors(item.note, colors) && colors.size() == 1) {
            return clipboard::CopyText(card::ColorDragText(colors.front()));
        }
    }
    const std::wstring id = !item.svgId.empty() ? item.svgId : item.noteId;
    /* 优先走宿主的 entry_copy：它会 markOwnClipboardWrite，历史不会新增/置顶。 */
    const int hostIndex = FindHostEntryById(id);
    if (hostIndex >= 0 && host_->entry_copy) {
        host_->entry_copy(host_->user, hostIndex);
        return true;
    }
    /* 旧宿主 / 记录已删除：只能直接写剪贴板。 */
    const std::wstring text = !item.svgId.empty() ? SvgText(index) : item.note;
    if (text.empty()) return false;
    return clipboard::CopyText(text);
}

int Plugin::FindDuplicateItem(const Item& item) const
{
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& existing = items_[static_cast<size_t>(i)];
        if (existing.kind != item.kind) {
            continue;
        }
        if (IsTextCardType(item.cardType) || item.kind == 1) {
            /* 颜色卡：#34C759 与 34C759 是同一个色值，不重复上绳；
               但 rgb(...) 和 hex 的卡片仍按不同格式保留。 */
            if (item.cardType == card::Type::Color &&
                existing.cardType == card::Type::Color && !item.note.empty() &&
                ColorDedupKey(existing.note) == ColorDedupKey(item.note)) {
                return i;
            }
            /* 文字类比正文：同样的文字不重复挂（便签/文本/链接/颜色互斥）。 */
            if (!item.note.empty() && existing.note == item.note) {
                return i;
            }
        } else if (!item.paths.empty()) {
            if (SamePathList(existing.paths, item.paths)) {
                return i;
            }
            /* 单文件路径不同但内容指纹相同：同一张图放在不同位置，也算重复。 */
            if (item.paths.size() == 1 && item.contentHash != 0 &&
                existing.contentHash == item.contentHash) {
                return i;
            }
        } else if (!item.path.empty() && util::SamePath(existing.path, item.path)) {
            return i;
        } else if (!item.label.empty() && existing.label == item.label) {
            return i;
        }
    }
    return -1;
}

void Plugin::NudgeExistingItem(int index)
{
    if (!window_ || index < 0) {
        return;
    }
    window_->Expand(true);
    window_->ShakeCard(index);
}

int Plugin::FindNoteItem(const std::wstring& noteId) const
{
    if (noteId.empty()) return -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].kind == 1 && items_[i].noteId == noteId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Plugin::SyncNotes()
{
    bool changed = false;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->kind != 1) {
            ++it;
            continue;
        }
        if (it->noteId.empty()) {
            /* 旧版本把正文存在本地：迁移成轻雪的记录，本地只留 id */
            const std::wstring legacy = TrimNoteText(it->note);
            if (legacy.empty()) {
                it = items_.erase(it);
                changed = true;
                continue;
            }
            std::wstring noteId;
            if (!CreateNoteRecord(legacy, noteId)) {
                ++it; /* 写不进去就先留着，下次启动再试 */
                continue;
            }
            it->noteId = noteId;
            it->note = legacy;
            changed = true;
            ++it;
            continue;
        }
        std::wstring text;
        if (!ReadNoteRecord(it->noteId, text) || text.empty()) {
            /* 轻雪那边的记录被删了：卡片跟着消失 */
            RackLog(L"便签记录已不在轻雪，移除卡片 %s", it->noteId.c_str());
            it = items_.erase(it);
            changed = true;
            continue;
        }
        if (it->note != text) {
            it->note = text;
            changed = true;
        }
        if (it->createdAt <= 0 && host_ && host_->entry_get) {
            const int hostIndex = FindHostEntryById(it->noteId);
            if (hostIndex >= 0) {
                SnowAirEntry entry{};
                if (host_->entry_get(host_->user, hostIndex, &entry) && entry.created_at > 0) {
                    it->createdAt = entry.created_at;
                    changed = true;
                }
            }
        }
        /* 文本/链接/颜色可能被外部改过内容：保持类型标签跟内容一致；便签永远还是便签。 */
        if (IsTextCardType(it->cardType) && it->cardType != card::Type::Note) {
            const card::Type type = card::TextType(text);
            if (it->cardType != type) {
                it->cardType = type;
                changed = true;
            }
        }
        ++it;
    }
    if (changed) {
        SaveItems();
    }
}

void Plugin::OnQuickHang(const wchar_t* path)
{
    /* 只记路径，不复制文件：图片本身归轻雪所有，插件不另存一份。 */
    const bool autoCapture = config_.autoCapture;
    config_.autoCapture = true;
    manualHang_ = true;
    OnCapture(path);
    manualHang_ = false;
    config_.autoCapture = autoCapture;
}

bool Plugin::QuickHang(int kind, const std::wstring& text,
                       const std::vector<std::wstring>& paths)
{
    if (!initialized_) return false;
    if (kind == SNOWAIR_ENTRY_TEXT) {
        const std::wstring body = TrimNoteText(text);
        if (body.empty()) return false;
        std::wstring entryId;
        if (host_ && host_->entry_create_text) {
            wchar_t id[128]{};
            if (host_->entry_create_text(host_->user, body.c_str(), L"", id,
                                         static_cast<int>(std::size(id)))) {
                entryId = id;
            }
        }
        if (!entryId.empty()) {
            const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
            OnHangEntry(SNOWAIR_ENTRY_TEXT, body, L"", L"", entryId, now, {}, false);
            return true;
        }
        DebugHangText(body, false);
        return true;
    }

    if (kind == SNOWAIR_ENTRY_FILES) {
        if (paths.empty()) return false;
        Item item;
        item.id = nextId_++;
        item.kind = 3;
        for (const auto& path : paths) {
            if (path.empty()) continue;
            item.paths.push_back(util::FullPath(path));
        }
        if (item.paths.empty()) return false;
        item.path = item.paths.front();
        item.label = std::filesystem::path(item.path).filename().wstring();
        item.createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        item.aspect = 1.0f;
        item.sourcePath = item.path;
        item.mtime = util::FileWriteTime(item.path);
        item.contentHash = util::FileContentHash(item.path);
        item.cardType = card::FileType(item.paths);
        if (const int duplicate = FindDuplicateItem(item); duplicate >= 0) {
            NudgeExistingItem(duplicate);
            return true;
        }
        items_.push_back(std::move(item));
        while (static_cast<int>(items_.size()) > config_.maxItems) {
            RemoveItemData(0);
        }
        if (window_) window_->OnItemsChanged(true);
        SaveItems();
        return true;
    }
    return false;
}

void Plugin::OnSettingChanged(const wchar_t* key)
{
    if (!initialized_ || !host_) {
        return;
    }
    config_.Load(host_, host_->user);
    const bool themeChanged =
        key && (_wcsicmp(key, L"theme") == 0 || _wcsicmp(key, L"themes") == 0 ||
                _wcsicmp(key, L"rack.theme") == 0);
    if (themeChanged || !key) {
        theme_.Load(host_, host_->user);
    }
    if (key && _wcsicmp(key, config_keys::kMaxItems) == 0) {
        TrimItems();
    }
    if (window_ && key && _wcsicmp(key, L"hoverPreview") == 0) {
        window_->DismissPreview();
    }
    if (window_ && key && _wcsicmp(key, L"rack.intro") == 0) {
        window_->Expand(true);
    }
    if (window_ && key && _wcsicmp(key, L"rack.toggle") == 0) {
        window_->ToggleVisibility();
        return;
    }
    if (window_) {
        if (themeChanged) {
            window_->OnThemeChanged();
            if (clipboard_)
                clipboard_->ApplyTheme(host_ ? Theme::HostUi(host_, host_->user)
                                             : Theme::MenuStyle(theme_.dark));
        }
        window_->OnConfigChanged();
    }
}

int Plugin::SettingsCount() const
{
    return 5;
}

bool Plugin::SettingsGet(int index, SnowAirSettingDesc* out) const
{
    if (!out) {
        return false;
    }
    SnowAirSettingDesc desc{};
    switch (index) {
    case 0:
        desc.key = config_keys::kEnabled;
        desc.label = kLabelEnabled;
        desc.desc = kDescEnabled;
        desc.type = SA_SET_BOOL;
        desc.default_bool = 1;
        desc.min_value = 0;
        desc.max_value = 1;
        break;
    case 1:
        desc.key = config_keys::kHoverDelay;
        desc.label = kLabelHover;
        desc.desc = kDescHover;
        desc.type = SA_SET_INT;
        desc.default_bool = 200;
        desc.min_value = 0;
        desc.max_value = 2000;
        break;
    case 2:
        desc.key = config_keys::kAutoHide;
        desc.label = kLabelAutoHide;
        desc.desc = kDescAutoHide;
        desc.type = SA_SET_INT;
        desc.default_bool = 700;
        desc.min_value = 0;
        desc.max_value = 5000;
        break;
    case 3:
        desc.key = config_keys::kMaxItems;
        desc.label = kLabelMaxItems;
        desc.desc = kDescMaxItems;
        desc.type = SA_SET_INT;
        desc.default_bool = 8;
        desc.min_value = 1;
        desc.max_value = 30;
        break;
    case 4:
        desc.key = config_keys::kAutoRetract;
        desc.label = kLabelAutoRetract;
        desc.desc = kDescAutoRetract;
        desc.type = SA_SET_BOOL;
        desc.default_bool = 1;
        desc.min_value = 0;
        desc.max_value = 1;
        break;
    default:
        return false;
    }
    *out = desc;
    return true;
}

void Plugin::PruneRackQuickCache()
{
    wchar_t appData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) {
        return;
    }
    std::filesystem::path dir{appData};
    dir /= L"SnowAir";
    dir /= L"RackQuick";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return;
    }

    std::vector<std::wstring> keep;
    for (Item& item : items_) {
        if (!item.path.empty() &&
            _wcsicmp(std::filesystem::path(item.path).parent_path().filename().c_str(),
                     L"RackQuick") == 0) {
            item.ownsFile = true;
            keep.push_back(item.path);
        }
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        const std::wstring path = entry.path().wstring();
        if (_wcsicmp(entry.path().extension().c_str(), L".png") != 0) continue;
        if (entry.path().filename().wstring().rfind(L"quick-", 0) != 0) continue;
        bool referenced = false;
        for (const auto& kept : keep) {
            if (util::SamePath(kept, path)) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            std::error_code removeEc;
            std::filesystem::remove(entry.path(), removeEc);
        }
    }
}

void Plugin::RemoveItemData(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    Item& item = items_[static_cast<size_t>(index)];
    if (item.ownsFile) {
        util::DeleteFileIfExists(item.path);
    }
    if (!item.editPath.empty() && !util::SamePath(item.editPath, item.path)) {
        util::DeleteFileIfExists(item.editPath);
    }
    const std::wstring noteKey = item.sourcePath.empty() ? item.path : item.sourcePath;
    notes_.Remove(noteKey);
    items_.erase(items_.begin() + index);
}

void Plugin::RemoveItemAt(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    if (window_) {
        window_->DismissPreview();
    }
    RemoveItemData(index);
    if (window_) {
        window_->OnItemsChanged(false);
    }
    SaveItems();
}

void Plugin::SetItemNote(int index, const std::wstring& text, int color)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    Item& item = items_[static_cast<size_t>(index)];
    if (item.kind == 1) {
        /* 便签：正文写回轻雪的对应记录，插件只留缓存。 */
        const std::wstring body = TrimNoteText(text);
        if (!UpdateNoteRecord(item.noteId, body)) {
            /* 记录被删掉了就补一条，卡片保持可用 */
            std::wstring noteId;
            if (!CreateNoteRecord(body, noteId)) {
                RackLog(L"便签保存失败：轻雪里写不进记录");
                return;
            }
            item.noteId = noteId;
        }
        item.note = body;
        item.noteColor = rack::ClampInt(color, 0, 2);
        /* 内容改完重新判定类型标签：链接不再是链接 → 文本，文本变成 URL → 链接。
           便签永远保持便签；强调色不随类型变化（item.accent 不动）。 */
        if (item.cardType != card::Type::Note) {
            item.cardType = card::TextType(body);
        }
        if (window_) {
            if (item.note.empty()) {
                window_->DismissPreview();
            } else {
                window_->RefreshPreview();
            }
            window_->RenderNow();
        }
        SaveItems();
        return;
    }
    item.note = text;
    item.noteColor = rack::ClampInt(color, 0, 2);
    const std::wstring key = item.sourcePath.empty() ? item.path : item.sourcePath;
    NoteEntry entry;
    entry.text = item.note;
    entry.color = item.noteColor;
    notes_.Set(key, entry);
    if (window_) {
        if (item.note.empty()) {
            window_->DismissPreview();
        } else {
            window_->RefreshPreview();
        }
        window_->RenderNow();
    }
    SaveItems();
}

void Plugin::TrimItems()
{
    bool changed = false;
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        if (window_) {
            window_->DismissPreview();
        }
        RemoveItemData(0);
        changed = true;
    }
    if (changed && window_) {
        window_->OnItemsChanged(false);
    }
    if (changed) {
        SaveItems();
    }
}

int Plugin::IndexById(int id) const
{
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Item* Plugin::ItemById(int id)
{
    const int index = IndexById(id);
    return index >= 0 ? &items_[static_cast<size_t>(index)] : nullptr;
}

/* 记录由轻雪统一负责，这里只负责读数量。 */
int Plugin::HistoryCount()
{
    return host_ && host_->history_count ? host_->history_count(host_->user) : 0;
}

/* 轻雪把「停用历史记录」存在 features.disableCaptureHistory 下。宿主没有
   专门的查询字段，但 config_get_bool 读的就是 features 里的键，所以能读到；
   这不是 rack.* 命名空间，属于对宿主配置的依赖，键名变了要跟着改。 */
bool Plugin::HistoryDisabled() const
{
    return host_ && host_->config_get_bool &&
           host_->config_get_bool(host_->user, L"disableCaptureHistory", false);
}

int Plugin::ClipboardCount()
{
    return host_ && host_->clipboard_count ? host_->clipboard_count(host_->user) : 0;
}

bool Plugin::HoverPreviewEnabled()
{
    return !host_ || !host_->hover_preview || host_->hover_preview(host_->user) != 0;
}

bool Plugin::PreviewPinned()
{
    return host_ && host_->rack_preview_pinned &&
           host_->rack_preview_pinned(host_->user) != 0;
}

bool Plugin::MenuOpen()
{
    return host_ && host_->menu_open && host_->menu_open(host_->user) != 0;
}

bool Plugin::ClipboardOpen()
{
    return clipboard_ && clipboard_->isOpen();
}

bool Plugin::ClipboardContains(POINT screen)
{
    return clipboard_ && clipboard_->isOpen() && clipboard_->ContainsPoint(screen);
}

void Plugin::CloseMenu()
{
    if (host_ && host_->close_menu) host_->close_menu(host_->user);
}

void Plugin::OpenHistory()
{
    /* 历史入口已停用：即使命中测试延迟，也不再打开。 */
    if (HistoryDisabled()) {
        return;
    }
    if (host_ && host_->open_history) {
        host_->open_history(host_->user);
    }
}

void Plugin::OpenClipboard(int screenX, int screenY)
{
    if (!clipboard_) {
        return;
    }
    // 点信封是开关：再点一次收起面板。外面点击的钩子可能已经先把它关掉了，
    // 所以紧跟在关闭之后的那次信封点击要忽略。
    if (clipboard_->isOpen()) {
        clipboard_->Close();
        return;
    }
    if (clipboardClosedTick_ != 0 && rack::NowMs() - clipboardClosedTick_ < 300) {
        clipboardClosedTick_ = 0;
        return;
    }
    clipboardClosedTick_ = 0;
    SyncHostTheme();
    const HMONITOR monitor =
        MonitorFromPoint(POINT{screenX, screenY}, MONITOR_DEFAULTTONEAREST);
    const float scale = util::ScaleForMonitor(monitor);
    const bool dark = HostThemeIsDark();
    clipboard_->Open(screenX, screenY, scale,
                     host_ ? Theme::HostUi(host_, host_->user) : Theme::MenuStyle(dark));
    /* 面板可能盖住信封角标的一部分：打开时按最新计数重画一次，避免先看到旧数字。 */
    if (window_) window_->RenderNow();
}

bool Plugin::HostThemeIsDark() const
{
    bool dark = theme_.dark;
    if (!host_) return dark;
    int mode = -1;
    if (host_->config_get_int) {
        mode = host_->config_get_int(host_->user, L"appTheme", -1);
    }
    if (mode == 2) return true;
    if (mode == 1) return false;
    if (mode == 0) {
        DWORD light = 1;
        DWORD size = sizeof(light);
        if (RegGetValueW(HKEY_CURRENT_USER,
                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light,
                         &size) == ERROR_SUCCESS) {
            return light == 0;
        }
        return false;
    }
    if (host_->is_dark) return host_->is_dark(host_->user) != 0;
    if (host_->theme_color) {
        const uint32_t bg = host_->theme_color(host_->user, SA_THEME_CANVAS);
        const float luma = 0.2126f * ((bg >> 24) & 0xFF) +
                           0.7152f * ((bg >> 16) & 0xFF) +
                           0.0722f * ((bg >> 8) & 0xFF);
        return luma < 128.0f;
    }
    return dark;
}

void Plugin::SyncHostTheme()
{
    if (!host_) return;
    const bool dark = HostThemeIsDark();
    if (lastHostDark_ == static_cast<int>(dark)) return;
    lastHostDark_ = static_cast<int>(dark);
    theme_.Load(host_, host_->user);
    if (clipboard_)
        clipboard_->ApplyTheme(host_ ? Theme::HostUi(host_, host_->user)
                                     : Theme::MenuStyle(dark));
    if (window_) window_->OnThemeChanged();
}

void Plugin::RefreshTheme()
{
    if (host_) {
        theme_.Load(host_, host_->user);
    }
    if (clipboard_)
        clipboard_->ApplyTheme(host_ ? Theme::HostUi(host_, host_->user)
                                     : Theme::MenuStyle(theme_.dark));
}

void Plugin::SaveNotes()
{
    notes_.Save();
}

void Plugin::SaveItems()
{
    itemStore_.Save(items_);
}

void Plugin::LogMessage(const wchar_t* text)
{
    if (!text) {
        return;
    }
    if (host_ && host_->log) {
        host_->log(host_->user, text);
    }
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");
}

void RackLog(const wchar_t* format, ...)
{
    wchar_t buffer[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    Plugin::Instance().LogMessage(buffer);
}

/* 可选导出：宿主快速贴屏把文字/文件交给晾衣架。旧宿主没有这个符号时会自动
   退回系统贴屏，不影响兼容性。 */
extern "C" __declspec(dllexport) int SnowAirRackQuickHang(
    int kind, const wchar_t* text, const wchar_t* const* paths, int count)
{
    std::vector<std::wstring> list;
    if (paths && count > 0) {
        list.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (paths[i]) list.emplace_back(paths[i]);
        }
    }
    return Plugin::Instance().QuickHang(kind, text ? text : L"", list) ? 1 : 0;
}

extern "C" SNOWAIR_PLUGIN_EXPORT const SnowAirPlugin* SnowAirPluginEntry(void)
{
    return &kDescriptor;
}

