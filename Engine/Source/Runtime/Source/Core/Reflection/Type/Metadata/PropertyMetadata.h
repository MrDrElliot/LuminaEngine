#pragma once

#include "Containers/HashTable.h"
#include "Containers/Name.h"

namespace Lumina
{
    class RUNTIME_API FMetaDataPair
    {
    public:

        void AddValue(const FName& Key, const FString& Value);

        // Replaces any value Key already has, where AddValue keeps the first.
        void SetValue(const FName& Key, const FString& Value);
        void RemoveValue(const FName& Key);

        bool HasMetadata(const FName& Key) const;
        
        const FString* TryGetMetadata(const FName& Key) const;
        const FString& GetMetadata(const FName& Key) const;
    

    private:

        THashMap<FName, FString> PairParams;
    };
}
