#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"

namespace Lumina
{
    class RUNTIME_API FMetaDataPair
    {
    public:

        void AddValue(const FName& Key, const FString& Value);

        bool HasMetadata(const FName& Key) const;
        
        const FString* TryGetMetadata(const FName& Key) const;
        const FString& GetMetadata(const FName& Key) const;
    

    private:

        THashMap<FName, FString> PairParams;
    };
}
