#include "RuntimePCH.h"
#include "InstancedStructProperty.h"
#include "Memory/Construct.h"

#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/NetArchive.h"
#include "Scripting/ScriptTypeRegistry.h"

namespace Lumina
{
    FInstancedStructProperty::FInstancedStructProperty(const FPropertyDesc& InDesc)
        : FProperty(InDesc)
    {
        // Null for a bare FInstancedStruct, which constrains nothing and accepts any reflected struct.
        MetaStruct = InDesc.Type->GetStruct();
        bResolvedStructBase = MetaStruct != nullptr;
        SetElementSize(sizeof(FInstancedStruct));
    }

    CStruct* FInstancedStructProperty::GetMetaStruct() const
    {
        if (!bResolvedStructBase)
        {
            bResolvedStructBase = true;
            if (const FCStringView Base = GetMetadata("StructBase"); !Base.IsEmpty())
            {
                MetaStruct = FindObject<CStruct>(FName(*Base));
            }
        }
        return MetaStruct;
    }

    void FInstancedStructProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto* Instance = static_cast<FInstancedStruct*>(Value);

        if (Ar.IsReading())
        {
            FName StructKey;
            Ar << StructKey;

            CStruct* Type = ResolveInstancedStructType(GetMetaStruct(), StructKey);
            Instance->InitializeAs(Type);

            if (Type != nullptr)
            {
                Type->SerializeProperties(Ar, Instance->GetMutableMemory());
            }
            else if (!StructKey.IsNone())
            {
                CStruct::SkipSerializedProperties(Ar);
            }
        }
        else
        {
            CStruct* Type = Instance->GetScriptStruct();
            FName StructKey = Type ? StructIdentity(Type) : NAME_None;
            Ar << StructKey;

            if (Type != nullptr)
            {
                Type->SerializeProperties(Ar, Instance->GetMutableMemory());
            }
        }
    }

    void FInstancedStructProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        FArchiveRecord Record = Slot.EnterRecord();
        auto* Instance = static_cast<FInstancedStruct*>(Value);

        if (Slot.GetArchiver().IsReading())
        {
            FName StructKey;
            Record << StructuredArchive::TNamedValue<FName>("StructType", StructKey);

            CStruct* Type = ResolveInstancedStructType(GetMetaStruct(), StructKey);
            Instance->InitializeAs(Type);

            if (Type != nullptr)
            {
                FArchiveRecord DataRecord = Record.EnterField("Data").EnterRecord();
                Type->SerializeProperties(DataRecord, Instance->GetMutableMemory());
            }
        }
        else
        {
            CStruct* Type = Instance->GetScriptStruct();
            FName StructKey = Type ? StructIdentity(Type) : NAME_None;
            Record << StructuredArchive::TNamedValue<FName>("StructType", StructKey);

            if (Type != nullptr)
            {
                FArchiveRecord DataRecord = Record.EnterField("Data").EnterRecord();
                Type->SerializeProperties(DataRecord, Instance->GetMutableMemory());
            }
        }
    }

    bool FInstancedStructProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        const auto* A = static_cast<const FInstancedStruct*>(ValueA);
        const auto* B = static_cast<const FInstancedStruct*>(ValueB);
        return A->Identical(*B);
    }

    void FInstancedStructProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
        *static_cast<FInstancedStruct*>(Dst) = *static_cast<const FInstancedStruct*>(Src);
    }

    void FInstancedStructProperty::ConstructValue(void* Value) const
    {
        Memory::ConstructAt(static_cast<FInstancedStruct*>(Value));
    }

    void FInstancedStructProperty::NetSerialize(FNetArchive& Ar, void* Value)
    {
        auto* Instance = static_cast<FInstancedStruct*>(Value);
        if (Ar.IsWriting())
        {
            CStruct* Type = Instance->GetScriptStruct();
            FName StructKey = Type ? StructIdentity(Type) : NAME_None;
            Ar << StructKey;
            if (Type != nullptr)
            {
                Type->NetSerializeAll(Ar, Instance->GetMutableMemory());
            }
            return;
        }

        FName StructKey;
        Ar << StructKey;
        CStruct* Type = StructKey.IsNone() ? nullptr : ResolveInstancedStructType(GetMetaStruct(), StructKey);
        if (Type == nullptr)
        {
            Instance->Reset();
            if (!StructKey.IsNone())
            {
                // A net payload carries no size, so nothing after a type this peer cannot resolve can be read safely.
                Ar.SetHasError(true);
            }
            return;
        }
        if (Instance->GetScriptStruct() != Type)
        {
            Instance->InitializeAs(Type);
        }
        Type->NetSerializeAll(Ar, Instance->GetMutableMemory());
    }

    void FInstancedStructProperty::DestructValue(void* Value) const
    {
        static_cast<FInstancedStruct*>(Value)->~FInstancedStruct();
    }

    void FInstancedStructProperty::RelocateValue(void* Dest, void* Source) const
    {
        Memory::ConstructAt(static_cast<FInstancedStruct*>(Dest), std::move(*static_cast<FInstancedStruct*>(Source)));
        static_cast<FInstancedStruct*>(Source)->~FInstancedStruct();
    }
}
