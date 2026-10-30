#include "NoteStore.h"
#include "MiniJson.h"
#include "Util.h"

std::wstring NoteStore::FilePath() const
{
    return util::JoinPath(util::GetDataDir(), L"rack.json");
}

bool NoteStore::Load()
{
    entries_.clear();
    dirty_ = false;

    std::vector<BYTE> bytes;
    if (!util::ReadFileBytes(FilePath(), bytes) || bytes.empty()) {
        return true;
    }

    std::string text(bytes.begin(), bytes.end());
    /* Accept files saved by editors that add a UTF-8 BOM. */
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    json::Value root;
    if (!json::Parse(text, root)) {
        RackLog(L"NoteStore: rack.json is not valid JSON, ignoring it");
        return false;
    }

    const json::Value* notes = root.Get("notes");
    if (!notes || notes->type != json::Type::Array) {
        return true;
    }

    for (const auto& value : notes->array) {
        if (value.type != json::Type::Object) {
            continue;
        }
        const json::Value* path = value.Get("path");
        const json::Value* note = value.Get("text");
        if (!path || path->type != json::Type::String) {
            continue;
        }
        NoteEntry entry;
        if (note && note->type == json::Type::String) {
            entry.text = util::Utf8ToWide(note->text);
        }
        const json::Value* color = value.Get("color");
        if (color && color->type == json::Type::Number) {
            entry.color = rack::ClampInt(static_cast<int>(color->number), 0, 2);
        }
        std::wstring widePath = util::Utf8ToWide(path->text);
        if (widePath.empty()) {
            continue;
        }
        entries_.emplace_back(std::move(widePath), std::move(entry));
    }
    return true;
}

bool NoteStore::Save()
{
    std::string output;
    output += "{\n  \"version\": 1,\n  \"notes\": [\n";
    bool first = true;
    for (const auto& entry : entries_) {
        if (entry.second.text.empty()) {
            continue;
        }
        if (!first) {
            output += ",\n";
        }
        first = false;
        output += "    { \"path\": ";
        output += json::Quote(util::WideToUtf8(entry.first));
        output += ", \"text\": ";
        output += json::Quote(util::WideToUtf8(entry.second.text));
        output += ", \"color\": ";
        output += std::to_string(entry.second.color);
        output += " }";
    }
    output += "\n  ]\n}\n";

    std::wstring target = FilePath();
    std::wstring temp = target + L".tmp";
    if (!util::WriteFileBytes(temp, output.data(), output.size())) {
        RackLog(L"NoteStore: failed to write %s", temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        RackLog(L"NoteStore: failed to replace %s (%lu)", target.c_str(), GetLastError());
        util::DeleteFileIfExists(temp);
        return false;
    }
    dirty_ = false;
    return true;
}

const NoteEntry* NoteStore::Find(const std::wstring& path) const
{
    for (const auto& entry : entries_) {
        if (util::SamePath(entry.first, path)) {
            return &entry.second;
        }
    }
    return nullptr;
}

void NoteStore::Set(const std::wstring& path, const NoteEntry& note)
{
    if (path.empty()) {
        return;
    }
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (util::SamePath(it->first, path)) {
            if (note.text.empty()) {
                entries_.erase(it);
            } else {
                it->second = note;
            }
            dirty_ = true;
            Save();
            return;
        }
    }
    if (note.text.empty()) {
        return;
    }
    entries_.emplace_back(path, note);
    dirty_ = true;
    Save();
}

void NoteStore::Remove(const std::wstring& path)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (util::SamePath(it->first, path)) {
            entries_.erase(it);
            dirty_ = true;
            Save();
            return;
        }
    }
}

void NoteStore::Clear()
{
    if (entries_.empty()) {
        return;
    }
    entries_.clear();
    dirty_ = true;
    Save();
}
