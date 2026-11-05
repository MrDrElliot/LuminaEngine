#include "RuntimePCH.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Object/Class.h"
#include "Core/Object/Class/StructTraits.h"
#include "Core/Object/Field.h"
#include "Containers/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/PropertyHeader.h"
#include "Core/Serialization/NetArchive.h"
#include "Memory/Memory.h"
#include "Log/Log.h"

IMPLEMENT_INTRINSIC_CLASS(CStruct, CField, RUNTIME_API)

namespace Lumina::StructInternal
{
    bool IsHeaderSizeValid(FArchive& Ar, const FPropertyHeader& Header, int64 DataStartPos)
    {
        const int64 Total = Ar.TotalSize();
        if (Header.Size >= 0 && (Total <= 0 || Header.Size <= Total - DataStartPos))
        {
            return true;
        }
        LOG_ERROR("Archiver corrupted: property '{}' claims {} bytes with {} left", Header.Name, Header.Size, Total - DataStartPos);
        Ar.SetHasError(true);
        return false;
    }
}

namespace Lumina
{
    bool CStruct::RequiresValueLifecycle() const
    {
        FStructOps* Ops = GetStructOps();
        return Ops != nullptr && (Ops->HasConstruct() || Ops->HasDestruct());
    }

    void CStruct::InitializeStruct(void* Dest) const
    {
        if (FStructOps* Ops = GetStructOps(); Ops && Ops->HasConstruct())
        {
            Ops->Construct(Dest);
        }
        else
        {
            Memory::Memzero(Dest, GetSize());
        }
    }

    void CStruct::DestroyStruct(void* Dest) const
    {
        if (FStructOps* Ops = GetStructOps(); Ops && Ops->HasDestruct())
        {
            Ops->Destruct(Dest);
        }
    }

    void CStruct::CopyStruct(void* Dest, const void* Src) const
    {
        if (FStructOps* Ops = GetStructOps(); Ops && Ops->HasCopy())
        {
            Ops->Copy(Dest, Src);
            return;
        }
        CopyProperties(Dest, Src);
    }

    void CStruct::RelocateStruct(void* Dest, void* Source) const
    {
        if (FStructOps* Ops = GetStructOps(); Ops && Ops->HasRelocate())
        {
            Ops->Relocate(Dest, Source);
            return;
        }
        InitializeStruct(Dest);
        CopyStruct(Dest, Source);
        DestroyStruct(Source);
    }

    bool CStruct::CompareStruct(const void* A, const void* B) const
    {
        if (FStructOps* Ops = GetStructOps(); Ops && Ops->HasEquality())
        {
            return Ops->Equals(A, B);
        }
        for (FProperty* Current : AllProperties)
        {
            if (!Current->Identical_InContainer(A, B))
            {
                return false;
            }
        }
        return true;
    }


    void CStruct::SetSuperStruct(CStruct* InSuper)
    {
        SuperStruct = InSuper;
        RebuildBaseChain();
    }

    void CStruct::RebuildBaseChain()
    {
        // Relies on a type always being registered after the type it derives from.
        const SIZE_T SuperDepth = SuperStruct != nullptr ? SuperStruct->BaseChain.Num() : 0;

        BaseChain.Reset();
        BaseChain.Reserve(IntCastChecked<int32>(SuperDepth + 1));

        if (SuperStruct != nullptr)
        {
            BaseChain.Append(SuperStruct->BaseChain);
        }
        BaseChain.Add(this);
    }

    void CStruct::RegisterDependencies()
    {
        if (SuperStruct != nullptr)
        {
            SuperStruct->RegisterDependencies();
        }
    }

    bool CStruct::IsChildOfByWalk(const CStruct* Base) const
    {
        for (const CStruct* Temp = this; Temp; Temp = Temp->GetSuperStruct())
        {
            if (Temp == Base)
            {
                return true;
            }
        }
        return false;
    }

    void CStruct::Link()
    {
        if (bLinked)
        {
            return;
        }
        bLinked = true;

        if (SuperStruct)
        { 
            SuperStruct->Link();
        }

        Flatten();
    }

