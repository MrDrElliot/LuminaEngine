#include "RuntimePCH.h"
#include "InstancedStruct.h"

#include "Class.h"
#include "Memory/Memory.h"

namespace Lumina
{
    FInstancedStruct::~FInstancedStruct()
    {
        Reset();
    }

    FInstancedStruct::FInstancedStruct(const FInstancedStruct& Other)
    {
        CopyFrom(Other);
    }

    FInstancedStruct::FInstancedStruct(FInstancedStruct&& Other) noexcept
        : ScriptStruct(Other.ScriptStruct)
        , InstanceMemory(Other.InstanceMemory)
    {
        Other.ScriptStruct = nullptr;
        Other.InstanceMemory = nullptr;
    }

    FInstancedStruct& FInstancedStruct::operator=(const FInstancedStruct& Other)
    {
        if (this != &Other)
        {
            Reset();
            CopyFrom(Other);
        }
        return *this;
    }

    FInstancedStruct& FInstancedStruct::operator=(FInstancedStruct&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset();
            ScriptStruct = Other.ScriptStruct;
            InstanceMemory = Other.InstanceMemory;
            Other.ScriptStruct = nullptr;
            Other.InstanceMemory = nullptr;
        }
        return *this;
    }

    void FInstancedStruct::InitializeAs(CStruct* InStruct)
    {
        Reset();

        if (InStruct == nullptr)
        {
            return;
        }

        ScriptStruct = InStruct;
        InstanceMemory = static_cast<uint8*>(Memory::Malloc(InStruct->GetAlignedSize(), InStruct->GetAlignment()));
        InStruct->InitializeStruct(InstanceMemory);
    }

    void FInstancedStruct::Reset()
    {
        if (InstanceMemory != nullptr)
        {
            if (ScriptStruct != nullptr)
            {
                ScriptStruct->DestroyStruct(InstanceMemory);
            }

            void* ToFree = InstanceMemory;
            Memory::Free(ToFree);
        }

        ScriptStruct = nullptr;
        InstanceMemory = nullptr;
    }

    void FInstancedStruct::CopyFrom(const FInstancedStruct& Other)
    {
        if (!Other.IsValid())
        {
            return;
        }

        InitializeAs(Other.ScriptStruct);
        ScriptStruct->CopyStruct(InstanceMemory, Other.InstanceMemory);
    }

    bool FInstancedStruct::Identical(const FInstancedStruct& Other) const
    {
        if (ScriptStruct != Other.ScriptStruct)
        {
            return false;
        }
        if (ScriptStruct == nullptr)
        {
            return true;
        }
        return ScriptStruct->CompareStruct(InstanceMemory, Other.InstanceMemory);
    }
}
