#include "RuntimePCH.h"
#include "InstancedStruct.h"

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptDataStruct.h"

#include "Class.h"
#include "Core/Object/ObjectIterator.h"
#include "Memory/Memory.h"

namespace Lumina
{
    namespace
    {
        // Matching type flags alone would let a retyped field copy one type's bytes into another's slot.
        bool CanMigrateField(const FProperty* Old, const FProperty* New)
        {
            return Old->TypeFlags == New->TypeFlags
                && Old->GetElementSize() == New->GetElementSize()
                && Old->GetTypeName() == New->GetTypeName();
        }
    }

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
        if (Type == nullptr || Type->IsA<CClass>())
        {
            return false;
        }

        // A CDO is an instance OF a struct type, not a type, so it is never something to instance.
        return !Type->HasAnyFlag(OF_DefaultObject);
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

        // Pinned, since a type that already died can neither be walked for fields nor destructed through.
        TObjectPtr<CStruct> StalePin(Stale);
        const bool bStaleAlive = StalePin.IsValid();

        // Inline bytes are about to be overwritten, so the old value has to be moved out first.
        uint8* StaleMemory = InstanceMemory;
        alignas(kInlineAlign) uint8 StaleCopy[kInlineSize];
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

            // By name, never memcpy, since a reload can reorder or retype fields and the old blob has no layout.
            if (StaleMemory != nullptr && bStaleAlive)
            {
                MigrateStructByFieldName(Stale, StaleMemory, Fresh, InstanceMemory);
            }
        }

        if (StaleMemory != nullptr)
        {
            // Destructed while the type is still pinned, or everything the old value owned leaks.
            if (bStaleAlive)
            {
                Stale->DestroyStruct(StaleMemory);
            }

            if (!bStaleInline)
            {
                void* Raw = StaleMemory;
                Memory::Free(Raw);
            }
        }
    }

    void MigrateStructByFieldName(const CStruct* From, const void* FromMemory,
        const CStruct* To, void* ToMemory)
    {
        if (From == nullptr || FromMemory == nullptr || To == nullptr || ToMemory == nullptr)
        {
            return;
        }

        const_cast<CStruct*>(To)->ForEachProperty<FProperty>([&](FProperty* NewProp)
        {
            FProperty* OldProp = const_cast<CStruct*>(From)->GetProperty(NewProp->Name);
            if (OldProp == nullptr || !CanMigrateField(OldProp, NewProp))
            {
                return;
            }

            // Each side addressed through its own property, or a reordered field reads the wrong bytes.
            NewProp->CopyCompleteValue(NewProp->GetValuePtr<void>(ToMemory),
                                       OldProp->GetValuePtr<void>(FromMemory));
        });
    }

    FName InstancedStructKey(CStruct* Type)
    {
        if (const FString* ScriptName = Type->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            return FName(ScriptName->c_str());
        }
        return Type->GetName();
    }

    CStruct* ResolveInstancedStructType(CStruct* MetaBase, const FName& Key)
    {
        if (Key.IsNone())
        {
            return nullptr;
        }
        if (CStruct* Found = FindObject<CStruct>(Key); IsInstancableStructType(Found))
        {
            return Found;
        }
        if (MetaBase != nullptr)
        {
            for (TObjectIterator<CStruct> It; It; ++It)
            {
                CStruct* Candidate = *It;
                if (Candidate == MetaBase || !Candidate->IsChildOf(MetaBase) || !IsInstancableStructType(Candidate))
                {
                    continue;
                }
                if (const FString* Name = Candidate->Metadata.TryGetMetadata("ScriptTypeName"); Name && FName(Name->c_str()) == Key)
                {
                    return Candidate;
                }
            }
        }
        return nullptr;
    }

    void SerializeInstancedStruct(FArchive& Ar, FInstancedStruct& Value, CStruct* MetaBase)
    {
        if (Ar.IsReading())
        {
            FName StructKey;
            Ar << StructKey;

            CStruct* Type = ResolveInstancedStructType(MetaBase, StructKey);
            Value.InitializeAs(Type);

            if (Type != nullptr)
            {
                Type->SerializeTaggedProperties(Ar, Value.GetMutableMemory());
            }
            return;
        }

        CStruct* Type = Value.GetScriptStruct();
        FName StructKey = Type ? InstancedStructKey(Type) : NAME_None;
        Ar << StructKey;

        if (Type != nullptr)
        {
            Type->SerializeTaggedProperties(Ar, Value.GetMutableMemory());
        }
    }
}
