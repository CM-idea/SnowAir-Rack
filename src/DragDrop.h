#pragma once

#include "Common.h"

namespace dragdrop {

/* 拖拽的额外结果：反悔 / 落进同一文件夹 / 桌面挪图标。 */
struct DragOutcome {
    bool returnedToRack = false;
    bool sameFolder = false;
    bool desktop = false;
    POINT point{};
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
                    DragOutcome* outcome);

/* 把桌面上某个文件/快捷方式的图标挪到屏幕坐标处（自动排列开启时由系统忽略）。 */
bool RepositionDesktopIcon(const std::wstring& path, POINT screenPoint);

} /* namespace dragdrop */
