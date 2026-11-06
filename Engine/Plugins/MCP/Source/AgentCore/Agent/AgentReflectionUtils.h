#pragma once

#include "Containers/Algorithm.h"
#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"

namespace Lumina::Agent::Detail
{
    // Base first, so a derived struct's own fields read after the ones it inherited.
    inline TVector<CStruct*> CollectStructChain(CStruct* Struct)
    {
        TVector<CStruct*> Chain;
        for (CStruct* Current = Struct; Current != nullptr; Current = Current->GetSuperStruct())
        {
            Chain.Add(Current);
        }

        Algo::Reverse(Chain);
        return Chain;
    }
}
