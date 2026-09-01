#include "MetadataUtils.h"

namespace
{
    constexpr std::string_view kWhitespace = " \t\n\r\f\v";

    constexpr std::string_view Trim(std::string_view Value)
    {
        const size_t First = Value.find_first_not_of(kWhitespace);
        if (First == std::string_view::npos)
        {
            return {};
        }

        return Value.substr(First, Value.find_last_not_of(kWhitespace) - First + 1);
    }

    constexpr std::string_view StripQuotes(std::string_view Value)
    {
        for (const char Quote : { '"', '\'', '`' })
        {
            if (Value.size() >= 2 && Value.front() == Quote && Value.back() == Quote)
            {
                Value = Value.substr(1, Value.size() - 2);
            }
        }

        return Value;
    }

    constexpr std::string_view Sanitize(std::string_view Value)
    {
        return Trim(StripQuotes(Trim(Value)));
    }

    constexpr bool IsEven(uint32_t Value)
    {
        return Value % 2 == 0;
    }

    // A comma inside a quoted value separates nothing, so the split tracks quote parity.
    constexpr size_t FindSeparator(std::string_view Text, char Separator)
    {
        uint32_t QuoteCount = 0;

        for (size_t Index = 0; Index < Text.size(); ++Index)
        {
            if (Text[Index] == Separator && IsEven(QuoteCount))
            {
                return Index;
            }

            if (Text[Index] == '"')
            {
                ++QuoteCount;
            }
        }

        return std::string_view::npos;
    }
}

void FMetadataParser::Parse(std::string_view Raw)
{
    Metadata.clear();

    while (!Raw.empty())
    {
        const size_t Comma = FindSeparator(Raw, ',');
        const std::string_view Entry = Trim(Raw.substr(0, Comma));

        Raw = Comma == std::string_view::npos ? std::string_view() : Raw.substr(Comma + 1);

        if (Entry.empty() && Comma == std::string_view::npos)
        {
            break;
        }

        const size_t Equals = FindSeparator(Entry, '=');

        FMetadataPair& Pair = Metadata.emplace_back();
        Pair.Key = Sanitize(Entry.substr(0, Equals));
        if (Equals != std::string_view::npos)
        {
            Pair.Value = Sanitize(Entry.substr(Equals + 1));
        }
    }
}
