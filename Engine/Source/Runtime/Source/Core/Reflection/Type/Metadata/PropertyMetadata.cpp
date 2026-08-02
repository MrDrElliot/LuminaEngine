#include "RuntimePCH.h"
#include "PropertyMetadata.h"

namespace Lumina
{
    void FMetaDataPair::AddValue(const FName& Key, const FString& Value)
    {
        PairParams.emplace(Key, Value);
    }

    bool FMetaDataPair::HasMetadata(const FName& Key) const
    {
        return PairParams.find(Key) != PairParams.end();
    }

    const FString* FMetaDataPair::TryGetMetadata(const FName& Key) const
    {
        auto It = PairParams.find(Key);
        if (It != PairParams.end())
        {
            return &It->second;
        }
        
        return nullptr;
    }

    const FString& FMetaDataPair::GetMetadata(const FName& Key) const
    {
        return PairParams.at(Key);
    }
}
