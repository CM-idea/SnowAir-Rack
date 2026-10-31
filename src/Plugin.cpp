#include "Plugin.h"

#include "NoteStore.h"
#include "PinWindow.h"
#include "RackWindow.h"
#include "Util.h"

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
    /* 便签正文在轻雪：读完卡片引用后同步正文、迁移旧数据、丢掉已被删除的记录。 */
    SyncNotes();
    nextId_ = 1;
    for (const auto& item : items_) {
        nextId_ = std::max(nextId_, item.id + 1);
    }
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        items_.erase(items_.begin());
    }

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
        clipboard_->SetOnClosed([this]() { clipboardClosedTick_ = rack::NowMs(); });
        clipboard_->SetOnHangText([this](const std::wstring& text) { OnHangText(text); });
        clipboard_->SetOnHangImage([this](const std::wstring& path) { OnHangImage(path); });
        clipboard_->SetOnHangEntry(
            [this](int kind, const std::wstring& label, const std::wstring& path,
                   const std::wstring& thumb, const std::wstring& entryId) {
                OnHangEntry(kind, label, path, thumb, entryId);
            });
    }

    initialized_ = true;
    window_->OnConfigChanged();
    /* 正常启动不打扰用户：只有宿主刚放好插件时才会请求那一次「看一眼」。 */
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

    for (const auto& existing : items_) {
        if (util::SamePath(existing.path, full) || util::SamePath(existing.sourcePath, full)) {
            if (window_) {
                window_->Expand(true);
            }
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
    item.path = full;
    item.sourcePath = full;
    item.aspect = static_cast<float>(width) / static_cast<float>(height);
    // 截图飞上绳子现在由 arrival 动画负责，不要再叠一层旧的落位弹簧。
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
    if (const NoteEntry* note = notes_.Find(full)) {
        item.note = note->text;
        item.noteColor = note->color;
    }

    if (window_) {
        window_->DismissPreview();
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

void Plugin::OnHangImage(const std::wstring& path)
{
    if (!initialized_ || path.empty()) return;
    const std::wstring full = util::FullPath(path);
    for (const auto& existing : items_) {
        if (util::SamePath(existing.path, full)) {
            if (window_) window_->Expand(true);
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
    item.path = full;
    item.sourcePath = full;
    item.aspect = aspect;
    item.addedMs = 0;
    item.mtime = util::FileWriteTime(full);
    items_.push_back(std::move(item));
    while (static_cast<int>(items_.size()) > config_.maxItems) {
        RemoveItemData(0);
    }
    if (window_) {
        /* OnItemsChanged 会启动飞行并顺带展开绳子。之前先展开的话，新卡片会
           有一帧出现在终点、再被拽回光标位置，看起来像闪了一下。 */
        window_->OnItemsChanged(true);
    }
    SaveItems();
}

void Plugin::OnHangEntry(int kind, const std::wstring& label, const std::wstring& path,
                         const std::wstring& thumb, const std::wstring& entryId)
{
    if (!initialized_) return;
    if (kind == SNOWAIR_ENTRY_TEXT) {
        /* 文本条目本身就是轻雪的记录：卡片只记 id，正文从轻雪读，不另存一份。 */
        if (entryId.empty()) {
            OnHangText(label);
            return;
        }
        if (FindNoteItem(entryId) >= 0) {
            if (window_) window_->Expand(true);
            return;
        }
        std::wstring text;
        if (!ReadNoteRecord(entryId, text) || text.empty()) return;
        AddNoteItem(text, entryId);
        return;
    }

    Item item;
    item.id = nextId_++;
    item.addedMs = 0;
    item.label = label;
    if (kind == SNOWAIR_ENTRY_IMAGE) {
        if (path.empty()) return;
        item.kind = 0;
        item.path = util::FullPath(path);
        item.sourcePath = item.path;
        item.mtime = util::FileWriteTime(item.path);
        for (const auto& existing : items_) {
            if (existing.kind == 0 && util::SamePath(existing.path, item.path)) {
                if (window_) window_->Expand(true);
                return;
            }
        }
        UINT width = 0, height = 0;
        if (graphics_ && SUCCEEDED(graphics_->images().GetSize(item.path, width, height)) &&
            width > 0 && height > 0) {
            item.aspect = static_cast<float>(width) / static_cast<float>(height);
        } else {
            item.aspect = 1.4f;
        }
    } else if (kind == SNOWAIR_ENTRY_MEDIA) {
        /* 录屏：优先用封面，没封面就退化成纯文字卡片。 */
        item.kind = 2;
        item.aspect = 16.0f / 9.0f;
        if (!path.empty()) {
            item.path = util::FullPath(path);
            item.sourcePath = item.path;
            item.mtime = util::FileWriteTime(item.path);
        }
        const std::wstring cover = thumb.empty() ? std::wstring() : util::FullPath(thumb);
        if (!cover.empty() && util::FileExists(cover)) {
            item.thumbPath = cover;
            UINT width = 0, height = 0;
            if (graphics_ &&
                SUCCEEDED(graphics_->images().GetSize(cover, width, height)) &&
                width > 0 && height > 0) {
                item.aspect = static_cast<float>(width) / static_cast<float>(height);
            }
        }
        for (const auto& existing : items_) {
            if (existing.kind == 2 && !item.path.empty() &&
                util::SamePath(existing.path, item.path)) {
                if (window_) window_->Expand(true);
                return;
            }
        }
    } else {
        /* 文件 / 其他：纯色卡片 + 文件夹图标，文字用条目名。 */
        item.kind = 3;
        item.aspect = 1.4f;
        if (!path.empty()) {
            item.path = util::FullPath(path);
            item.sourcePath = item.path;
            item.mtime = util::FileWriteTime(item.path);
        }
        if (item.label.empty() && !item.path.empty()) {
            item.label = std::filesystem::path(item.path).filename().wstring();
        }
        if (item.label.empty()) return;
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

void Plugin::PinItemToDesktop(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    const Item& item = items_[static_cast<size_t>(index)];
    if (item.kind != 0 || item.path.empty()) {
        return;
    }
    /* 优先调用宿主导出的原生贴图（带落下动效）；旧宿主没有该导出时再退回内置窗口。 */
    using PinImageFileFn = void (*)(const wchar_t*);
    static PinImageFileFn hostPin = []() -> PinImageFileFn {
        const HMODULE host = GetModuleHandleW(nullptr);
        return host ? reinterpret_cast<PinImageFileFn>(
                          GetProcAddress(host, "SnowAirPinImageFile"))
                    : nullptr;
    }();
    if (hostPin) {
        hostPin(item.path.c_str());
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
    if (!pinWindow_->Open(graphics_.get(), item.path, cursor)) {
        RackLog(L"PinWindow: failed to pin %s", item.path.c_str());
    }
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

void Plugin::OnHangText(const std::wstring& text)
{
    if (!initialized_) return;
    const std::wstring body = TrimNoteText(text);
    if (body.empty()) return;
    std::wstring noteId;
    if (!CreateNoteRecord(body, noteId)) {
        RackLog(L"便签没能写进轻雪，已放弃上绳");
        return;
    }
    AddNoteItem(body, noteId);
}

void Plugin::AddNoteItem(const std::wstring& text, const std::wstring& noteId)
{
    Item item;
    item.id = nextId_++;
    item.kind = 1; /* 便签 */
    item.note = text;
    item.noteId = noteId;
    item.noteColor = 2; /* yellow sticky note */
    item.aspect = 1.1f;
    item.addedMs = 0;   /* no drop-in spring: avoids a flicker on landing */
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

void Plugin::DeleteNoteRecord(const std::wstring& noteId)
{
    if (!host_ || !host_->entry_delete_id || noteId.empty()) return;
    host_->entry_delete_id(host_->user, noteId.c_str());
    if (host_->entry_refresh) host_->entry_refresh(host_->user);
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
        ++it;
    }
    if (changed) {
        SaveItems();
    }
}

void Plugin::DestroyNote(int index)
{
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& item = items_[static_cast<size_t>(index)];
    if (item.kind != 1) return;
    DeleteNoteRecord(item.noteId);
}

void Plugin::OnQuickHang(const wchar_t* path)
{
    const bool autoCapture = config_.autoCapture;
    config_.autoCapture = true;
    manualHang_ = true;
    OnCapture(path);
    manualHang_ = false;
    config_.autoCapture = autoCapture;
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
        /* 便签：正文写回轻雪的那条记录，插件只留缓存。 */
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

int Plugin::HistoryCount()
{
    return config_.collectImages && host_ && host_->history_count
               ? host_->history_count(host_->user)
               : 0;
}

int Plugin::ClipboardCount()
{
    return config_.collectClipboard && host_ && host_->clipboard_count
               ? host_->clipboard_count(host_->user)
               : 0;
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

extern "C" SNOWAIR_PLUGIN_EXPORT const SnowAirPlugin* SnowAirPluginEntry(void)
{
    return &kDescriptor;
}

