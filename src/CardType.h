#pragma once

/*
 * 非图片卡片的类型与颜色解析。
 *
 * 只放纯函数：不碰 D2D / Win32，方便单独跑测试。
 * 卡片颜色规则以用户的复制习惯为准：
 *   - 空格 / 换行和 '#' 都是分隔符，'#' 同时是 hex 前缀；
 *   - 裸 6 位、8 位 hex 算颜色；裸 3 位只有在“多段颜色”里才算；
 *   - rgb() / rgba() 算颜色，8 位 hex 的 alpha 在最后两位（RRGGBBAA）；
 *   - 一段话里夹着色值不算，必须整段都是色值。
 */

#include "Common.h"

#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace card {

enum class Type {
    Image,      /* 截图 / 图片，沿用现有照片卡 */
    Svg,        /* SVG 预览，沿用现有照片卡 */
    Recording,  /* 录屏且有封面，沿用现有照片卡 + 相机角标 */
    Note,       /* 便签，黄色纸卡 */
    Text,       /* 普通文本 */
    Link,       /* 单个 URL */
    Color,      /* 一个或多个色值 */
    File,       /* 普通文件 / 文件夹 / 混搭 */
    Video,      /* 视频文件 / 无封面录屏 */
    Music       /* 音频文件 */
};

struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

inline bool IsSpace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L'\v' ||
           ch == L'\f';
}

inline std::wstring Trim(const std::wstring& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

inline bool IsHexDigit(wchar_t ch)
{
    return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
           (ch >= L'A' && ch <= L'F');
}

inline int HexValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

inline bool ParseHexColor(const std::wstring& digits, bool prefixed, Rgba& out)
{
    if (!(digits.size() == 3 || digits.size() == 6 || digits.size() == 8)) {
        return false;
    }
    std::wstring full = digits;
    if (full.size() == 3) {
        full = {full[0], full[0], full[1], full[1], full[2], full[2]};
    }
    auto byteAt = [&](size_t index) -> int {
        return HexValue(full[index * 2]) * 16 + HexValue(full[index * 2 + 1]);
    };
    for (size_t i = 0; i + 1 < full.size(); i += 2) {
        if (HexValue(full[i]) < 0 || HexValue(full[i + 1]) < 0) return false;
    }
    out.r = static_cast<uint8_t>(byteAt(0));
    out.g = static_cast<uint8_t>(byteAt(1));
    out.b = static_cast<uint8_t>(byteAt(2));
    out.a = full.size() == 8 ? static_cast<uint8_t>(byteAt(3)) : 255;
    (void)prefixed;
    return true;
}

/* 解析一组 rgb()/rgba()；失败返回 false。 */
inline bool ParseRgbFunction(const std::wstring& text, size_t open, size_t close, bool alpha,
                             Rgba& out)
{
    const std::wstring body = text.substr(open + 1, close - open - 1);
    std::vector<double> values;
    size_t start = 0;
    for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == L',') {
            std::wstring part = Trim(body.substr(start, i - start));
            if (part.empty()) return false;
            wchar_t* end = nullptr;
            const double value = wcstod(part.c_str(), &end);
            if (!end || *end != L'\0') return false;
            values.push_back(value);
            start = i + 1;
        }
    }
    if (values.size() != (alpha ? 4u : 3u)) return false;
    for (int i = 0; i < 3; ++i) {
        if (values[static_cast<size_t>(i)] < 0.0 || values[static_cast<size_t>(i)] > 255.0) {
            return false;
        }
    }
    out.r = static_cast<uint8_t>(values[0] + 0.5);
    out.g = static_cast<uint8_t>(values[1] + 0.5);
    out.b = static_cast<uint8_t>(values[2] + 0.5);
    out.a = 255;
    if (alpha) {
        double a = values[3];
        if (a < 0.0) return false;
        if (a > 1.0 && a <= 255.0) a /= 255.0;
        if (a > 1.0) return false;
        out.a = static_cast<uint8_t>(a * 255.0 + 0.5);
    }
    return true;
}

/*
 * 整段解析为颜色。成功返回 true，colors 按原顺序排列。
 * 只要有一个字符不是色值/分隔符就失败（例如“一段话 #34C759”）。
 */
