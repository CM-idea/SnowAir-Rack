#pragma once

#include "Common.h"

struct NoteEntry {
    std::wstring text;
    int color = 0;

    bool empty() const { return text.empty(); }
};

/*
 * Notes are the only thing we persist. Screenshots themselves remain SnowAir's
 * files; the plugin never owns them.
 *
 * File: <data>\rack.json  (UTF-8)
 * {
 *   "version": 1,
 *   "notes": [ { "path": "C:\\...\\shot.png", "text": "...", "color": 0 } ]
 * }
 */
class NoteStore {
public:
    bool Load();
    bool Save();

    const NoteEntry* Find(const std::wstring& path) const;
    void Set(const std::wstring& path, const NoteEntry& note);
    void Remove(const std::wstring& path);
    void Clear();

    size_t size() const { return entries_.size(); }

private:
    std::wstring FilePath() const;

    std::vector<std::pair<std::wstring, NoteEntry>> entries_;
    bool dirty_ = false;
};
