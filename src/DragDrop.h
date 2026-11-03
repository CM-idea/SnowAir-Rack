#pragma once

#include "Common.h"

namespace dragdrop {

/* 拖拽的额外结果：取消 / 落进同一文件夹 / 桌面图标移动。 */
struct DragOutcome {
    bool returnedToRack = false;
    bool sameFolder = false;
    bool desktop = false;
    bool cancelled = false;
    bool duplicate = false;
    POINT point{};
    std::wstring duplicatePath;
};

/*
 * 对 `path` 发起一次 OLE 拖动。返回最终的 DROPEFFECT；
 * 有目标接收了数据时把 `dropped` 置为 true。
 */
DWORD BeginDrag(const std::wstring& path, HWND rack, RECT returnBand, int cardWidth,
                int cardHeight,
                float frameWidth, bool* dropped, DragOutcome* outcome);

/*
 * 对一段正文发起一次 OLE 拖动（CF_UNICODETEXT / CF_TEXT）：
 * 拖进设计软件、聊天框、编辑器拿到的都是文字本身。
 */
/* ghost 由调用方渲染（和卡片本体一致），可为空；函数接管它的释放。 */
DWORD BeginTextDrag(const std::wstring& text, HWND rack, RECT returnBand, HBITMAP ghost,
                    SIZE ghostSize,
                    int cardWidth, int cardHeight, float frameWidth, bool* dropped,
                    DragOutcome* outcome, const std::wstring& previewPath = std::wstring(),
                    bool pasteOnly = false);

/* 拖拽期间统一设置「箭头 + 复制框」光标。 */
void SetDragCursor();
/* 鼠标下面是否是 UIA/MSAA 能识别的可输入元素。 */
bool EditableAtPoint(POINT point);

/* 把桌面上某个文件/快捷方式的图标移动到屏幕坐标处（自动排列开启时由系统忽略）。 */
bool RepositionDesktopIcon(const std::wstring& path, POINT screenPoint);

} /* namespace dragdrop */
