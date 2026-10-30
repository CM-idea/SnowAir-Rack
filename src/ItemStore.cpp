#include "ItemStore.h"

#include "MiniJson.h"
#include "Util.h"

std::wstring ItemStore::FilePath() const
{
    return util::JoinPath(util::GetDataDir(), L"rack_items.json");
}

bool ItemStore::Load(std::vector<Item>& items) const
{
    items.clear();
    std::vector<BYTE> bytes;
    if (!util::ReadFileBytes(FilePath(), bytes) || bytes.empty()) {
        return true;
    }
    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    json::Value root;
    if (!json::Parse(text, root)) {
        RackLog(L"ItemStore: rack_items.json is not valid JSON, ignoring it");
        return false;
    }
    const json::Value* list = root.Get("items");
    if (!list || list->type != json::Type::Array) {
        return true;
    }

    for (const auto& value : list->array) {
        if (value.type != json::Type::Object) {
            continue;
        }
        const json::Value* path = value.Get("path");
        const json::Value* noteValue = value.Get("note");
        const bool hasNote = noteValue && noteValue->type == json::Type::String &&
                             !noteValue->text.empty();
        int kind = 0;
        bool hasKind = false;
        if (const json::Value* k = value.Get("kind"); k && k->type == json::Type::Number) {
            kind = rack::ClampInt(static_cast<int>(k->number), 0, 3);
            hasKind = true;
        }
        std::wstring label;
        if (const json::Value* l = value.Get("label"); l && l->type == json::Type::String) {
            label = util::Utf8ToWide(l->text);
        }
        std::wstring thumbPath;
        if (const json::Value* tp = value.Get("thumbPath");
            tp && tp->type == json::Type::String) {
            thumbPath = util::Utf8ToWide(tp->text);
        }
        const bool hasPath = path && path->type == json::Type::String && !path->text.empty();
        Item item;
        item.kind = hasKind ? kind : (hasPath ? 0 : 1);
        item.label = label;
        if (!thumbPath.empty()) item.thumbPath = util::FullPath(thumbPath);
        if (const json::Value* id = value.Get("id"); id && id->type == json::Type::Number) {
            item.id = static_cast<int>(id->number);
        }
        if (const json::Value* aspect = value.Get("aspect");
            aspect && aspect->type == json::Type::Number && aspect->number > 0.0) {
            item.aspect = static_cast<float>(aspect->number);
        }
        if (const json::Value* offset = value.Get("offsetX");
            offset && offset->type == json::Type::Number) {
            item.rackOffsetX = static_cast<float>(offset->number);
        }
        if (const json::Value* locked = value.Get("locked");
            locked && locked->type == json::Type::Bool) {
            item.locked = locked->boolean;
        }
        if (const json::Value* added = value.Get("addedMs");
            added && added->type == json::Type::Number) {
            item.addedMs = static_cast<uint64_t>(added->number);
        }
        if (const json::Value* color = value.Get("noteColor");
            color && color->type == json::Type::Number) {
            item.noteColor = rack::ClampInt(static_cast<int>(color->number), 0, 2);
        }
        if (hasNote) item.note = util::Utf8ToWide(noteValue->text);

        if (!hasPath) {
            /* 无路径：便签 / 纯文字录屏 / 纯文字文件 */
            if (!hasNote && item.label.empty()) continue;
            item.aspect = 1.1f;
            items.push_back(std::move(item));
            continue;
        }
        const std::wstring wide = util::Utf8ToWide(path->text);
        item.path = util::FullPath(wide);
        item.sourcePath = item.path;
        if (const json::Value* source = value.Get("sourcePath");
            source && source->type == json::Type::String) {
            const std::wstring sourcePath = util::Utf8ToWide(source->text);
            if (!sourcePath.empty()) item.sourcePath = sourcePath;
        }
        if (!util::FileExists(item.path) && !hasNote && item.label.empty()) {
            continue;
        }
        if (item.path.empty()) item.aspect = 1.1f;
        item.mtime = util::FileWriteTime(item.path);
        items.push_back(std::move(item));
    }
    return true;
}

bool ItemStore::Save(const std::vector<Item>& items) const
{
    std::string output;
    output += "{\n  \"version\": 1,\n  \"items\": [\n";
    bool first = true;
    for (const auto& item : items) {
        if (item.path.empty() && item.note.empty() && item.label.empty()) {
            continue;
        }
        if (!first) output += ",\n";
        first = false;
        output += "    { \"kind\": " + std::to_string(item.kind);
        output += ", \"path\": " + json::Quote(util::WideToUtf8(item.path));
        output += ", \"sourcePath\": " + json::Quote(util::WideToUtf8(item.sourcePath));
        output += ", \"thumbPath\": " + json::Quote(util::WideToUtf8(item.thumbPath));
        output += ", \"label\": " + json::Quote(util::WideToUtf8(item.label));
        output += ", \"id\": " + std::to_string(item.id);
        output += ", \"aspect\": " + std::to_string(item.aspect);
        output += ", \"offsetX\": " + std::to_string(item.rackOffsetX);
        output += ", \"locked\": ";
        output += item.locked ? "true" : "false";
        output += ", \"addedMs\": " + std::to_string(item.addedMs);
        output += ", \"note\": " + json::Quote(util::WideToUtf8(item.note));
        output += ", \"noteColor\": " + std::to_string(item.noteColor);
        output += " }";
    }
    output += "\n  ]\n}\n";

    const std::wstring target = FilePath();
    const std::wstring temp = target + L".tmp";
    if (!util::WriteFileBytes(temp, output.data(), output.size())) {
        RackLog(L"ItemStore: failed to write %s", temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        RackLog(L"ItemStore: failed to replace %s (%lu)", target.c_str(), GetLastError());
        util::DeleteFileIfExists(temp);
        return false;
    }
    return true;
}