inline bool ParseColors(const std::wstring& source, std::vector<Rgba>& colors)
{
    colors.clear();
    const std::wstring text = Trim(source);
    if (text.empty()) return false;

    struct Token {
        Rgba color;
        bool prefixed = false;
        bool threeDigits = false;
    };
    std::vector<Token> tokens;

    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && IsSpace(text[i])) ++i;
        if (i >= text.size()) break;

        if (text[i] == L'#') {
            ++i;
            const size_t start = i;
            while (i < text.size() && IsHexDigit(text[i])) ++i;
            if (i == start) return false;
            Token token;
            token.prefixed = true;
            const std::wstring digits = text.substr(start, i - start);
            token.threeDigits = digits.size() == 3;
            if (!ParseHexColor(digits, true, token.color)) return false;
            tokens.push_back(token);
            continue;
        }

        const bool isRgba =
            (i + 4 <= text.size()) && _wcsnicmp(text.c_str() + i, L"rgba", 4) == 0;
        const bool isRgb = !isRgba && (i + 3 <= text.size()) &&
                           _wcsnicmp(text.c_str() + i, L"rgb", 3) == 0;
        if ((isRgb || isRgba) && i + (isRgba ? 4 : 3) < text.size() &&
            text[i + (isRgba ? 4 : 3)] == L'(') {
            const size_t prefixLen = isRgba ? 4 : 3;
            const size_t open = i + prefixLen;
            const size_t close = text.find(L')', open + 1);
            if (close == std::wstring::npos) return false;
            Token token;
            if (!ParseRgbFunction(text, open, close, isRgba, token.color)) return false;
            tokens.push_back(token);
            i = close + 1;
            continue;
        }

        /* 裸 hex：连续的 hex 字符合成一个 token；12 位这种宽度非法。 */
        const size_t start = i;
        while (i < text.size() && IsHexDigit(text[i])) ++i;
        if (i == start) return false; /* 逗号、句号、中文等都不是分隔符 */
        Token token;
        const std::wstring digits = text.substr(start, i - start);
        token.threeDigits = digits.size() == 3;
        if (!ParseHexColor(digits, false, token.color)) return false;
        tokens.push_back(token);
    }

    if (tokens.empty()) return false;
    /*
     * 裸 3 位 hex 单独出现时太像数字（666），不算颜色；
     * 但只要整段是多段颜色（000 333 / #000 666），它就是一个颜色。
     */
    if (tokens.size() == 1 && tokens.front().threeDigits && !tokens.front().prefixed) {
        return false;
    }

    colors.reserve(tokens.size());
    for (const Token& token : tokens) colors.push_back(token.color);
    return true;
}

/* 这张颜色卡最初复制的是 rgb()/rgba() 还是 hex。用于决定 hex/rgb 两行
   谁在上：同一个颜色用两种格式复制时，卡片外观也能区分开。 */
inline bool ColorTextIsRgb(const std::wstring& source)
{
    size_t i = 0;
    while (i < source.size() && std::iswspace(source[i])) ++i;
    auto letter = [&](size_t at, wchar_t lower) {
        return at < source.size() &&
               std::towlower(source[at]) == static_cast<wint_t>(lower);
    };
    if (!letter(i, L'r') || !letter(i + 1, L'g') || !letter(i + 2, L'b')) {
        return false;
    }
    size_t j = i + 3;
    if (letter(j, L'a')) ++j;
    while (j < source.size() && std::iswspace(source[j])) ++j;
    return j < source.size() && source[j] == L'(';
}

inline bool AllPathsAreFolders(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) {
        return false;
    }
    for (const auto& path : paths) {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec) || ec) {
            return false;
        }
    }
    return true;
}

/* 全是文件夹 → 「文件夹」；文件、混搭 → 「文件」。 */
inline std::wstring FileLabel(const std::vector<std::wstring>& paths)
{
    return AllPathsAreFolders(paths) ? L"文件夹" : L"文件";
}

inline bool IsLink(const std::wstring& source)
{
    const std::wstring text = Trim(source);
    if (text.size() < 8) return false;
    for (wchar_t ch : text) {
        if (IsSpace(ch)) return false;
    }
    const std::wstring lower = [&]() {
        std::wstring value = text;
        for (wchar_t& ch : value) ch = static_cast<wchar_t>(towlower(ch));
        return value;
    }();
    const bool http = lower.rfind(L"http://", 0) == 0;
    const bool https = lower.rfind(L"https://", 0) == 0;
    if (!http && !https) return false;
    return text.size() > (https ? 8u : 7u);
}

inline std::wstring HexText(const Rgba& color)
{
    wchar_t buffer[16]{};
    if (color.a < 255) {
        swprintf_s(buffer, L"#%02X%02X%02X%02X", static_cast<unsigned>(color.r),
                   static_cast<unsigned>(color.g), static_cast<unsigned>(color.b),
                   static_cast<unsigned>(color.a));
    } else {
        swprintf_s(buffer, L"#%02X%02X%02X", static_cast<unsigned>(color.r),
                   static_cast<unsigned>(color.g), static_cast<unsigned>(color.b));
    }
    return buffer;
}

