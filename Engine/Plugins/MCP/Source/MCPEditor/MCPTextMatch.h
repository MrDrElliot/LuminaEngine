#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"

namespace Lumina::MCP
{
    // An empty needle matches everything, since a filter nobody set should not exclude anything.
    NODISCARD inline bool ContainsText(FStringView Haystack, const FString& Needle)
    {
        return Needle.IsEmpty() || Haystack.Find(FStringView(Needle)) != Constants::kIndexNone;
    }

    NODISCARD inline bool EqualsTextFold(FStringView A, FStringView B)
    {
        if (A.Len() != B.Len())
        {
            return false;
        }

        const auto Lower = [](char Character)
        {
            return (Character >= 'A' && Character <= 'Z') ? static_cast<char>(Character - 'A' + 'a') : Character;
        };

        for (int32 Index = 0; Index < A.Len(); ++Index)
        {
            if (Lower(A[Index]) != Lower(B[Index]))
            {
                return false;
            }
        }

        return true;
    }

    NODISCARD inline bool ContainsTextFold(FStringView Haystack, const FString& Needle)
    {
        if (Needle.IsEmpty())
        {
            return true;
        }

        if (Needle.Len() > Haystack.Len())
        {
            return false;
        }

        const auto Lower = [](char Character)
        {
            return (Character >= 'A' && Character <= 'Z') ? static_cast<char>(Character - 'A' + 'a') : Character;
        };

        for (int32 Start = 0; Start + Needle.Len() <= Haystack.Len(); ++Start)
        {
            bool bMatches = true;
            for (int32 Index = 0; Index < Needle.Len() && bMatches; ++Index)
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
