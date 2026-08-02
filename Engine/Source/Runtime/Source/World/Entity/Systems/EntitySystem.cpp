#include "RuntimePCH.h"
#include "EntitySystem.h"

namespace Lumina
{
    FSystemRegistry& FSystemRegistry::Get()
    {
        static FSystemRegistry Instance;
        return Instance;
    }

    void FSystemRegistry::Register(const FNativeSystemDesc& Desc)
    {
        Systems.push_back(Desc);
    }
}
