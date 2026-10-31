#pragma once

#include "Common.h"

struct NoteEntry {
    std::wstring text;
    int color = 0;

    bool empty() const { return text.empty(); }
};

/*
 * 便签是这里唯一持久化下来的内容；截图本身是轻雪的文件，插件从不持有。
 *
 * 文件：<data>\rack.json（UTF-8）
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
