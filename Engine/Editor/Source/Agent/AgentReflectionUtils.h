#pragma once

#include "Containers/Algorithm.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"

#include <string>

namespace Lumina::Agent::Detail
{
    inline std::string ToStandard(FStringView Text)
    {
        return std::string(Text.data(), Text.size());
    }

    // Base first, so a derived struct's own fields read after the ones it inherited.
    inline TVector<CStruct*> CollectStructChain(CStruct* Struct)
    {
        TVector<CStruct*> Chain;
        for (CStruct* Current = Struct; Current != nullptr; Current = Current->GetSuperStruct())
        {
            Chain.push_back(Current);
        }

        Algo::Reverse(Chain.begin(), Chain.end());
        return Chain;
    }
}
