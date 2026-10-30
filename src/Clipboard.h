#pragma once

#include "Common.h"

namespace clipboard {

/* Put PNG / CF_DIB / CF_HDROP (and image/png) on the clipboard. */
bool CopyImage(const std::wstring& path);
bool CopyText(const std::wstring& text);
/* 只把文件放进剪贴板（CF_HDROP），不读取文件内容，适合视频/大文件。 */
bool CopyFilePath(const std::wstring& path);

} /* namespace clipboard */
