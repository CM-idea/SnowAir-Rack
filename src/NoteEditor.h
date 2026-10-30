#pragma once

#include "Common.h"

class NoteEditor {
public:
    /* Returns true when the user pressed Save. `text` receives the new note. */
    static bool Edit(HWND owner, HINSTANCE instance, const std::wstring& initial, bool dark,
                     std::wstring& text);
};