    void CStruct::Flatten()
    {
        // Copied, not spliced: a struct with no own properties used to adopt the super's list head outright,
        // so a later AddProperty walked into the super and appended there instead.
        AllProperties = OwnProperties;
        if (SuperStruct != nullptr)
        {
            AllProperties.Append(SuperStruct->AllProperties);
        }

        // Derived first, so FindFunction reaches an override before the declaration it replaces.
        AllFunctions = OwnFunctions;
        if (SuperStruct != nullptr)
        {
            AllFunctions.Append(SuperStruct->AllFunctions);
        }

        TrivialCopyRuns.Reset();
        NonTrivialProperties.Reset();

        TVector<FProperty*> Trivial;
        for (FProperty* Property : AllProperties)
        {
            // An enum's own size is only known once its inner arrives, so one without it copies through its call.
            const bool bRawCopy = Property->IsTrivial() && Property->GetElementSize() != 0;
            (bRawCopy ? Trivial : NonTrivialProperties).Add(Property);
        }

        Algo::Sort(Trivial, [](const FProperty* A, const FProperty* B) { return A->Offset < B->Offset; });
        for (const FProperty* Property : Trivial)
        {
            if (!TrivialCopyRuns.IsEmpty() && TrivialCopyRuns.Last().Offset + TrivialCopyRuns.Last().Size == Property->Offset)
            {
                TrivialCopyRuns.Last().Size += (uint32)Property->GetElementSize();
            }
            else
            {
                TrivialCopyRuns.Emplace(Property->Offset, (uint32)Property->GetElementSize());
            }
        }
    }

    void CStruct::CopyProperties(void* Dest, const void* Src) const
    {
        for (const FCopyRun& Run : TrivialCopyRuns)
        {
            Memory::Memcpy(static_cast<uint8*>(Dest) + Run.Offset, static_cast<const uint8*>(Src) + Run.Offset, Run.Size);
        }
        for (const FProperty* Property : NonTrivialProperties)
        {
            Property->CopyCompleteValue_InContainer(Dest, Src);
        }
    }

    void CStruct::Unlink()
    {
        OwnProperties.Reset();
        AllProperties.Reset();
        OwnFunctions.Reset();
        AllFunctions.Reset();
        TrivialCopyRuns.Reset();
        NonTrivialProperties.Reset();
        bLinked = false;
    }

    FFixedString CStruct::MakeDisplayName() const
    {
        FFixedString DisplayName = *GetName();
        DisplayName.RemoveAt(0, 1);
        return DisplayName;
    }
    
    FProperty* CStruct::GetProperty(const FName& Name) const
    {
        for (FProperty* Current : AllProperties)
        {
            if (Current->Name == Name)
            {
                return Current;
            }
        }
        
        return nullptr;
    }

    void CStruct::AddProperty(FProperty* Property)
    {
        OwnProperties.Add(Property);

        // Link caches the flattened list, so a member arriving after it would otherwise never be seen.
        if (bLinked)
        {
            Flatten();
        }
    }

    void CStruct::AddFunction(FFunction* Function)
    {
        OwnFunctions.Add(Function);

        if (bLinked)
        {
            Flatten();
        }
    }

    FFunction* CStruct::FindFunction(const FName& Name) const
    {
        // AllFunctions is derived-first, so the nearest declaration is the first match.
        for (FFunction* Current : AllFunctions)
        {
            if (Current->GetFunctionName() == Name)
            {
                return Current;
            }
        }

        return nullptr;
    }
    
    // Entities were saved as plain 32-bit numbers before they had a kind of their own, and the packed bits match.
    static bool IsEntityRetag(EPropertyTypeFlags PropertyType, EPropertyTypeFlags HeaderType)
    {
        const auto IsPacked32 = [](EPropertyTypeFlags Type)
        {
            return Type == EPropertyTypeFlags::Int32 || Type == EPropertyTypeFlags::UInt32;
        };
        return (PropertyType == EPropertyTypeFlags::Entity && IsPacked32(HeaderType))
            || (HeaderType == EPropertyTypeFlags::Entity && IsPacked32(PropertyType));
    }

