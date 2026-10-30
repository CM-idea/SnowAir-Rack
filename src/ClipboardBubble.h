#pragma once

#include "Common.h"
#include "Graphics.h"
#include "Image.h"
#include "LayeredWindow.h"
#include "Theme.h"

#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

/* The envelope panel: SnowAir's own history (screenshots / recordings / other
   files) and the clipboard history in one card list, filtered by category or
   custom tag. Data and tags live in SnowAir; the plugin only draws. */
class ClipboardBubble : public LayeredWindow {
public:
    ClipboardBubble();
    ~ClipboardBubble() override;
    bool isOpen() const { return visible(); }

    bool Init(Graphics* graphics, const SnowAirHost* host);
    void Open(int screenX, int screenY, float scale, const Theme& theme);
    void Close();
    void ApplyTheme(const Theme& theme);
    bool showing() const { return visible() && !entries_.empty(); }
    void SetOnClosed(std::function<void()> callback) { onClosed_ = std::move(callback); }
    void SetOnHangText(std::function<void(const std::wstring&)> callback) { onHangText_ = std::move(callback); }
    void SetOnHangImage(std::function<void(const std::wstring&)> callback) { onHangImage_ = std::move(callback); }
    void SetOnHangEntry(std::function<void(int, const std::wstring&, const std::wstring&,
                                           const std::wstring&)> callback) {
        onHangEntry_ = std::move(callback);
    }

protected:
    void Paint(ID2D1RenderTarget* target) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    struct Entry {
        std::wstring label;
        std::wstring path;
        std::wstring thumb;
        std::wstring time;
        std::vector<std::wstring> tags;
        int kind = 0;     /* SNOWAIR_ENTRY_* */
        int source = 0;   /* SNOWAIR_SOURCE_* */
        int width = 0;
        int height = 0;
        int duration = 0;
        long long createdAt = 0;
        float cardHeight = 0.0f;
    };

    struct Brushes {
        ComPtr<ID2D1SolidColorBrush> fg;
        ComPtr<ID2D1SolidColorBrush> muted;
        ComPtr<ID2D1SolidColorBrush> secondaryFg;
        ComPtr<ID2D1SolidColorBrush> border;
        ComPtr<ID2D1SolidColorBrush> accent;
        ComPtr<ID2D1SolidColorBrush> primary;
        ComPtr<ID2D1SolidColorBrush> primaryFg;
        ComPtr<ID2D1SolidColorBrush> secondary;
        ComPtr<ID2D1SolidColorBrush> card;
        ComPtr<ID2D1SolidColorBrush> cardBg;
        ComPtr<ID2D1SolidColorBrush> panel;
        ComPtr<ID2D1SolidColorBrush> canvas;
        ComPtr<ID2D1SolidColorBrush> dim;
        ComPtr<ID2D1SolidColorBrush> contentBorder;
        ComPtr<ID2D1SolidColorBrush> chipBg;
        ComPtr<ID2D1SolidColorBrush> searchBg;
        ComPtr<ID2D1SolidColorBrush> iconBg;
        ComPtr<ID2D1SolidColorBrush> iconHover;
        ComPtr<ID2D1SolidColorBrush> iconSelected;
        ComPtr<ID2D1SolidColorBrush> hoverBg;
        ComPtr<ID2D1SolidColorBrush> tagBg;
        ComPtr<ID2D1SolidColorBrush> tagHover;
        ComPtr<ID2D1SolidColorBrush> plusBg;
        ComPtr<ID2D1SolidColorBrush> plusHover;
        ComPtr<ID2D1SolidColorBrush> selectedBg;
        ComPtr<ID2D1SolidColorBrush> success;
        ComPtr<ID2D1SolidColorBrush> danger;
        ComPtr<ID2D1SolidColorBrush> dangerHover;
        ComPtr<ID2D1SolidColorBrush> primaryHover;
        ComPtr<ID2D1SolidColorBrush> modalCard;
        ComPtr<ID2D1SolidColorBrush> modalBorder;
        ComPtr<ID2D1SolidColorBrush> modalShadow;
        ComPtr<ID2D1SolidColorBrush> insertion;
        ComPtr<ID2D1SolidColorBrush> disabledBg;
        ComPtr<ID2D1SolidColorBrush> onDanger;
    };

    /* ---- data ---- */
    void LoadEntries();
    void LoadCategories();
    void SaveCategories();
    void RefreshCategories();
    void RebuildShown();
    void MeasureCards();
    void Reload(bool resetScroll);
    void ResizeAndMove();

    /* ---- geometry (already scaled) ---- */
    float SearchTop() const;
    float ChipsTop() const;
    float ListTop() const;
    float ListViewportHeight() const;
    float CardWidth() const;
    float TextContentWidth() const;
    void LayoutChips();
    float TextWidth(const std::wstring& text, IDWriteTextFormat* format) const;
    float RowTop(int row) const;

