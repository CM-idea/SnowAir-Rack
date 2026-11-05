#include "ItemStore.h"

#include "MiniJson.h"
#include "Util.h"

namespace {

bool IsTextLike(card::Type type)
{
    return type == card::Type::Note || type == card::Type::Text ||
           type == card::Type::Link || type == card::Type::Color;
}

int CardTypeToInt(card::Type type)
{
    switch (type) {
    case card::Type::Image: return 0;
    case card::Type::Svg: return 1;
    case card::Type::Recording: return 2;
    case card::Type::Note: return 3;
    case card::Type::Text: return 4;
    case card::Type::Link: return 5;
    case card::Type::Color: return 6;
    case card::Type::File: return 7;
    case card::Type::Video: return 8;
    case card::Type::Music: return 9;
    }
    return 0;
}

card::Type CardTypeFromInt(int value)
{
    switch (value) {
    case 0: return card::Type::Image;
    case 1: return card::Type::Svg;
    case 2: return card::Type::Recording;
    case 3: return card::Type::Note;
    case 4: return card::Type::Text;
    case 5: return card::Type::Link;
    case 6: return card::Type::Color;
    case 7: return card::Type::File;
    case 8: return card::Type::Video;
    case 9: return card::Type::Music;
    default: return card::Type::Image;
    }
}

card::Type DeriveCardType(int kind, bool hasSvg, bool hasCover,
                          const std::vector<std::wstring>& paths,
                          const std::wstring& path)
{
    if (hasSvg) return card::Type::Svg;
    switch (kind) {
    case 0: return card::Type::Image;
    case 1: return card::Type::Note;
    case 2: return hasCover ? card::Type::Recording : card::Type::Video;
    case 3: {
        std::vector<std::wstring> files = paths;
        if (files.empty() && !path.empty()) files.push_back(path);
        return card::FileType(files);
    }
    default: return card::Type::File;
    }
}

} /* namespace */

std::wstring ItemStore::FilePath() const
{
    return util::JoinPath(util::GetDataDir(), L"rack_items.json");
}

