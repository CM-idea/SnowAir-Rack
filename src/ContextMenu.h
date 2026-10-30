#pragma once

#include "Common.h"
#include "LayeredWindow.h"
#include "Theme.h"

#include <functional>

struct MenuItem {
    enum class Kind { Action, Separator, Color };

    Kind kind = Kind::Action;
    std::wstring label;
    int id = 0;
    bool enabled = true;
    bool checked = false;
    D2D1_COLOR_F swatch{};
};

/*
 * Self-drawn context menu.
 *
 * SnowAir's rack window is WS_EX_NOACTIVATE; a native TrackPopupMenu on such a
 * window can appear but does not reliably receive clicks. Drawing our own menu
 * keeps activation out of the picture and matches the rack's visual language.
 */
class ContextMenu : public LayeredWindow {
public:
    ContextMenu();
    ~ContextMenu() override = default;

    bool Init(Graphics* graphics);
    void Show(const std::vector<MenuItem>& items, POINT screen, const RECT& workArea,
              float scale, const Theme& theme, std::function<void(int)> onSelect,
              std::function<void()> onClose);
    void Close();
    bool showing() const { return visible() && !items_.empty(); }

protected:
    void Paint(ID2D1RenderTarget* target) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    void EnsureFormat(float scale);
    int HitRow(int x, int y) const;
    D2D1_RECT_F RowRect(int index) const;
    int MenuHeight() const;

    std::vector<MenuItem> items_;
    std::function<void(int)> onSelect_;
    std::function<void()> onClose_;
    ComPtr<IDWriteTextFormat> textFormat_;
    Theme theme_;
    float formatScale_ = 0.0f;
    float scale_ = 1.0f;
    float width_ = 0.0f;
    float height_ = 0.0f;
    int hovered_ = -1;
    int pressed_ = -1;
    bool closing_ = false;
};
