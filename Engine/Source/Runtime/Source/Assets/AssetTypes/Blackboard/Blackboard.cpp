#include "RuntimePCH.h"
#include "Blackboard.h"

namespace Lumina
{
    int32 CBlackboard::FindKeyIndex(const FName& Name) const
    {
        for (int32 i = 0; i < (int32)Keys.size(); ++i)
        {
            if (Keys[i].Name == Name)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    const FBlackboardKey* CBlackboard::FindKey(const FName& Name) const
    {
        const int32 Index = FindKeyIndex(Name);
        return Index == INDEX_NONE ? nullptr : &Keys[Index];
    }

    EBlackboardKeyType CBlackboard::GetKeyType(const FName& Name, EBlackboardKeyType Fallback) const
    {
        const FBlackboardKey* Key = FindKey(Name);
        return Key == nullptr ? Fallback : Key->Type;
    }

    FName CBlackboard::GetKeyName(int32 Index) const
    {
        return (Index >= 0 && Index < (int32)Keys.size()) ? Keys[Index].Name : FName();
    }
}