bool ItemStore::Load(std::vector<Item>& items) const
{
    items.clear();
    std::vector<BYTE> bytes;
    const bool loaded = util::ReadFileBytes(FilePath(), bytes);
    if (!loaded || bytes.empty()) {
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
        std::wstring svgId;
        if (const json::Value* sp = value.Get("svgId");
            sp && sp->type == json::Type::String) {
            svgId = util::Utf8ToWide(sp->text);
        }
        const bool hasPath = path && path->type == json::Type::String && !path->text.empty();
        Item item;
        item.kind = hasKind ? kind : (hasPath ? 0 : 1);
        item.label = label;
        if (!thumbPath.empty()) item.thumbPath = util::FullPath(thumbPath);
        item.svgId = svgId;
        std::vector<std::wstring> paths;
        if (const json::Value* pathsValue = value.Get("paths");
            pathsValue && pathsValue->type == json::Type::Array) {
            for (const auto& entry : pathsValue->array) {
                if (entry.type == json::Type::String && !entry.text.empty()) {
                    paths.push_back(util::FullPath(util::Utf8ToWide(entry.text)));
                }
            }
        }
        const std::wstring rawPath =
            hasPath ? util::Utf8ToWide(path->text) : std::wstring();
        if (paths.empty() && !rawPath.empty()) {
            paths.push_back(util::FullPath(rawPath));
        }
        if (const json::Value* typeValue = value.Get("cardType");
            typeValue && typeValue->type == json::Type::Number) {
            item.cardType = CardTypeFromInt(static_cast<int>(typeValue->number));
        } else {
            item.cardType = DeriveCardType(item.kind, !svgId.empty(),
                                           !thumbPath.empty(), paths, rawPath);
        }
        if (const json::Value* created = value.Get("createdAt");
            created && created->type == json::Type::Number) {
            item.createdAt = static_cast<long long>(created->number);
        }
        if (const json::Value* accent = value.Get("accent");
            accent && accent->type == json::Type::Number) {
            item.accent = static_cast<uint32_t>(accent->number);
        }
        if (const json::Value* accentSet = value.Get("accentSet");
            accentSet && accentSet->type == json::Type::Bool) {
            item.accentSet = accentSet->boolean;
        }
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
        if (const json::Value* hashValue = value.Get("hash");
            hashValue && hashValue->type == json::Type::String && !hashValue->text.empty()) {
            item.contentHash = _wcstoui64(util::Utf8ToWide(hashValue->text).c_str(), nullptr, 16);
        }
        if (const json::Value* color = value.Get("noteColor");
            color && color->type == json::Type::Number) {
            item.noteColor = rack::ClampInt(static_cast<int>(color->number), 0, 2);
        }
        if (hasNote) item.note = util::Utf8ToWide(noteValue->text);
        if (const json::Value* noteId = value.Get("noteId");
            noteId && noteId->type == json::Type::String) {
            item.noteId = util::Utf8ToWide(noteId->text);
        }

        if (!hasPath) {
            /* 无路径：便签 / 文本 / 链接 / 颜色 / 纯文字文件 */
            if (!hasNote && item.noteId.empty() && item.label.empty()) continue;
            item.aspect = 1.1f;
            item.paths = paths;
            items.push_back(std::move(item));
            continue;
        }
        item.paths = paths;
        item.path = item.paths.empty() ? util::FullPath(rawPath) : item.paths.front();
        if (item.paths.empty()) item.paths.push_back(item.path);
        item.sourcePath = item.path;
        if (const json::Value* source = value.Get("sourcePath");
            source && source->type == json::Type::String) {
            const std::wstring sourcePath = util::Utf8ToWide(source->text);
            if (!sourcePath.empty()) item.sourcePath = sourcePath;
        }
        /* 文件暂时找不到也保留卡片：宁可显示「无法读取」，
           也不要让它悄悄从绳子上消失（用户会以为记录被清空）。 */
        if (item.path.empty()) item.aspect = 1.1f;
        item.mtime = util::FileWriteTime(item.path);
        /* 老数据没有指纹：补算一次，之后就能靠内容认文件。 */
        if (item.contentHash == 0 && util::FileExists(item.path)) {
            item.contentHash = util::FileContentHash(item.path);
        }
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
        const bool textCard = IsTextLike(item.cardType);
        if (item.path.empty() && !textCard && item.note.empty() && item.label.empty()) {
            continue;
        }
        /* 文字类正文在轻雪里；本地只留 noteId，不写正文。 */
        if (textCard && item.noteId.empty()) {
            continue;
        }
        if (!first) output += ",\n";
        first = false;
        output += "    { \"kind\": " + std::to_string(item.kind);
        output += ", \"cardType\": " + std::to_string(CardTypeToInt(item.cardType));
        output += ", \"path\": " + json::Quote(util::WideToUtf8(item.path));
        output += ", \"sourcePath\": " + json::Quote(util::WideToUtf8(item.sourcePath));
        output += ", \"thumbPath\": " + json::Quote(util::WideToUtf8(item.thumbPath));
        output += ", \"svgId\": " + json::Quote(util::WideToUtf8(item.svgId));
        output += ", \"label\": " + json::Quote(util::WideToUtf8(item.label));
        output += ", \"createdAt\": " + std::to_string(item.createdAt);
        output += ", \"accent\": " + std::to_string(item.accent);
        output += ", \"accentSet\": ";
        output += item.accentSet ? "true" : "false";
        output += ", \"paths\": [";
        for (size_t pi = 0; pi < item.paths.size(); ++pi) {
            if (pi > 0) output += ", ";
            output += json::Quote(util::WideToUtf8(item.paths[pi]));
        }
        output += "]";
        output += ", \"id\": " + std::to_string(item.id);
        output += ", \"aspect\": " + std::to_string(item.aspect);
        output += ", \"offsetX\": " + std::to_string(item.rackOffsetX);
        output += ", \"locked\": ";
        output += item.locked ? "true" : "false";
        output += ", \"addedMs\": " + std::to_string(item.addedMs);
        output += ", \"noteId\": " + json::Quote(util::WideToUtf8(item.noteId));
        output += ", \"note\": ";
        output += json::Quote(textCard ? std::string() : util::WideToUtf8(item.note));
        output += ", \"noteColor\": " + std::to_string(item.noteColor);
        {
            wchar_t hashText[32]{};
            swprintf_s(hashText, L"%016llX", static_cast<unsigned long long>(item.contentHash));
            output += ", \"hash\": " + json::Quote(util::WideToUtf8(hashText));
        }
        output += " }";
    }
    output += "\n  ]\n}\n";

    const std::wstring target = FilePath();
    const std::wstring temp = target + L".tmp";
    /* 覆盖之前先把上一版留成 .bak：万一哪次写坏了还能捞回来。 */
    if (util::FileExists(target)) {
        const std::wstring backup = target + L".bak";
        DeleteFileW(backup.c_str());
        CopyFileW(target.c_str(), backup.c_str(), FALSE);
    }
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
