#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"

namespace Lumina::MCP
{
    // An empty needle matches everything, since a filter nobody set should not exclude anything.
    NODISCARD inline bool ContainsText(FStringView Haystack, const FString& Needle)
    {
        return Needle.empty() || Haystack.find(FStringView(Needle)) != FStringView::npos;
    }

    NODISCARD inline bool ContainsTextFold(FStringView Haystack, const FString& Needle)
    {
        if (Needle.empty())
        {
            return true;
        }

        if (Needle.size() > Haystack.size())
        {
            return false;
        }

        const auto Lower = [](char Character)
        {
            return (Character >= 'A' && Character <= 'Z') ? static_cast<char>(Character - 'A' + 'a') : Character;
        };

        for (size_t Start = 0; Start + Needle.size() <= Haystack.size(); ++Start)
        {
            bool bMatches = true;
            for (size_t Index = 0; Index < Needle.size() && bMatches; ++Index)
            {
                bMatches = Lower(Haystack[Start + Index]) == Lower(Needle[Index]);
            }

            if (bMatches)
            {
                return true;
            }
        }

        return false;
    }
}