    /* ---- hit tests ---- */
    int HitRow(float x, float y) const;
    int HitChip(float x, float y) const;
    int HitPlus(float x, float y) const;
    int HitAction(float x, float y, int row) const;
    int HitTagChip(float x, float y, int row) const;
    /* 1 = 取消, 2 = 确定, 3 = 删除, 4 = 添加标签; 0 = 未命中任何弹窗按钮. */
    int HitModalButton(float x, float y) const;
    std::wstring TypeTagFor(const Entry& entry) const;

    /* ---- painting ---- */
    void PaintHeader(ID2D1RenderTarget* target, const Brushes& b);
    void PaintCards(ID2D1RenderTarget* target, const Brushes& b);
    void PaintModals(ID2D1RenderTarget* target, const Brushes& b);
    void PaintEmptyState(ID2D1RenderTarget* target, const Brushes& b);

    /* ---- interaction ---- */
    /* 分类与标签弹窗：完全套用 SnowAir「自定义标签」弹窗的布局与交互。
       标签以 chip 形式内联在输入框里，空格成词、退格删 chip。 */
    struct TagChipRect {
        D2D1_RECT_F rect{};
        int index = -1;       /* catChips：categories_ 下标 */
        std::wstring text;
    };
    struct TagEditorLayout {
        D2D1_RECT_F card{};
        D2D1_RECT_F title{};
        D2D1_RECT_F labelCats{};
        D2D1_RECT_F labelTags{};
        D2D1_RECT_F field{};
        D2D1_RECT_F edit{};   /* 输入框在 field 里的内联位置 */
        D2D1_RECT_F cancel{};
        D2D1_RECT_F save{};
        std::vector<TagChipRect> catChips;
        std::vector<TagChipRect> tagChips;
    };
    TagEditorLayout BuildTagEditorLayout() const;
    void CommitTagText();
    void OpenTagEditor(int entryIndex);
    void CloseTagEditor(bool save);
    void OpenCategoryEditor();
    void CloseCategoryEditor(bool save);
    /* 新建便签：搜索栏的 + 打开，输入文字后点完成直接上绳。 */
    struct NoteEditorLayout {
        D2D1_RECT_F card{};
        D2D1_RECT_F title{};
        D2D1_RECT_F field{};
        D2D1_RECT_F cancel{};
        D2D1_RECT_F save{};
    };
    NoteEditorLayout BuildNoteEditorLayout() const;
    void OpenNoteEditor();
    void CloseNoteEditor(bool save);
    void LayoutEditors();
    /* 弹窗/分类设置打开时把搜索框的 EDIT 窗口藏起来，否则它会浮在遮罩上面。 */
    void UpdateSearchEditVisibility();
    void ClearHover();
    /* 悬停提示：根据鼠标位置返回当前控件的提示文字，空串表示不显示。 */
    std::wstring TooltipAt(float x, float y, D2D1_RECT_F* anchor) const;
    void ScrollToShowChip(int chip);
    void EnsureRowVisible(int row);
    std::vector<int> ActionsFor(const Entry& entry) const;

