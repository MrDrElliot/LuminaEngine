#include "MetadataUtils.h"

static inline void SanitizeKeyValueString(eastl::string& OutString)
{
    if (OutString.empty())
    {
        return;
    }

    OutString.ltrim();
    OutString.rtrim();

    // Strip pairs of quotes
    auto StripQuotes = [&OutString] ( char quote )
    {
        if (OutString.front() == quote && OutString.back() == quote)
        {
            OutString = OutString.substr(1, OutString.length() - 2);
        }
    };

    StripQuotes('"');
    StripQuotes('\'');
    StripQuotes('`');

    OutString.ltrim();
    OutString.rtrim();
}

void FMetadataParser::Parse(const eastl::string& Raw)
{
    eastl::string RawCopy = Raw;
    
    auto IsEven = [] (uint32_t Value) -> bool
    {
        return Value % 2 == 0;
    };
    
    Metadata.clear();

    // Fancier split since we might have delimiter characters inside a string block
    eastl::vector<eastl::string> results;

    eastl::string currentToken;
    int32_t quoteCount = 0;
    int32_t length = (int32_t) RawCopy.length();
    for (int32_t i = 0; i < length; i++)
    {
        if (RawCopy[i] == ',' && IsEven(quoteCount))
        {
            currentToken.rtrim();
            results.emplace_back( currentToken );
            currentToken.clear();
            quoteCount = 0;
            continue;
        }

        if (RawCopy[i] == '"')
        {
            quoteCount++;
        }

        // Strip leading whitespace
        if (!currentToken.empty() || RawCopy[i] != ' ')
        {
            currentToken.append(1, RawCopy[i]);
        }
    }

    if (!currentToken.empty())
    {
        currentToken.rtrim();
        results.emplace_back(currentToken);
        currentToken.clear();
        quoteCount = 0;
    }

    // Split results using the '=' char
    for (eastl::string& part : results)
    {
        int32_t separatorIdx = -1;
        length = (int32_t) part.length();
        for (int32_t i = 0; i < length; i++)
        {
            if (part[i] == '=' && IsEven(quoteCount))
            {
                separatorIdx = i;
                break;
            }

            if (part[i] == '"')
            {
                quoteCount++;
            }
        }

        FMetadataPair& kv = Metadata.emplace_back();
        if (separatorIdx != -1)
        {
            kv.Key = part.substr(0, separatorIdx);
            kv.Value = part.substr(separatorIdx + 1, part.length() - separatorIdx - 1);
        }
        else
        {
            kv.Key = part;
        }

        SanitizeKeyValueString(kv.Key);
        SanitizeKeyValueString(kv.Value);
    }
}