inline std::wstring RgbText(const Rgba& color)
{
    wchar_t buffer[48]{};
    swprintf_s(buffer, L"rgb(%u,%u,%u)", static_cast<unsigned>(color.r),
               static_cast<unsigned>(color.g), static_cast<unsigned>(color.b));
    return buffer;
}

inline std::wstring RgbaText(const Rgba& color)
{
    double alpha = static_cast<double>(color.a) / 255.0;
    wchar_t alphaText[16]{};
    swprintf_s(alphaText, L"%.2f", alpha);
    std::wstring value = alphaText;
    while (!value.empty() && value.back() == L'0') value.pop_back();
    if (!value.empty() && value.back() == L'.') value.pop_back();
    if (value.empty()) value = L"0";
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"rgba(%u,%u,%u,%s)", static_cast<unsigned>(color.r),
               static_cast<unsigned>(color.g), static_cast<unsigned>(color.b), value.c_str());
    return buffer;
}

/* 颜色卡整体拖出时：带 alpha 用 rgba，不透明用 hex。 */
inline std::wstring ColorDragText(const Rgba& color)
{
    return color.a < 255 ? RgbaText(color) : HexText(color);
}

inline bool IsAudioExtension(std::wstring extension)
{
    if (extension.empty()) return false;
    if (extension[0] == L'.') extension.erase(0, 1);
    for (wchar_t& ch : extension) ch = static_cast<wchar_t>(towlower(ch));
    static const wchar_t* kAudio[] = {L"mp3", L"wav", L"flac", L"aac", L"m4a", L"ogg",
                                      L"wma", L"ape", L"opus", L"aiff", L"mid", L"midi"};
    for (const wchar_t* value : kAudio) {
        if (extension == value) return true;
    }
    return false;
}

inline bool IsVideoExtension(std::wstring extension)
{
    if (extension.empty()) return false;
    if (extension[0] == L'.') extension.erase(0, 1);
    for (wchar_t& ch : extension) ch = static_cast<wchar_t>(towlower(ch));
    static const wchar_t* kVideo[] = {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"flv",
                                      L"webm", L"m4v", L"mpg", L"mpeg", L"ts", L"3gp"};
    for (const wchar_t* value : kVideo) {
        if (extension == value) return true;
    }
    return false;
}

inline Type TextType(const std::wstring& text)
{
    if (IsLink(text)) return Type::Link;
    std::vector<Rgba> colors;
    if (ParseColors(text, colors)) return Type::Color;
    return Type::Text;
}

inline Type FileType(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return Type::File;
    bool allAudio = true;
    bool allVideo = true;
    for (const std::wstring& path : paths) {
        const std::wstring extension = std::filesystem::path(path).extension().wstring();
        allAudio = allAudio && IsAudioExtension(extension);
        allVideo = allVideo && IsVideoExtension(extension);
    }
    if (allAudio) return Type::Music;
    if (allVideo) return Type::Video;
    return Type::File;
}

inline const wchar_t* TypeLabel(Type type)
{
    switch (type) {
    case Type::Text: return L"文本";
    case Type::Link: return L"链接";
    case Type::Color: return L"颜色";
    case Type::File: return L"文件";
    case Type::Video: return L"视频";
    case Type::Music: return L"音乐";
    case Type::Note: return L"便签";
    case Type::Image: return L"图片";
    case Type::Svg: return L"SVG";
    case Type::Recording: return L"录屏";
    }
    return L"卡片";
}

/* 默认强调色（0xRRGGBB）。图片/SVG/有封面录屏没有色带，返回 0。 */
inline uint32_t DefaultAccent(Type type)
{
    switch (type) {
    case Type::Text: return 0xAC7F5E;
    case Type::Link: return 0x1677FF;
    case Type::File: return 0xFF8D28;
    case Type::Video: return 0xFF383C;
    case Type::Music: return 0xCB30E0;
    case Type::Color: return 0x34C759;
    case Type::Note: return 0xF3CD5F;
    default: return 0;
    }
}

inline bool HasAccent(Type type)
{
    return DefaultAccent(type) != 0;
}

/* 头部第二行时间：11-05 16:27。 */
inline std::wstring FormatCardTime(long long milliseconds)
{
    if (milliseconds <= 0) return std::wstring();
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    struct tm local {};
    if (localtime_s(&local, &seconds) != 0) return std::wstring();
    wchar_t buffer[32]{};
    wcsftime(buffer, 32, L"%m-%d %H:%M", &local);
    return buffer;
}

inline std::wstring FileName(const std::wstring& path)
{
    return std::filesystem::path(path).filename().wstring();
}

} /* namespace card */
