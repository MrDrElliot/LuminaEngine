#include "RuntimePCH.h"
#include "InstancedStructProperty.h"

#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"

namespace Lumina
{
    // Stable serialized key for a stored type. Native structs use their registered name; script
    // candidates use the C# type name in ScriptTypeName metadata (their object name changes every load).
    static FName InstancedStructKey(CStruct* Type)
    {
        if (const FString* ScriptName = Type->Metadata.TryGetMetadata("ScriptTypeName"))
        {
            return FName(ScriptName->c_str());
        }
        return Type->GetName();
    }

    // Resolves a serialized key back to its struct. Native via FindObject; a script candidate by
    // matching ScriptTypeName among MetaStruct-derived structs.
    static CStruct* ResolveInstancedStructType(CStruct* MetaBase, const FName& Key)
    {
        if (Key.IsNone())
        {
            return nullptr;
        }
        if (CStruct* Found = FindObject<CStruct>(Key))
        {
            return Found;
        }
        if (MetaBase != nullptr)
        {
            for (TObjectIterator<CStruct> It; It; ++It)
            {
                CStruct* Candidate = *It;
                if (Candidate == MetaBase || !Candidate->IsChildOf(MetaBase))
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

    FInstancedStructProperty::FInstancedStructProperty(const FFieldOwner& InOwner, const FInstancedStructPropertyParams* Params)
        : FProperty(InOwner, Params)
    {
        MetaStruct = Params->StructFunc();
        SetElementSize(sizeof(FInstancedStruct));
    }

    void FInstancedStructProperty::Serialize(FArchive& Ar, void* Value)
    {
        auto* Instance = static_cast<FInstancedStruct*>(Value);

        if (Ar.IsReading())
        {
            FName StructKey;
            Ar << StructKey;

            CStruct* Type = ResolveInstancedStructType(MetaStruct, StructKey);
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

            CStruct* Type = ResolveInstancedStructType(MetaStruct, StructKey);
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
