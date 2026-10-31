#pragma once

#include "Common.h"

namespace clipboard {

/* 把 PNG / CF_DIB / CF_HDROP（以及 image/png 格式）放进剪贴板。 */
bool CopyImage(const std::wstring& path);
bool CopyText(const std::wstring& text);
/* 只把文件放进剪贴板（CF_HDROP），不读取文件内容，适合视频/大文件。 */
bool CopyFilePath(const std::wstring& path);

} /* namespace clipboard */
