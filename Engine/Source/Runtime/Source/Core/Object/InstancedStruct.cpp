#include "RuntimePCH.h"
#include "InstancedStruct.h"

#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Scripting/ScriptStruct.h"

#include "Class.h"
#include "ObjectCore.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Memory/Memory.h"

namespace Lumina::InstancedStructInternal
{
    // Same kind is not same layout: every list is one size whatever it holds, and a reload reshapes the struct inside.
    bool ShapesMatch(const FProperty* Old, const FProperty* New)
    {
        if (Old == nullptr || New == nullptr)
        {
            return Old == New;
        }

        if (Old->TypeFlags != New->TypeFlags || Old->GetElementSize() != New->GetElementSize() || Old->GetTypeName() != New->GetTypeName())
        {
            return false;
        }

        switch (Old->GetType())
        {
        case EPropertyTypeFlags::Struct:
            return static_cast<const FStructProperty*>(Old)->GetStruct() == static_cast<const FStructProperty*>(New)->GetStruct();
        case EPropertyTypeFlags::Vector:
            return ShapesMatch(static_cast<const FArrayProperty*>(Old)->GetInternalProperty(), static_cast<const FArrayProperty*>(New)->GetInternalProperty());
        case EPropertyTypeFlags::Map:
            return ShapesMatch(static_cast<const FMapProperty*>(Old)->GetKeyProperty(), static_cast<const FMapProperty*>(New)->GetKeyProperty())
                && ShapesMatch(static_cast<const FMapProperty*>(Old)->GetValueProperty(), static_cast<const FMapProperty*>(New)->GetValueProperty());
        case EPropertyTypeFlags::Optional:
            return ShapesMatch(static_cast<const FOptionalProperty*>(Old)->GetInternalProperty(), static_cast<const FOptionalProperty*>(New)->GetInternalProperty());
        default:
            return true;
        }
    }

    bool MigrateValue(const FProperty* Old, const void* OldValue, const FProperty* New, void* NewValue);

    bool MigrateArray(const FArrayProperty* Old, const void* OldValue, const FArrayProperty* New, void* NewValue)
    {
        const FProperty* OldInner = Old->GetInternalProperty();
        const FProperty* NewInner = New->GetInternalProperty();
        if (OldInner == nullptr || NewInner == nullptr || Old->GetOps() == nullptr || New->GetOps() == nullptr)
        {
            return false;
        }

        const bool bNumeric = IsPropertyNumeric(OldInner->GetType()) && IsPropertyNumeric(NewInner->GetType());
        if (!bNumeric && OldInner->GetType() != NewInner->GetType())
        {
            return false;
        }

        // Each side walks its own stride, which is what a retyped element changed.
        const SIZE_T Count = Old->GetNum(OldValue);
        New->Resize(NewValue, 0);
        New->Resize(NewValue, Count);
        for (SIZE_T Index = 0; Index < Count; ++Index)
        {
            MigrateValue(OldInner, Old->GetAt(const_cast<void*>(OldValue), Index), NewInner, New->GetAt(NewValue, Index));
        }
        return true;
    }

    // A value that cannot be carried across is left at the new layout's default rather than reinterpreted.
    bool MigrateValue(const FProperty* Old, const void* OldValue, const FProperty* New, void* NewValue)
    {
        const EPropertyTypeFlags OldType = Old->GetType();
        const EPropertyTypeFlags NewType = New->GetType();

        if (IsPropertyNumeric(OldType) && IsPropertyNumeric(NewType))
        {
            FNumericValue Number;
            return ReadNumber(OldType, OldValue, Number) && WriteNumber(NewType, NewValue, Number);
        }

        if (ShapesMatch(Old, New))
        {
            New->CopyCompleteValue(NewValue, OldValue);
            return true;
        }

        if (OldType != NewType)
        {
            return false;
        }

        switch (NewType)
        {
        case EPropertyTypeFlags::Struct:
            MigrateStructByFieldName(static_cast<const FStructProperty*>(Old)->GetStruct(), OldValue, static_cast<const FStructProperty*>(New)->GetStruct(), NewValue);
            return true;
        case EPropertyTypeFlags::Vector:
            return MigrateArray(static_cast<const FArrayProperty*>(Old), OldValue, static_cast<const FArrayProperty*>(New), NewValue);
        default:
            return false;
        }
    }
}

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
        ScriptTypePin     = Move(Other.ScriptTypePin);
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
            ScriptTypePin     = Move(Other.ScriptTypePin);
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

        if (const CScriptStruct* Script = Cast<CScriptStruct>(Type); Script != nullptr && !Script->IsValueType())
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
        TypeIdentity = StructIdentity(InStruct);
        if (!TypeIdentity.IsNone())
        {
            ScriptTypePin = InStruct;
        }
        SeededGeneration = FScriptStructRegistry::Get().GetGeneration();
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
        ScriptTypePin = nullptr;
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

        const uint64 Now = FScriptStructRegistry::Get().GetGeneration();
        if (Now == SeededGeneration)
        {
            return;
        }
        SeededGeneration = Now;

        // A native type is the same object every run, so this only ever moves for a script type.
        CStruct* Fresh = TypeIdentity.IsNone() ? ScriptStruct : ResolveStructByName(TypeIdentity);
        if (Fresh == ScriptStruct)
        {
            return;
        }

        CStruct* Stale = ScriptStruct;
        const bool bStaleInline = bInline;

        // Pinned, since a type that already died can neither be walked for fields nor destructed through.
        TStrongObjectPtr<CStruct> StalePin = ScriptTypePin.IsValid() ? ScriptTypePin : TStrongObjectPtr<CStruct>(Stale);
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
        ScriptTypePin = Fresh;
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
            const FProperty* OldProp = const_cast<CStruct*>(From)->GetProperty(NewProp->Name);
            if (OldProp == nullptr)
            {
                return;
            }

            // Each side addressed through its own property, or a reordered field reads the wrong bytes.
            InstancedStructInternal::MigrateValue(OldProp, OldProp->GetValuePtr<void>(FromMemory), NewProp, NewProp->GetValuePtr<void>(ToMemory));
        });
    }

    CStruct* ResolveInstancedStructType(CStruct* MetaBase, const FName& Key)
    {
        if (Key.IsNone())
        {
            return nullptr;
        }
        // A typed slot only takes its own hierarchy, since a reader casts the memory to that base.
        CStruct* Found = ResolveStructByName(Key);
        return IsInstancableStructType(Found) && (MetaBase == nullptr || Found->IsChildOf(MetaBase)) ? Found : nullptr;
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
                Type->SerializeProperties(Ar, Value.GetMutableMemory());
            }
            return;
        }

        CStruct* Type = Value.GetScriptStruct();
        FName StructKey = Type ? StructIdentity(Type) : NAME_None;
        Ar << StructKey;

        if (Type != nullptr)
        {
            Type->SerializeProperties(Ar, Value.GetMutableMemory());
        }
    }
}
