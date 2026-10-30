#pragma once

#include "Common.h"

namespace clipboard {

/* Put PNG / CF_DIB / CF_HDROP (and image/png) on the clipboard. */
bool CopyImage(const std::wstring& path);
bool CopyText(const std::wstring& text);

} /* namespace clipboard */
