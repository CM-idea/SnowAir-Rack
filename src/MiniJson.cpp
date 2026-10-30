#include "MiniJson.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::Get(const char* key) const
{
    if (type != Type::Object || !key) {
        return nullptr;
    }
    for (const auto& entry : object) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const std::string& Value::StringOr(const char* fallback) const
{
    static const std::string empty;
    if (type == Type::String) {
        return text;
    }
    if (fallback) {
        static thread_local std::string fallbackValue;
        fallbackValue = fallback;
        return fallbackValue;
    }
    return empty;
}

int Value::IntOr(int fallback) const
{
    if (type == Type::Number) {
        return static_cast<int>(number);
    }
    if (type == Type::Bool) {
        return boolean ? 1 : 0;
    }
    return fallback;
}

namespace {

class Parser {
public:
    Parser(const std::string& input) : begin_(input.data()), cursor_(input.data()),
                                       end_(input.data() + input.size()) {}

    bool ParseDocument(Value& out)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipWhitespace();
        return cursor_ == end_;
    }

private:
    void SkipWhitespace()
    {
        while (cursor_ < end_) {
            char ch = *cursor_;
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                ++cursor_;
            } else {
                break;
            }
        }
    }

    bool Consume(char ch)
    {
        if (cursor_ < end_ && *cursor_ == ch) {
            ++cursor_;
            return true;
        }
        return false;
    }

    bool ParseValue(Value& out, int depth)
    {
        if (depth > 64) {
            return false;
        }
        SkipWhitespace();
        if (cursor_ >= end_) {
            return false;
        }
        switch (*cursor_) {
        case '{':
            return ParseObject(out, depth + 1);
        case '[':
            return ParseArray(out, depth + 1);
        case '"':
            out.type = Type::String;
            return ParseString(out.text);
        case 't':
            if (end_ - cursor_ >= 4 && std::memcmp(cursor_, "true", 4) == 0) {
                cursor_ += 4;
                out.type = Type::Bool;
                out.boolean = true;
                return true;
            }
            return false;
        case 'f':
            if (end_ - cursor_ >= 5 && std::memcmp(cursor_, "false", 5) == 0) {
                cursor_ += 5;
                out.type = Type::Bool;
                out.boolean = false;
                return true;
            }
            return false;
        case 'n':
            if (end_ - cursor_ >= 4 && std::memcmp(cursor_, "null", 4) == 0) {
                cursor_ += 4;
                out.type = Type::Null;
                return true;
            }
            return false;
        default:
            return ParseNumber(out);
        }
    }

    bool ParseObject(Value& out, int depth)
    {
        if (!Consume('{')) {
            return false;
        }
        out.type = Type::Object;
        out.object.clear();
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            Value value;
            if (!ParseValue(value, depth)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool ParseArray(Value& out, int depth)
    {
        if (!Consume('[')) {
            return false;
        }
        out.type = Type::Array;
        out.array.clear();
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            Value value;
            if (!ParseValue(value, depth)) {
                return false;
            }
            out.array.emplace_back(std::move(value));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool ParseString(std::string& out)
    {
        if (!Consume('"')) {
            return false;
        }
        out.clear();
        while (cursor_ < end_) {
            unsigned char ch = static_cast<unsigned char>(*cursor_++);
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (cursor_ >= end_) {
                    return false;
                }
                char esc = *cursor_++;
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned codepoint = 0;
                    if (!ParseHex4(codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && end_ - cursor_ >= 6 &&
                        cursor_[0] == '\\' && cursor_[1] == 'u') {
                        cursor_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return false;
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(out, codepoint);
                    break;
                }
                default:
                    return false;
                }
            } else if (ch < 0x20) {
                return false;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
        return false;
    }

    bool ParseHex4(unsigned& value)
    {
        if (end_ - cursor_ < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            char ch = cursor_[i];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<unsigned>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<unsigned>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<unsigned>(ch - 'A' + 10);
            } else {
                return false;
            }
        }
        cursor_ += 4;
        return true;
    }

    static void AppendUtf8(std::string& out, unsigned codepoint)
    {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool ParseNumber(Value& out)
    {
        const char* start = cursor_;
        if (cursor_ < end_ && *cursor_ == '-') {
            ++cursor_;
        }
        bool any = false;
        while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
            ++cursor_;
            any = true;
        }
        if (cursor_ < end_ && *cursor_ == '.') {
            ++cursor_;
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
                ++cursor_;
                any = true;
            }
        }
        if (cursor_ < end_ && (*cursor_ == 'e' || *cursor_ == 'E')) {
            ++cursor_;
            if (cursor_ < end_ && (*cursor_ == '+' || *cursor_ == '-')) {
                ++cursor_;
            }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9') {
                ++cursor_;
                any = true;
            }
        }
        if (!any) {
            return false;
        }
        std::string number(start, cursor_);
        out.type = Type::Number;
        out.number = std::strtod(number.c_str(), nullptr);
        return true;
    }

    const char* begin_;
    const char* cursor_;
    const char* end_;
};

} /* namespace */

bool Parse(const std::string& input, Value& out)
{
    if (input.empty()) {
        return false;
    }
    Parser parser(input);
    return parser.ParseDocument(out);
}

std::string Quote(const std::string& utf8)
{
    std::string out;
    out.reserve(utf8.size() + 2);
    out.push_back('"');
    for (unsigned char ch : utf8) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[7]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

} /* namespace json */