    /* ---- search edit ---- */
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    COLORREF editBrushColor_ = 0xFFFFFFFF;
    WNDPROC editOldProc_ = nullptr;
    bool syncingEdit_ = false;
    void CreateSearchEdit();
    void DestroySearchEdit();
    static LRESULT CALLBACK SearchEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                           LPARAM lParam);

    /* ---- modal input edit ---- */
    HWND modalEdit_ = nullptr;
    WNDPROC modalEditOldProc_ = nullptr;
    void CreateModalEdit();
    void DestroyModalEdit();
    static LRESULT CALLBACK ModalEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                          LPARAM lParam);

    /* ---- note editor (multi-line) ---- */
    HWND noteEdit_ = nullptr;
    WNDPROC noteEditOldProc_ = nullptr;
    bool noteEditorOpen_ = false;
    void CreateNoteEdit();
    void DestroyNoteEdit();
    static LRESULT CALLBACK NoteEditProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam);

    /* ---- state ---- */
    std::function<void()> onClosed_;
    std::function<void(const std::wstring&)> onHangText_;
    std::function<void(const std::wstring&)> onHangImage_;
    std::function<void(int, const std::wstring&, const std::wstring&, const std::wstring&)>
        onHangEntry_;
    const SnowAirHost* host_ = nullptr;
    Theme theme_;
    float scale_ = 1.0f;
    float width_ = 0.0f;
    float height_ = 0.0f;
    int x_ = 0;
    int y_ = 0;
    float anchorX_ = 0.0f;

    std::vector<Entry> entries_;
    std::vector<int> shown_;
    std::vector<float> rowTops_;
    std::vector<float> rowHeights_;
    float scrollY_ = 0.0f;
    float rowsHeight_ = 0.0f;

    int filter_ = 0; /* 0..5 fixed, 6.. custom tag */
    std::vector<D2D1_RECT_F> chipRects_;
    float chipScrollX_ = 0.0f;
    float chipContentWidth_ = 0.0f;
    /* 分类条左右拖拽：按下先记录，移动超过阈值才算拖拽，松手未拖拽才切换分类。 */
    int chipPress_ = -1;
    bool chipDragging_ = false;
    float chipDragStartX_ = 0.0f;
    float chipDragScrollStart_ = 0.0f;

    std::wstring query_;
    bool searchFocused_ = false;
    bool caretVisible_ = true;
    float caretClientX_ = 0.0f;
    float caretClientY_ = 0.0f;
    void PositionIme();
    void ActivateForInput();
    bool positioningIme_ = false;

    int hovered_ = -1;
    int hoveredAction_ = -1;
    int hoveredChip_ = -1;
    int hoveredTag_ = -1;
    bool hoveredPlus_ = false;
    bool hoveredGear_ = false;
    int hoveredModalButton_ = 0;
    std::wstring tooltipText_;
    float tooltipX_ = 0.0f;
    float tooltipY_ = 0.0f;
    D2D1_RECT_F tooltipAnchor_{};
    bool trackingLeave_ = false;
    bool searchEditVisible_ = false;
    bool gearPressed_ = false;
    bool captured_ = false;

    /* tag editor */
    bool tagEditorOpen_ = false;
    int tagEditorEntry_ = -1;
    std::vector<std::wstring> tagSelection_;
    std::vector<int> editCats_;

    /* category settings */
    struct Category {
        std::wstring name;
        int builtin = -1; /* 0..5 fixed chips, -1 = custom */
        bool hidden = false;
    };
    std::vector<Category> categories_;
    std::vector<int> visibleCats_;
    std::map<std::wstring, std::vector<std::wstring>> assignments_;
    std::vector<std::wstring> hostTags_;
    std::wstring EntryKey(const Entry& entry) const;
    void RefreshVisibleCategories();
    bool CategoryMatches(const Entry& entry, const Category& category) const;

    bool settingsOpen_ = false;
    int settingsDrag_ = -1;
    int settingsDrop_ = -1;
    bool settingsDropHidden_ = false;
    bool settingsDropAfter_ = false;
    int renamingCategory_ = -1;
    int settingsHover_ = -1;
    bool hoveredSettingsAdd_ = false;
    D2D1_RECT_F SettingsAddRect() const;
    ULONGLONG lastCategoryClick_ = 0;
    int lastCategoryRow_ = -1;
    float dragX_ = 0.0f;
    float dragY_ = 0.0f;
    void OpenSettings();
    void CloseSettings();
    void OpenRenameCategory(int index);
    void ToggleCategoryHidden(int index);
    void DeleteCategory(int index);
    D2D1_RECT_F SettingsCardRect() const;
    D2D1_RECT_F SettingsRowRect(int row) const;
    int SettingsRowAt(float x, float y) const;
    void PaintSettings(ID2D1RenderTarget* target, const Brushes& b);
    void PaintCategoryManager(ID2D1RenderTarget* target, const Brushes& b);
    D2D1_RECT_F ManagerCardRect(bool hidden) const;
    D2D1_RECT_F ManagerRowRect(int index) const;
    int ManagerRowAt(float x, float y) const;
    int ManagerDropAt(float x, float y, bool* after) const;
    bool CategoryHasItems(int index) const;
    int deleteConfirm_ = -1;
    bool HitGear(float x, float y) const;

    /* category editor */
    bool categoryEditorOpen_ = false;

    std::unordered_map<std::wstring, ImageBitmap> thumbs_;
    float titleLineHeight_ = 18.0f;

    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> chipFormat_;
    ComPtr<IDWriteTextFormat> metaFormat_;
    ComPtr<IDWriteTextFormat> tagFormat_;
    ComPtr<IDWriteTextFormat> buttonFormat_;
    ComPtr<IDWriteTextFormat> hintFormat_;
    ComPtr<IDWriteTextFormat> bigHintFormat_;
    float formatScale_ = 0.0f;
    void EnsureFormats(float scale);

    static HHOOK mouseHook_;
    static ClipboardBubble* hookOwner_;
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam);

    void LoadIcons();
    void BuildGlyph(uint32_t codepoint, ComPtr<ID2D1PathGeometry>& out);
    void DrawGlyph(ID2D1RenderTarget* target, ID2D1PathGeometry* glyph,
                   D2D1_POINT_2F center, float size, ID2D1Brush* brush);

    ComPtr<IDWriteFontFace> iconFace_;    ComPtr<ID2D1PathGeometry> iconCopy_;
    ComPtr<ID2D1PathGeometry> iconDelete_;
    ComPtr<ID2D1PathGeometry> iconPeg_;
    ComPtr<ID2D1PathGeometry> iconText_;
    ComPtr<ID2D1PathGeometry> iconImage_;
    ComPtr<ID2D1PathGeometry> iconFiles_;
    ComPtr<ID2D1PathGeometry> iconGear_;
    ComPtr<ID2D1PathGeometry> iconDrag_;
};
