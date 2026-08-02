#pragma once
#include "Core/Object/ConstructObjectParams.h"

namespace Lumina
{
    class CObject;
    class CPackage;
}

namespace Lumina
{
    
    class FObjectInitializer
    {
    public:

        RUNTIME_API FObjectInitializer()
            : Package(nullptr)
            , Params(nullptr)
        {}
        
        RUNTIME_API FObjectInitializer(CPackage* InPackage, const FConstructCObjectParams& InParams);
        ~FObjectInitializer();

        static FObjectInitializer* Get();

        void Construct();

        CPackage* Package;
        FConstructCObjectParams Params;
    };
    
}
