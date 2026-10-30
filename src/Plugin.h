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
    void DestroyNoteText(const std::wstring& text);
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

    std::unique_ptr<RackWindow> window_;
    std::unique_ptr<ClipboardBubble> clipboard_;
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
