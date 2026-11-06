#include "RuntimePCH.h"
#include "PropertyMetadata.h"

namespace Lumina
{
    void FMetaDataPair::AddValue(const FName& Key, const FString& Value)
    {
        PairParams.TryAdd(Key, Value);
    }

    void FMetaDataPair::SetValue(const FName& Key, const FString& Value)
    {
        PairParams.Add(Key, Value);
    }

    void FMetaDataPair::RemoveValue(const FName& Key)
    {
        PairParams.Remove(Key);
    }

    bool FMetaDataPair::HasMetadata(const FName& Key) const
    {
        return PairParams.Contains(Key);
    }

    const FString* FMetaDataPair::TryGetMetadata(const FName& Key) const
    {
        auto It = PairParams.Find(Key);
        if (It != nullptr)
        {
            return &(*It);
        }
        
        return nullptr;
    }

    const FString& FMetaDataPair::GetMetadata(const FName& Key) const
    {
        return PairParams.FindChecked(Key);
    }
}
