#include "RuntimePCH.h"
#include "InstancedStruct.h"

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptDataStruct.h"

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

    // Inline storage cannot be stolen, so a move relocates the value and clears the source.
    FInstancedStruct::FInstancedStruct(FInstancedStruct&& Other) noexcept
    {
        if (Other.bInline)
        {
            CopyFrom(Other);
            Other.Reset();
            return;
        }

        ScriptStruct      = Other.ScriptStruct;
        InstanceMemory    = Other.InstanceMemory;
        TypeIdentity      = Other.TypeIdentity;
        SeededGeneration  = Other.SeededGeneration;

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

            if (Other.bInline)
            {
                CopyFrom(Other);
                Other.Reset();
                return *this;
            }

            ScriptStruct      = Other.ScriptStruct;
            InstanceMemory    = Other.InstanceMemory;
            TypeIdentity      = Other.TypeIdentity;
            SeededGeneration  = Other.SeededGeneration;

            Other.ScriptStruct = nullptr;
            Other.InstanceMemory = nullptr;
        }
        return *this;
    }

    bool IsInstancableStructType(const CStruct* Type)
    {
        return Type != nullptr && !Type->IsA<CClass>();
    }

    void FInstancedStruct::InitializeAs(CStruct* InStruct)
    {
        Reset();

        // A CClass describes a CObject, which has identity and is referenced, never stored by value.
        if (!IsInstancableStructType(InStruct))
        {
            return;
        }

        ScriptStruct = InStruct;
        TypeIdentity = DataStructIdentity(InStruct);
        SeededGeneration = FScriptDataStructRegistry::Get().GetGeneration();
        AllocateFor(InStruct);
        InStruct->InitializeStruct(InstanceMemory);
    }

    void FInstancedStruct::AllocateFor(const CStruct* Type) const
    {
        const SIZE_T Size  = Type->GetAlignedSize();
        const SIZE_T Align = Type->GetAlignment();

        bInline = Size <= kInlineSize && Align <= kInlineAlign;
        InstanceMemory = bInline ? InlineStorage : static_cast<uint8*>(Memory::Malloc(Size, Align));
    }

    void FInstancedStruct::ReleaseStorage(CStruct* Type, uint8* Memory, bool bWasInline)
    {
        if (Memory == nullptr)
        {
            return;
        }

        if (Type != nullptr)
        {
            Type->DestroyStruct(Memory);
        }

        if (!bWasInline)
        {
            void* ToFree = Memory;
            Lumina::Memory::Free(ToFree);
        }
    }

    void FInstancedStruct::Reset()
    {
        ReleaseStorage(ScriptStruct, InstanceMemory, bInline);

        ScriptStruct = nullptr;
        InstanceMemory = nullptr;
        bInline = false;
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

    void FInstancedStruct::EnsureCurrentType() const
    {
        if (ScriptStruct == nullptr)
        {
            return;
        }

        const uint64 Now = FScriptDataStructRegistry::Get().GetGeneration();
        if (Now == SeededGeneration)
        {
            return;
        }
        SeededGeneration = Now;

        // A native type is the same object every run, so this only ever moves for a script type.
        CStruct* Fresh = TypeIdentity.IsNone() ? ScriptStruct : ResolveDataStructByName(TypeIdentity);
        if (Fresh == ScriptStruct)
        {
            return;
        }

        CStruct* Stale = ScriptStruct;
        const bool bStaleInline = bInline;

        // Inline bytes are about to be overwritten, so the old value has to be moved out first.
        uint8* StaleMemory = InstanceMemory;
        uint8 StaleCopy[kInlineSize];
        if (bStaleInline && StaleMemory != nullptr)
        {
            Memory::Memcpy(StaleCopy, StaleMemory, kInlineSize);
            StaleMemory = StaleCopy;
        }

        ScriptStruct = Fresh;
        InstanceMemory = nullptr;
        bInline = false;

        if (Fresh != nullptr)
        {
            AllocateFor(Fresh);
            Fresh->InitializeStruct(InstanceMemory);

            // By name, never memcpy: a reload can reorder or retype fields, so the old blob has no layout.
            if (StaleMemory != nullptr)
            {
                Fresh->ForEachProperty<FProperty>([&](FProperty* NewProp)
                {
                    FProperty* OldProp = Stale != nullptr ? Stale->GetProperty(NewProp->Name) : nullptr;
                    if (OldProp != nullptr && OldProp->TypeFlags == NewProp->TypeFlags)
                    {
                        NewProp->CopyCompleteValue_InContainer(InstanceMemory, StaleMemory);
                    }
                });
            }
        }

        // The stale type object may already be gone, so free the bytes without destructing through it.
        if (StaleMemory != nullptr && !bStaleInline)
        {
            void* Raw = StaleMemory;
            Memory::Free(Raw);
        }
    }
}
