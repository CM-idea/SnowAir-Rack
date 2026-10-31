#pragma once

#include "Common.h"

namespace dragdrop {

/*
 * 对 `path` 发起一次 OLE 拖动。返回最终的 DROPEFFECT；
 * 有目标接收了数据时把 `dropped` 置为 true。
 */
DWORD BeginDrag(const std::wstring& path, int cardWidth, int cardHeight,
                float frameWidth, bool* dropped);

} /* namespace dragdrop */
