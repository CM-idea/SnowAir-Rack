#pragma once

#include "Common.h"

namespace dragdrop {

/*
 * Starts an OLE drag of `path`. Returns the resulting DROPEFFECT and sets
 * `dropped` when something accepted the data.
 */
DWORD BeginDrag(const std::wstring& path, int cardWidth, int cardHeight,
                float frameWidth, bool* dropped);

} /* namespace dragdrop */