    static bool ReadNumericValue(FArchive& Ar, EPropertyTypeFlags Type, double& OutValue)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:   { int8   v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::Int16:  { int16  v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::Int32:  { int32  v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::Int64:  { int64  v; Ar << v; OutValue = (double)v; return true; }
        case EPropertyTypeFlags::UInt8:  { uint8  v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::UInt16: { uint16 v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::UInt32: { uint32 v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::UInt64: { uint64 v; Ar << v; OutValue = (double)v; return true; }
        case EPropertyTypeFlags::Float:  { float  v; Ar << v; OutValue = v; return true; }
        case EPropertyTypeFlags::Double: { Ar << OutValue; return true; }
        default:                         return false;
        }
    }

    static void SetConvertedNumericValue(const FProperty& Property, void* Data, double Value)
    {
        switch (Property.GetType())
        {
        case EPropertyTypeFlags::Int8:   Property.SetValue(Data, static_cast<int8>(Value)); break;
        case EPropertyTypeFlags::Int16:  Property.SetValue(Data, static_cast<int16>(Value)); break;
        case EPropertyTypeFlags::Int32:  Property.SetValue(Data, static_cast<int32>(Value)); break;
        case EPropertyTypeFlags::Int64:  Property.SetValue(Data, static_cast<int64>(Value)); break;
        case EPropertyTypeFlags::UInt8:  Property.SetValue(Data, static_cast<uint8>(Value)); break;
        case EPropertyTypeFlags::UInt16: Property.SetValue(Data, static_cast<uint16>(Value)); break;
        case EPropertyTypeFlags::UInt32: Property.SetValue(Data, static_cast<uint32>(Value)); break;
        case EPropertyTypeFlags::UInt64: Property.SetValue(Data, static_cast<uint64>(Value)); break;
        case EPropertyTypeFlags::Float:  Property.SetValue(Data, static_cast<float>(Value)); break;
        case EPropertyTypeFlags::Double: Property.SetValue(Data, Value); break;
        default:                         break;
        }
    }
    
    void CStruct::SerializeProperties(FArchive& Ar, void* Data) const
    {
        if (StructOps && StructOps->HasSerializer())
        {
            StructOps->Serialize(Ar, Data);
            return;
        }
        
        if (Ar.IsWriting())
        {
            uint32 NumProperties = 0;
            int64 NumPropertiesWritePos = Ar.Tell();
            Ar << NumProperties;
            
            for (FProperty* Current : AllProperties)
            {
                if (!Current->ShouldSerialize())
                {
                    continue;
                }
                // Editor-only properties are absent in shipped packages.
                if (Ar.IsCooking() && Current->IsEditorOnly())
                {
                    continue;
                }

                FPropertyHeader Header;
                Header.Type = Current->GetType();
                Header.Name = Current->GetPropertyName();

                // placeholder rewritten with the final size, so nothing in the header may be variable length
                int64 HeaderPosition = Ar.Tell();
                Ar << Header;
                int64 AfterHeaderPosition = Ar.Tell();

                void* ValuePtr = Current->GetValuePtr<void>(Data);

                const FCStringView Bundles = Current->GetMetadata("Bundle");
                const FStringView OuterBundles = Bundles.IsEmpty() ? FStringView() : Ar.SetSoftReferenceBundles(FStringView(Bundles.GetData(), Bundles.Len()));
                Current->Serialize(Ar, ValuePtr);
                if (!Bundles.IsEmpty())
                {
                    Ar.SetSoftReferenceBundles(OuterBundles);
                }

                int64 DataEndPosition = Ar.Tell();
                Header.Size = (int32)(DataEndPosition - AfterHeaderPosition);

                Ar.Seek(HeaderPosition);
                Ar << Header;
            
                Ar.Seek(DataEndPosition);

                NumProperties++;
            }

            int64 Pos = Ar.Tell();
            Ar.Seek(NumPropertiesWritePos);
            Ar << NumProperties;
            Ar.Seek(Pos);

        }
        else if (Ar.IsReading())
        {
            uint32 NumProperties = 0;
            Ar << NumProperties;

            if (NumProperties > Ar.GetMaxSerializeSize() || !Ar.CanHoldCount(NumProperties))
            {
                LOG_ERROR("Archiver corrupted: struct claims {} serialized properties (max {})", NumProperties, Ar.GetMaxSerializeSize());
                Ar.SetHasError(true);
                return;
            }

            int32 NextInOrder = 0;
            for (uint32 i = 0; i < NumProperties && !Ar.HasError(); ++i)
            {
                FPropertyHeader Header;
                Ar << Header;
        
                int64 DataStartPos = Ar.Tell();
                if (!StructInternal::IsHeaderSizeValid(Ar, Header, DataStartPos))
                {
                    return;
                }
        
                FProperty* FoundProperty = nullptr;

                // O(1) fast path assuming order is unchanged.
                if (NextInOrder < AllProperties.Num() && AllProperties[NextInOrder]->GetPropertyName() == Header.Name)
                {
                    FoundProperty = AllProperties[NextInOrder];
                    ++NextInOrder;
                }

                // O(n) fallback for reordered properties.
                if (FoundProperty == nullptr)
                {
                    for (FProperty* Search : AllProperties)
                    {
                        if (Search->GetPropertyName() == Header.Name)
                        {
                            FoundProperty = Search;
                            break;
                        }
                    }
                }

                // Match old header names against the ';'-joined Aliases metadata of renamed properties.
                if (FoundProperty == nullptr)
                {
                    for (FProperty* Search : AllProperties)
                    {
                        if (!Search->HasMetadata("Aliases"))
                        {
                            continue;
                        }
                        const FCStringView Aliases = Search->GetMetadata("Aliases");
                        for (int32 Start = 0; Start <= Aliases.Len(); )
                        {
                            int32 End = Aliases.Find(';', Start);
                            if (End == Constants::kIndexNone)
                            {
                                End = Aliases.Len();
                            }
                            // FName's two-arg ctor takes a NUMBER, not a length, so that spelling never matched an alias.
                            if (End > Start && FName(FStringView(Aliases.GetData() + Start, End - Start)) == Header.Name)
                            {
                                FoundProperty = Search;
                                break;
                            }
                            Start = End + 1;
                        }
                        if (FoundProperty != nullptr)
                        {
                            break;
                        }
                    }
                }

                if (FoundProperty)
                {
                    if (!FoundProperty->ShouldSerialize())
                    {
                        LOG_WARN("Property '{}' in struct '{}' that was previously serialized, is not marked transient. Skipping.", Header.Name.ToString(), GetName());
                        Ar.Seek(DataStartPos + Header.Size);
                        continue;
                    }
                    
                    if (FoundProperty->GetType() == Header.Type)
                    {
                        void* ValuePtr = FoundProperty->GetValuePtr<void>(Data);
                        FoundProperty->Serialize(Ar, ValuePtr);
                    }
                    else if (IsEntityRetag(FoundProperty->GetType(), Header.Type))
                    {
                        FoundProperty->Serialize(Ar, FoundProperty->GetValuePtr<void>(Data));
                    }
                    else if (IsPropertyNumeric(FoundProperty->GetType()) && IsPropertyNumeric(Header.Type))
                    {
                        double OldValue = 0.0;
                        if (!ReadNumericValue(Ar, Header.Type, OldValue))
                        {
                            LOG_ERROR("Failed to read numeric value for property '{}'", Header.Name);
                        }
                        else if (IsValueValidForType(OldValue, FoundProperty->GetType()))
                        {
                            SetConvertedNumericValue(*FoundProperty, Data, OldValue);

                            LOG_WARN("Property '{}' in struct '{}' type changed from '{}' to '{}', converted value to new type.",
                            Header.Name, GetName(), PropertyTypeToString(Header.Type), PropertyTypeToString(FoundProperty->GetType()));
                        }
                        else
                        {
                            LOG_WARN("Property '{}' in struct '{}' type changed from '{}' to '{}', but the value cannot fit in the new type.",
                            Header.Name, GetName(), PropertyTypeToString(Header.Type), PropertyTypeToString(FoundProperty->GetType()));
                        }
                    }
                    else
                    {
                        LOG_WARN("Property '{}' in struct '{}' type changed from '{}' to '{}', which do not convert; it keeps its default.",
                            Header.Name, GetName(), PropertyTypeToString(Header.Type), PropertyTypeToString(FoundProperty->GetType()));
                    }
                }
                else
                {
                    LOG_WARN("Property '{}' of type '{}' not found in struct '{}', skipping", Header.Name.ToString(), PropertyTypeToString(Header.Type), GetName());
                }
        
                Ar.Seek(DataStartPos + Header.Size);
            }
        }
    }

    void CStruct::SkipSerializedProperties(FArchive& Ar)
    {
        uint32 NumProperties = 0;
        Ar << NumProperties;
        if (NumProperties > Ar.GetMaxSerializeSize() || !Ar.CanHoldCount(NumProperties))
        {
            LOG_ERROR("Archiver corrupted: skipped struct claims {} serialized properties", NumProperties);
            Ar.SetHasError(true);
            return;
        }

        for (uint32 Index = 0; Index < NumProperties && !Ar.HasError(); ++Index)
        {
            FPropertyHeader Header;
            Ar << Header;
            const int64 DataStartPos = Ar.Tell();
            if (!StructInternal::IsHeaderSizeValid(Ar, Header, DataStartPos))
            {
                return;
            }
            Ar.Seek(DataStartPos + Header.Size);
        }
    }

    void CStruct::SerializeProperties(IStructuredArchive::FRecord& Record, void* Data, void const* Defaults) const
    {
        for (FProperty* Current : AllProperties)
        {
            if (!Current->ShouldSerialize())
            {
                continue;
            }

            void* ValuePtr = Current->GetValuePtr<void>(Data);
            const void* DefPtr = Defaults ? Current->GetValuePtr<void>(Defaults) : nullptr;
            Current->SerializeItem(Record.EnterField(Current->GetPropertyName()), ValuePtr, DefPtr);
        }
    }

    void CStruct::NetSerializeProperties(FNetArchive& Ar, void* Data) const
    {
        // Walk the same PROPERTY(Replicated) fields. NetSerialize reads or writes per the archive's mode.
        for (FProperty* Current : AllProperties)
        {
            if (!Current->ShouldSerialize() || Current->IsEditorOnly() || !Current->IsReplicated())
            {
                continue;
            }

            void* ValuePtr = Current->GetValuePtr<void>(Data);
            Current->NetSerialize(Ar, ValuePtr);
        }
    }

    void CStruct::NetSerializeAll(FNetArchive& Ar, void* Data) const
    {
        // A struct that opts into custom/tight net packing (e.g. a quantized math type) handles itself.
        if (StructOps && StructOps->HasNetSerializer())
        {
            StructOps->NetSerialize(Ar, Data);
            return;
        }

        // No Replicated filter here, since the struct is serialized as a unit.
        for (FProperty* Current : AllProperties)
        {
            if (!Current->ShouldSerialize())
            {
                continue;
            }
            Current->NetSerialize(Ar, Current->GetValuePtr<void>(Data));
        }
    }

    // Centralized so the count, the writer diff and the reader mask walk the identical set in order.
    static bool IsNetReplicatedField(const FProperty* P)
    {
        return P->ShouldSerialize() && !P->IsEditorOnly() && P->IsReplicated();
    }

    uint32 CStruct::GetNetReplicatedPropertyCount() const
    {
        uint32 Count = 0;
        for (FProperty* Current : AllProperties)
        {
            if (IsNetReplicatedField(Current))
            {
                ++Count;
            }
        }
        return Count;
    }

    void CStruct::GetNetReplicatedProperties(TVector<FProperty*>& Out) const
    {
        Out.Reset();
        for (FProperty* Current : AllProperties)
        {
            if (IsNetReplicatedField(Current))
            {
                Out.Add(Current);
            }
        }
    }

    void CStruct::NetSerializeReplicatedFlat(const FNetArchive& HookSource, void* Data,
                                             TVector<uint8>& OutBytes, TVector<uint32>& OutOffsets) const
    {
        OutBytes.Reset();
        OutOffsets.Reset();
        OutOffsets.Add(0u);

        FNetArchive Tmp(OutBytes);

        // Copied once for the whole struct so refs mint into the same maps as the live archive would.
        Tmp.EntityToNetGUID    = HookSource.EntityToNetGUID;
        Tmp.NetGUIDToEntity    = HookSource.NetGUIDToEntity;
        Tmp.ObjectToNetIndex   = HookSource.ObjectToNetIndex;
        Tmp.NetIndexToObject   = HookSource.NetIndexToObject;
        Tmp.DocumentRefToNetIndex = HookSource.DocumentRefToNetIndex;
        Tmp.NetIndexToDocumentRef = HookSource.NetIndexToDocumentRef;
        Tmp.NameToNetIndex     = HookSource.NameToNetIndex;
        Tmp.NetIndexToName     = HookSource.NetIndexToName;

        for (FProperty* Current : AllProperties)
        {
            if (!IsNetReplicatedField(Current))
            {
                continue;
            }

            Current->NetSerialize(Tmp, Current->GetValuePtr<void>(Data));

            // Each field ends whole-byte, which is exactly what NetReadReplicatedMasked skips to.
            Tmp.AlignToByte();
            OutOffsets.Add(static_cast<uint32>(Tmp.Tell()));
        }
    }

    void CStruct::NetWriteAllDelta(FNetArchive& Ar, void* Data, void* Defaults) const
    {
        if (StructOps && StructOps->HasNetSerializer())
        {
            StructOps->NetSerialize(Ar, Data);
            return;
        }

        static thread_local TVector<uint8> Mask;
        static thread_local TVector<uint8> Changed;
        static thread_local TVector<uint8> Current;
        static thread_local TVector<uint8> Default;
        Mask.Reset();
        Changed.Reset();

        uint32 Index = 0;
        for (FProperty* Property : AllProperties)
        {
            if (!Property->ShouldSerialize())
            {
                continue;
            }

            auto Flatten = [&Ar, Property](void* Source, TVector<uint8>& Out)
            {
                Out.Reset();
                FNetArchive Tmp(Out);
                Tmp.EntityToNetGUID    = Ar.EntityToNetGUID;
                Tmp.NetGUIDToEntity    = Ar.NetGUIDToEntity;
                Tmp.ObjectToNetIndex   = Ar.ObjectToNetIndex;
                Tmp.NetIndexToObject   = Ar.NetIndexToObject;
                Tmp.DocumentRefToNetIndex = Ar.DocumentRefToNetIndex;
                Tmp.NetIndexToDocumentRef = Ar.NetIndexToDocumentRef;
                Tmp.NameToNetIndex     = Ar.NameToNetIndex;
                Tmp.NetIndexToName     = Ar.NetIndexToName;
                Property->NetSerialize(Tmp, Property->GetValuePtr<void>(Source));
                Tmp.AlignToByte();
            };
            Flatten(Data, Current);
            Flatten(Defaults, Default);

            if ((Index >> 3) >= static_cast<uint32>(Mask.Num()))
            {
                Mask.Add(0);
            }
            if (Current.Num() != Default.Num() || Memory::Memcmp(Current.GetData(), Default.GetData(), Current.Num()) != 0)
            {
                Mask[Index >> 3] |= static_cast<uint8>(1u << (Index & 7));
                Changed.Append(Current);
            }
            ++Index;
        }

        Ar.AlignToByte();
        if (!Mask.IsEmpty())
        {
            Ar.Serialize(Mask.GetData(), static_cast<int64>(Mask.Num()));
        }
        if (!Changed.IsEmpty())
        {
            Ar.Serialize(Changed.GetData(), static_cast<int64>(Changed.Num()));
        }
    }

    void CStruct::NetReadAllDelta(FNetArchive& Ar, void* Data) const
    {
        if (StructOps && StructOps->HasNetSerializer())
        {
            StructOps->NetSerialize(Ar, Data);
            return;
        }

        uint32 Fields = 0;
        for (FProperty* Property : AllProperties)
        {
            Fields += Property->ShouldSerialize() ? 1u : 0u;
        }

        static thread_local TVector<uint8> Mask;
        Mask.Init(0, (Fields + 7) / 8);
        Ar.AlignToByte();
        if (!Mask.IsEmpty())
        {
            Ar.Serialize(Mask.GetData(), static_cast<int64>(Mask.Num()));
        }

        uint32 Index = 0;
        for (FProperty* Property : AllProperties)
        {
            if (!Property->ShouldSerialize())
            {
                continue;
            }
            if (Mask[Index >> 3] & (1u << (Index & 7)))
            {
                Property->NetSerialize(Ar, Property->GetValuePtr<void>(Data));
                Ar.AlignToByte();
            }
            ++Index;
        }
    }

    void CStruct::NetReadReplicatedMasked(FNetArchive& Ar, void* Data, const uint8* Mask) const
    {
        uint32 Index = 0;
        for (FProperty* Current : AllProperties)
        {
            if (!IsNetReplicatedField(Current))
            {
                continue;
            }

            const bool bPresent = (Mask[Index >> 3] & (1u << (Index & 7))) != 0;
            if (bPresent)
            {
                Current->NetSerialize(Ar, Current->GetValuePtr<void>(Data));
                // Writer emitted each changed field as a whole-byte buffer; skip to the next byte boundary.
                Ar.AlignToByte();
            }
            ++Index;
        }
    }
}
