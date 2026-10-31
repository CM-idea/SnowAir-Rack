#pragma once

#include "Common.h"
#include "ClipboardBubble.h"
#include "Config.h"
#include "Graphics.h"
#include "Item.h"
#include "ItemStore.h"
#include "NoteStore.h"
#include "Theme.h"

class RackWindow;
class PinWindow;

class Plugin {
public:
    static Plugin& Instance();

    bool Init(const SnowAirHost* host);
    void Shutdown();
    bool initialized() const { return initialized_; }

    void OnCapture(const wchar_t* path);
    void OnQuickHang(const wchar_t* path);
    void OnHangText(const std::wstring& text);
    void OnHangImage(const std::wstring& path);
    /* 统一上绳：kind = SNOWAIR_ENTRY_*；entryId 只在文本条目上有值（轻雪的记录 id）。 */
    void OnHangEntry(int kind, const std::wstring& label, const std::wstring& path,
                     const std::wstring& thumb, const std::wstring& entryId);
    /* 点击图片卡片的夹子：把图片贴到桌面。 */
    void PinItemToDesktop(int index);
    void PinImageFile(const std::wstring& path);
    /* 销毁便签：删掉轻雪里对应的文本记录（卡片由调用方移除）。 */
    void DestroyNote(int index);
    void OnSettingChanged(const wchar_t* key);
    int SettingsCount() const;
    bool SettingsGet(int index, SnowAirSettingDesc* out) const;

    std::vector<Item>& items() { return items_; }
    Theme& theme() { return theme_; }
    RackConfig& config() { return config_; }
    NoteStore& notes() { return notes_; }
    Graphics& graphics() { return *graphics_; }
    const SnowAirHost* host() const { return host_; }
    bool dragEnabled() const { return oleInitialized_; }

    void RemoveItemAt(int index);
    void SetItemNote(int index, const std::wstring& text, int color);
    void TrimItems();
    int IndexById(int id) const;
    Item* ItemById(int id);
    void RefreshTheme();
    void SyncHostTheme();
    bool HostThemeIsDark() const;
    void SaveNotes();
    void SaveItems();
    int HistoryCount();
    int ClipboardCount();
    bool HoverPreviewEnabled();
    bool PreviewPinned();
    bool MenuOpen();
    bool ClipboardOpen();
    /* 屏幕点是否落在剪切板面板上（面板打开时才有效）。 */
    bool ClipboardContains(POINT screen);
    void CloseMenu();
    void OpenHistory();
    void OpenClipboard(int screenX, int screenY);
    void LogMessage(const wchar_t* text);

private:
    Plugin() = default;
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    void RemoveItemData(int index);

    /*
     * 便签数据在轻雪：这里只是读写它剪切板里的文本记录。
     * 插件本地只保存 noteId，正文永远从轻雪取。
     */
    void SyncNotes();
    void AddNoteItem(const std::wstring& text, const std::wstring& noteId);
    bool CreateNoteRecord(const std::wstring& text, std::wstring& noteId);
    bool ReadNoteRecord(const std::wstring& noteId, std::wstring& text);
    bool UpdateNoteRecord(const std::wstring& noteId, const std::wstring& text);
    void DeleteNoteRecord(const std::wstring& noteId);
    int FindNoteItem(const std::wstring& noteId) const;

    std::unique_ptr<RackWindow> window_;
    std::unique_ptr<ClipboardBubble> clipboard_;
    std::unique_ptr<PinWindow> pinWindow_;
    uint64_t clipboardClosedTick_ = 0;
    int lastHostDark_ = -1;
    std::unique_ptr<Graphics> graphics_;
    NoteStore notes_;
    ItemStore itemStore_;
    RackConfig config_;
    Theme theme_;
    std::vector<Item> items_;
    const SnowAirHost* host_ = nullptr;
    bool initialized_ = false;
    bool comInitialized_ = false;
    bool oleInitialized_ = false;
    int nextId_ = 1;
    bool manualHang_ = false;
};
