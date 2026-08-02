#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObjectBase;
}

namespace Lumina
{
    class FCObjectAllocator
    {
    public:

        FCObjectAllocator();
        ~FCObjectAllocator();

        /** Raw allocation; does not construct. */
        RUNTIME_API void* AllocateCObject(uint32 Size, uint32 Alignment);

        RUNTIME_API void FreeCObject(CObjectBase* Ptr);

    private:
        
    };
    
    extern RUNTIME_API FCObjectAllocator GCObjectAllocator;

}
