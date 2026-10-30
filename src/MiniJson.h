#pragma once

#include <string>
#include <utility>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* Get(const char* key) const;
    const std::string& StringOr(const char* fallback) const;
    int IntOr(int fallback) const;
};

bool Parse(const std::string& input, Value& out);
std::string Quote(const std::string& utf8);

} /* namespace json */
