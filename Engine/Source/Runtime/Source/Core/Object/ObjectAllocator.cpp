#include "RuntimePCH.h"
#include "ObjectAllocator.h"

#include "ObjectBase.h"
#include "Memory/Memory.h"

namespace Lumina
{
    /** Global CObject allocator */
    RUNTIME_API FCObjectAllocator GCObjectAllocator;


    FCObjectAllocator::FCObjectAllocator()
    {
    }

    FCObjectAllocator::~FCObjectAllocator()
    {
    }

    void* FCObjectAllocator::AllocateCObject(uint32 Size, uint32 Alignment)
    {
        return Memory::Malloc(Size, Alignment);
    }

    void FCObjectAllocator::FreeCObject(CObjectBase* Ptr)
    {
        Memory::Delete(Ptr);
    }
}
