#include "RuntimePCH.h"
#include "InstancedStructProperty.h"

#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"

namespace Lumina
{
    FInstancedStructProperty::FInstancedStructProperty(const FFieldOwner& InOwner, const FInstancedStructPropertyParams* Params)
        : FProperty(InOwner, Params)
    {
        // Null for a bare FInstancedStruct, which constrains nothing and accepts any reflected struct.
        MetaStruct = Params->StructFunc != nullptr ? Params->StructFunc() : nullptr;
        bResolvedStructBase = MetaStruct != nullptr;
        SetElementSize(sizeof(FInstancedStruct));
    }

    CStruct* FInstancedStructProperty::GetMetaStruct() const
    {
        if (!bResolvedStructBase)
        {
            bResolvedStructBase = true;
            if (const FString* Base = TryGetMetadata("StructBase"))
            {
                MetaStruct = FindObject<CStruct>(FName(Base->c_str()));
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
                Type->SerializeTaggedProperties(Ar, Instance->GetMutableMemory());
            }
        }
        else
        {
            CStruct* Type = Instance->GetScriptStruct();
            FName StructKey = Type ? InstancedStructKey(Type) : NAME_None;
            Ar << StructKey;

            if (Type != nullptr)
            {
                Type->SerializeTaggedProperties(Ar, Instance->GetMutableMemory());
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
                Type->SerializeTaggedProperties(DataRecord, Instance->GetMutableMemory());
            }
        }
        else
        {
            CStruct* Type = Instance->GetScriptStruct();
            FName StructKey = Type ? InstancedStructKey(Type) : NAME_None;
            Record << StructuredArchive::TNamedValue<FName>("StructType", StructKey);

            if (Type != nullptr)
            {
                FArchiveRecord DataRecord = Record.EnterField("Data").EnterRecord();
                Type->SerializeTaggedProperties(DataRecord, Instance->GetMutableMemory());
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
        new (Value) FInstancedStruct();
    }

    void FInstancedStructProperty::DestructValue(void* Value) const
    {
        static_cast<FInstancedStruct*>(Value)->~FInstancedStruct();
    }
}
