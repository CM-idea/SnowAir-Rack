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
    /* 快速贴屏接管：文本 / 文件或文件夹都直接上绳；图片仍走 OnQuickHang。 */
    bool QuickHang(int kind, const std::wstring& text,
                   const std::vector<std::wstring>& paths);
    void OnHangText(const std::wstring& text);
    void OnHangImage(const std::wstring& path, const std::wstring& svgId = std::wstring());
    /* SVG 卡片：按轻雪记录 id 取回代码，复制/拖拽用；取不到返回空。 */
    std::wstring SvgText(int index);
    /* 文字类卡片：把内容放进剪贴板，但不让轻雪历史把它提到最前/新增一条。 */
    bool CopyCardText(int index);
    /* 统一上绳：kind = SNOWAIR_ENTRY_*；entryId 只在文本条目上有值（轻雪的记录 id）。 */
    void OnHangEntry(int kind, const std::wstring& label, const std::wstring& path,
                     const std::wstring& thumb, const std::wstring& entryId, long long createdAt,
                     const std::vector<std::wstring>& paths, bool isNote);
    /* 仅供 HostSim 预览用的入口：直接挂一张文字/文件卡。 */
    void DebugHangText(const std::wstring& text, bool note);
    void DebugHangFile(const std::wstring& path);
    /* 点击图片卡片的夹子：把图片贴到桌面。 */
    void PinImageFile(const std::wstring& path);
    void PinTextToDesktop(const std::wstring& text);
    void PinFilesToDesktop(const std::vector<std::wstring>& paths);
    /* 指定卡片正下方锚点（屏幕坐标：centerX / 卡片底边 y）。 */
    void PinImageFileAt(const std::wstring& path, int centerX, int topY);
    void PinTextToDesktopAt(const std::wstring& text, int centerX, int topY);
    void PinFilesToDesktopAt(const std::vector<std::wstring>& paths, int centerX, int topY);
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
    /* 轻雪「停用历史记录」开关。宿主没有专门的查询接口，直接读它的配置键。 */
    bool HistoryDisabled() const;
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
    /* 清掉 RackQuick 下已经不在任何卡片里的旧缓存图（永久删除）。 */
    void PruneRackQuickCache();

    /*
     * 便签数据在轻雪：这里只是读写它剪切板里的文本记录。
     * 插件本地只保存 noteId，正文永远从轻雪取；从绳上取下卡片不删记录，
     * 轻雪剪切板历史里仍是一条带「便签」标签的普通文本。
     */
    void SyncNotes();
    void AddNoteItem(const std::wstring& text, const std::wstring& noteId,
                     card::Type type = card::Type::Note, long long createdAt = 0);
    bool CreateNoteRecord(const std::wstring& text, std::wstring& noteId);
    bool ReadNoteRecord(const std::wstring& noteId, std::wstring& text);
    bool UpdateNoteRecord(const std::wstring& noteId, const std::wstring& text);
    int FindHostEntryById(const std::wstring& id) const;
    int FindNoteItem(const std::wstring& noteId) const;
    /* 同一套「重复上绳」判定：便签比正文，其它比路径，没有路径时比标题。 */
    int FindDuplicateItem(const Item& item) const;
    /* 重复时的反馈：展开绳子 + 使该卡片轻微晃动提示。 */
    void NudgeExistingItem(int index);

    std::unique_ptr<RackWindow> window_;
    std::unique_ptr<ClipboardBubble> clipboard_;
    std::unique_ptr<PinWindow> pinWindow_;
    uint64_t clipboardClosedTick_ = 0;
    /* 上一次从篮子打开的历史记录窗口（宿主的设置窗口）；篮子再点一次时关掉它。 */
    HWND historyWindow_ = nullptr;
    std::wstring historyTitle_;
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
