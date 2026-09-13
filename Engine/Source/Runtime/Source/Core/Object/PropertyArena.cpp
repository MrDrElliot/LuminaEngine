#include "RuntimePCH.h"
#include "PropertyArena.h"

#include <cstddef>

#include "Core/Assertions/Assert.h"
#include "Core/Math/Math.h"
#include "Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Templates/Align.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
    FPropertyArena::~FPropertyArena()
    {
        Reset();
    }

    void FPropertyArena::Reset()
    {
        // Reverse of construction, so an inner property outlives the outer one that may still reach it.
        for (size_t Index = Owned.size(); Index > 0; --Index)
        {
            Memory::DestroyAt(Owned[Index - 1]);
        }
        Owned.clear();

        FBlock* Block = Head;
        while (Block != nullptr)
        {
            FBlock* Next = Block->Next;
            void* Raw = Block;
            Memory::Free(Raw);
            Block = Next;
        }
        Head = nullptr;
    }

    void* FPropertyArena::AllocateBytes(size_t Size, size_t Alignment)
    {
        return Allocate(Size, Alignment);
    }

    const char* FPropertyArena::CopyString(FStringView Text)
    {
        char* Copy = static_cast<char*>(Allocate(Text.size() + 1, alignof(char)));
        Memory::Memcpy(Copy, const_cast<char*>(Text.data()), Text.size());
        Copy[Text.size()] = '\0';
        return Copy;
    }

    void FPropertyArena::Reserve(size_t Bytes)
    {
        if (Bytes == 0)
        {
            return;
        }

        // Only ever grows the chain, so a reserve after properties exist cannot move what is already placed.
        if (Head != nullptr && (Head->Capacity - Head->Used) >= Bytes)
        {
            return;
        }

        LUMINA_MEMORY_SCOPE("CObject");
        const size_t BlockBytes = Align(sizeof(FBlock) + Bytes, alignof(std::max_align_t));

        FBlock* Block = static_cast<FBlock*>(Memory::Malloc(BlockBytes, alignof(std::max_align_t)));
        Block->Next     = Head;
        Block->Used     = sizeof(FBlock);
        Block->Capacity = BlockBytes;
        Head = Block;
    }

    void* FPropertyArena::Allocate(size_t Size, size_t Alignment)
    {
        if (Head != nullptr)
        {
            const size_t Aligned = Align(Head->Used, Alignment);
            if (Aligned + Size <= Head->Capacity)
            {
                Head->Used = Aligned + Size;
                return reinterpret_cast<uint8*>(Head) + Aligned;
            }
        }

        Reserve(Math::Max(Size + Alignment, kDefaultBlockBytes));

        const size_t Aligned = Align(Head->Used, Alignment);
        ASSERT(Aligned + Size <= Head->Capacity);
        Head->Used = Aligned + Size;
        return reinterpret_cast<uint8*>(Head) + Aligned;
    }

    void FPropertyOwner::Attach(FProperty* Property) const
    {
        if (Struct != nullptr)
        {
            Property->OwnerStruct = Struct;
            Struct->AddProperty(Property);
        }
        else if (Collector != nullptr)
        {
            Collector->push_back(Property);
        }
        else
        {
            Field->AddProperty(Property);
        }
    }
}
