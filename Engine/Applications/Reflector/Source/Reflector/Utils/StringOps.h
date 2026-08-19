#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace Lumina::StringOps
{
    inline void ToLower(std::string& Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(),
            [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
    }

    inline void ToUpper(std::string& Value)
    {
        std::transform(Value.begin(), Value.end(), Value.begin(),
            [](unsigned char Character) { return static_cast<char>(std::toupper(Character)); });
    }

    inline void TrimStart(std::string& Value)
    {
        const size_t First = Value.find_first_not_of(" \t\n\r\f\v");
        Value.erase(0, First == std::string::npos ? Value.size() : First);
    }

    inline void TrimEnd(std::string& Value)
    {
        const size_t Last = Value.find_last_not_of(" \t\n\r\f\v");
        Value.resize(Last == std::string::npos ? 0 : Last + 1);
    }
}
