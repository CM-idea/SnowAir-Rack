#pragma once

#include "Common.h"

namespace clipboard {

/* 把 PNG / CF_DIB / CF_HDROP（以及 image/png 格式）放进剪贴板。 */
bool CopyImage(const std::wstring& path);
bool CopyText(const std::wstring& text);
/* 把图片文件解成 CF_DIB 数据块（拖放时给设计软件用），失败返回 nullptr。 */
HGLOBAL MakeDibFromFile(const std::wstring& path);

/* 只把文件放进剪贴板（CF_HDROP），不读取文件内容，适合视频/大文件。 */
bool CopyFilePath(const std::wstring& path);
/* 多文件 CF_HDROP。 */
bool CopyFilePaths(const std::vector<std::wstring>& paths);

} /* namespace clipboard */
