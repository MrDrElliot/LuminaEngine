#include "pch.h"
#include "ScriptStruct.h"

#include "Core/Math/Math.h"
#include "Core/Object/ConstructObjectParams.h"
#include "Core/Object/Field.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Templates/Align.h"
#include "Core/Threading/Atomic.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptValueBridge.h"

namespace Lumina
{
    IMPLEMENT_INTRINSIC_CLASS(CScriptStruct, CStruct, RUNTIME_API)

    using namespace Scripting;

    struct CScriptStruct::FFieldPlan
    {
        const FScriptExportField*   Field = nullptr;
        EPropertyTypeFlags          Kind = EPropertyTypeFlags::None;
        uint32                      Size = 0;
        uint32                      Align = 1;
        EScriptElementKind          Life = EScriptElementKind::Trivial;
        CStruct*                    Native = nullptr;
        const CScriptStruct*        Sub = nullptr;
        bool                        bArray = false;
        FScriptArrayElementDesc*    ArrayDesc = nullptr;
        bool                        bMap = false;
        FScriptMapElementDesc*      MapDesc = nullptr;
    };

    namespace
    {
        thread_local CStruct*          GPendingStruct = nullptr;
        thread_local CEnum*            GPendingEnum   = nullptr;
        thread_local CClass*           GPendingClass  = nullptr;
        thread_local const FVectorOps* GPendingOps    = nullptr;
        thread_local const FMapOps*    GPendingMapOps = nullptr;

        FFieldOwner OwnerOf(CStruct* Owner)
        {
            FFieldOwner FieldOwner;
            FieldOwner.emplace<CStruct*>(Owner);
            return FieldOwner;
        }

        FFieldOwner OwnerOf(FField* Owner)
        {
            FFieldOwner FieldOwner;
            FieldOwner.emplace<FField*>(Owner);
            return FieldOwner;
        }

        void FillBaseParams(FPropertyParams& Params, EPropertyTypeFlags TypeFlags, uint32 Offset, const char* Name)
        {
            Params.Name          = Name;
            Params.PropertyFlags = EPropertyFlags::Editable;
            Params.TypeFlags     = TypeFlags;
            Params.SetterFunc    = nullptr;
            Params.GetterFunc    = nullptr;
            Params.Offset        = (uint16)Offset;
        }

        void ApplyMeta(FProperty* Property, const FScriptExportMeta* Meta, const char* ExtraKey)
        {
            if (Meta != nullptr)
            {
                for (const FScriptExportMetaArg& Arg : Meta->Entries)
                {
                    Property->Metadata.AddValue(Arg.Key.c_str(), Arg.Value.c_str());
                }
            }
            if (ExtraKey != nullptr)
            {
                Property->Metadata.AddValue(ExtraKey, "");
            }
            Property->OnMetadataFinalized();
        }

        template<typename TPropertyType, EPropertyTypeFlags TypeFlags>
        FProperty* MakeSimple(const FFieldOwner& Owner, const FName& Name, uint32 Offset)
        {
            const FString NameStr = Name.ToString();
            FPropertyParams Params{};
            FillBaseParams(Params, TypeFlags, Offset, NameStr.c_str());
            return Memory::New<TPropertyType>(Owner, &Params);
        }

        FProperty* MakeScalar(EPropertyTypeFlags Kind, const FFieldOwner& Owner, const FName& Name, uint32 Offset)
        {
            switch (Kind)
            {
            case EPropertyTypeFlags::Bool:   return MakeSimple<FBoolProperty,   EPropertyTypeFlags::Bool>  (Owner, Name, Offset);
            case EPropertyTypeFlags::Int8:   return MakeSimple<FInt8Property,   EPropertyTypeFlags::Int8>  (Owner, Name, Offset);
            case EPropertyTypeFlags::Int16:  return MakeSimple<FInt16Property,  EPropertyTypeFlags::Int16> (Owner, Name, Offset);
            case EPropertyTypeFlags::Int32:  return MakeSimple<FInt32Property,  EPropertyTypeFlags::Int32> (Owner, Name, Offset);
            case EPropertyTypeFlags::Int64:  return MakeSimple<FInt64Property,  EPropertyTypeFlags::Int64> (Owner, Name, Offset);
            case EPropertyTypeFlags::UInt8:  return MakeSimple<FUInt8Property,  EPropertyTypeFlags::UInt8> (Owner, Name, Offset);
            case EPropertyTypeFlags::UInt16: return MakeSimple<FUInt16Property, EPropertyTypeFlags::UInt16>(Owner, Name, Offset);
            case EPropertyTypeFlags::UInt32: return MakeSimple<FUInt32Property, EPropertyTypeFlags::UInt32>(Owner, Name, Offset);
            case EPropertyTypeFlags::UInt64: return MakeSimple<FUInt64Property, EPropertyTypeFlags::UInt64>(Owner, Name, Offset);
            case EPropertyTypeFlags::Float:  return MakeSimple<FFloatProperty,  EPropertyTypeFlags::Float> (Owner, Name, Offset);
            case EPropertyTypeFlags::Double: return MakeSimple<FDoubleProperty, EPropertyTypeFlags::Double>(Owner, Name, Offset);
            default: return nullptr;
            }
        }

        FProperty* MakeStruct(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CStruct* Resolved)
        {
            GPendingStruct = Resolved;
            const FString NameStr = Name.ToString();
            FStructPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Struct, Offset, NameStr.c_str());
            Params.StructFunc    = +[]() -> CStruct* { return GPendingStruct; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FStructProperty>(Owner, &Params);
            GPendingStruct = nullptr;
            return Property;
        }

        FProperty* MakeInstanced(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CStruct* MetaBase)
        {
            GPendingStruct = MetaBase;
            const FString NameStr = Name.ToString();
            FInstancedStructPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::InstancedStruct, Offset, NameStr.c_str());
            Params.StructFunc    = +[]() -> CStruct* { return GPendingStruct; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FInstancedStructProperty>(Owner, &Params);
            GPendingStruct = nullptr;
            return Property;
        }

        FProperty* MakeSoftObject(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            GPendingClass = TargetClass != nullptr ? TargetClass : CObject::StaticClass();
            const FString NameStr = Name.ToString();
            FSoftObjectPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::SoftObject, Offset, NameStr.c_str());
            Params.ClassFunc     = +[]() -> CClass* { return GPendingClass; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FSoftObjectProperty>(Owner, &Params);
            GPendingClass = nullptr;
            return Property;
        }

        FProperty* MakeEnum(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CEnum* Resolved)
        {
            GPendingEnum = Resolved;
            const FString NameStr = Name.ToString();
            FEnumPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Enum, Offset, NameStr.c_str());
            Params.EnumFunc      = +[]() -> CEnum* { return GPendingEnum; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FEnumProperty* Property = Memory::New<FEnumProperty>(Owner, &Params);
            GPendingEnum = nullptr;

            Property->SetElementSize(sizeof(int64));
            FPropertyParams InnerParams{};
            FillBaseParams(InnerParams, EPropertyTypeFlags::Int64, Offset, NameStr.c_str());
            (void)Memory::New<FInt64Property>(OwnerOf(static_cast<FField*>(Property)), &InnerParams);
            return Property;
        }

        FProperty* MakeArray(const FFieldOwner& Owner, const FName& Name, uint32 Offset, const FVectorOps* Ops)
        {
            GPendingOps = Ops;
            const FString NameStr = Name.ToString();
            FArrayPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Vector, Offset, NameStr.c_str());
            Params.GetOpsFn      = +[]() -> const FVectorOps* { return GPendingOps; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FArrayProperty>(Owner, &Params);
            GPendingOps = nullptr;
            return Property;
        }

        bool ScalarSizeAlign(EPropertyTypeFlags Kind, uint32& Size, uint32& Align)
        {
            switch (Kind)
            {
            case EPropertyTypeFlags::Bool:   Size = sizeof(bool);   Align = alignof(bool);   return true;
            case EPropertyTypeFlags::Int8:   Size = sizeof(int8);   Align = alignof(int8);   return true;
            case EPropertyTypeFlags::Int16:  Size = sizeof(int16);  Align = alignof(int16);  return true;
            case EPropertyTypeFlags::Int32:  Size = sizeof(int32);  Align = alignof(int32);  return true;
            case EPropertyTypeFlags::Int64:  Size = sizeof(int64);  Align = alignof(int64);  return true;
            case EPropertyTypeFlags::UInt8:  Size = sizeof(uint8);  Align = alignof(uint8);  return true;
            case EPropertyTypeFlags::UInt16: Size = sizeof(uint16); Align = alignof(uint16); return true;
            case EPropertyTypeFlags::UInt32: Size = sizeof(uint32); Align = alignof(uint32); return true;
            case EPropertyTypeFlags::UInt64: Size = sizeof(uint64); Align = alignof(uint64); return true;
            case EPropertyTypeFlags::Float:  Size = sizeof(float);  Align = alignof(float);  return true;
            case EPropertyTypeFlags::Double: Size = sizeof(double); Align = alignof(double); return true;
            case EPropertyTypeFlags::Enum:   Size = sizeof(int64);  Align = alignof(int64);  return true;
            default: return false;
            }
        }

        SIZE_T ArraySize(const void* Vector)
        {
            const FScriptDynamicArray* Array = static_cast<const FScriptDynamicArray*>(Vector);
            return (Array->Element && Array->Element->Size > 0) ? Array->Bytes.size() / Array->Element->Size : 0;
        }

        void* ArrayData(void* Vector)
        {
            return static_cast<FScriptDynamicArray*>(Vector)->Bytes.data();
        }

        void ArrayPushBack(void* Vector, const void* InElement)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            if (ES == 0) { return; }
            const SIZE_T Old = Array->Bytes.size();
            Array->Bytes.resize(Old + ES);
            uint8* Slot = Array->Bytes.data() + Old;
            Array->Element->ConstructElement(Slot);
            if (InElement != nullptr) { Array->Element->CopyElement(Slot, InElement); }
        }

        void ArrayRemoveAt(void* Vector, SIZE_T Index)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            const SIZE_T Count = ArraySize(Vector);
            if (ES == 0 || Index >= Count) { return; }
            uint8* Data = Array->Bytes.data();
            for (SIZE_T J = Index; J + 1 < Count; ++J)
            {
                Array->Element->CopyElement(Data + J * ES, Data + (J + 1) * ES);
            }
            Array->Element->DestructElement(Data + (Count - 1) * ES);
            Array->Bytes.resize((Count - 1) * ES);
        }

        void ArrayClear(void* Vector)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            const SIZE_T Count = ArraySize(Vector);
            uint8* Data = Array->Bytes.data();
            for (SIZE_T Index = 0; Index < Count && ES > 0; ++Index)
            {
                Array->Element->DestructElement(Data + Index * ES);
            }
            Array->Bytes.clear();
        }

        void ArrayResize(void* Vector, SIZE_T NewCount)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            if (ES == 0) { return; }
            const SIZE_T Count = ArraySize(Vector);
            if (NewCount > Count)
            {
                Array->Bytes.resize(NewCount * ES);
                uint8* Data = Array->Bytes.data();
                for (SIZE_T Index = Count; Index < NewCount; ++Index)
                {
                    Array->Element->ConstructElement(Data + Index * ES);
                }
            }
            else if (NewCount < Count)
            {
                uint8* Data = Array->Bytes.data();
                for (SIZE_T Index = NewCount; Index < Count; ++Index)
                {
                    Array->Element->DestructElement(Data + Index * ES);
                }
                Array->Bytes.resize(NewCount * ES);
            }
        }

        void ArrayReserve(void* Vector, SIZE_T NewCount)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            if (ES > 0) { Array->Bytes.reserve(NewCount * ES); }
        }

        void ArraySwap(void* Vector, SIZE_T A, SIZE_T B)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            const SIZE_T Count = ArraySize(Vector);
            if (ES == 0 || A >= Count || B >= Count || A == B) { return; }
            uint8* Data = Array->Bytes.data();
            void* Temp = Memory::Malloc(ES, 16);
            Array->Element->ConstructElement(Temp);
            Array->Element->CopyElement(Temp, Data + A * ES);
            Array->Element->CopyElement(Data + A * ES, Data + B * ES);
            Array->Element->CopyElement(Data + B * ES, Temp);
            Array->Element->DestructElement(Temp);
            Memory::Free(Temp);
        }

        void FillArrayOps(FVectorOps& Ops, uint32 ElementSize)
        {
            Ops.Size        = &ArraySize;
            Ops.Data        = &ArrayData;
            Ops.PushBack    = &ArrayPushBack;
            Ops.RemoveAt    = &ArrayRemoveAt;
            Ops.Clear       = &ArrayClear;
            Ops.Resize      = &ArrayResize;
            Ops.Reserve     = &ArrayReserve;
            Ops.Swap        = &ArraySwap;
            Ops.ElementSize = ElementSize;
        }

        // ---- Script dynamic map (type-erased pairs, linear find via the key property's Identical) ----

        FProperty* MakeMap(const FFieldOwner& Owner, const FName& Name, uint32 Offset, const FMapOps* Ops)
        {
            GPendingMapOps = Ops;
            const FString NameStr = Name.ToString();
            FMapPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Map, Offset, NameStr.c_str());
            Params.GetOpsFn      = +[]() -> const FMapOps* { return GPendingMapOps; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FMapProperty>(Owner, &Params);
            GPendingMapOps = nullptr;
            return Property;
        }

        SIZE_T MapSize(const void* InMap)
        {
            const FScriptDynamicMap* Map = static_cast<const FScriptDynamicMap*>(InMap);
            return (Map->Desc && Map->Desc->PairStride > 0) ? Map->Bytes.size() / Map->Desc->PairStride : 0;
        }

        void* MapFind(void* InMap, const void* KeyPtr)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc == nullptr || Desc->Key.Inner == nullptr || Desc->PairStride == 0) { return nullptr; }
            const SIZE_T Count = Map->Bytes.size() / Desc->PairStride;
            uint8* Data = Map->Bytes.data();
            for (SIZE_T i = 0; i < Count; ++i)
            {
                void* Pair = Data + i * Desc->PairStride;
                if (Desc->Key.Inner->Identical(Desc->KeyAt(Pair), KeyPtr))
                {
                    return Desc->ValueAt(Pair);
                }
            }
            return nullptr;
        }

        void* MapInsert(void* InMap, const void* KeyPtr, const void* ValuePtr)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc == nullptr || Desc->PairStride == 0) { return nullptr; }

            if (void* Existing = MapFind(InMap, KeyPtr))
            {
                if (ValuePtr != nullptr) { Desc->Value.CopyElement(Existing, ValuePtr); }
                return Existing;
            }

            const SIZE_T Old = Map->Bytes.size();
            Map->Bytes.resize(Old + Desc->PairStride);
            uint8* Pair = Map->Bytes.data() + Old;
            Desc->ConstructPair(Pair);
            if (KeyPtr != nullptr)   { Desc->Key.CopyElement(Desc->KeyAt(Pair), KeyPtr); }
            if (ValuePtr != nullptr) { Desc->Value.CopyElement(Desc->ValueAt(Pair), ValuePtr); }
            return Desc->ValueAt(Pair);
        }

        bool MapRemoveByKey(void* InMap, const void* KeyPtr)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc == nullptr || Desc->Key.Inner == nullptr || Desc->PairStride == 0) { return false; }
            const SIZE_T Count = Map->Bytes.size() / Desc->PairStride;
            uint8* Data = Map->Bytes.data();
            for (SIZE_T i = 0; i < Count; ++i)
            {
                void* Pair = Data + i * Desc->PairStride;
                if (Desc->Key.Inner->Identical(Desc->KeyAt(Pair), KeyPtr))
                {
                    // Order-independent removal: overwrite the hole with the last pair, then shrink.
                    void* Last = Data + (Count - 1) * Desc->PairStride;
                    if (Pair != Last) { Desc->CopyPair(Pair, Last); }
                    Desc->DestructPair(Last);
                    Map->Bytes.resize((Count - 1) * Desc->PairStride);
                    return true;
                }
            }
            return false;
        }

        void MapClear(void* InMap)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc != nullptr && Desc->PairStride > 0)
            {
                const SIZE_T Count = Map->Bytes.size() / Desc->PairStride;
                uint8* Data = Map->Bytes.data();
                for (SIZE_T i = 0; i < Count; ++i)
                {
                    Desc->DestructPair(Data + i * Desc->PairStride);
                }
            }
            Map->Bytes.clear();
        }

        void MapReserve(void* InMap, SIZE_T Count)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            if (Map->Desc && Map->Desc->PairStride > 0) { Map->Bytes.reserve(Count * Map->Desc->PairStride); }
        }

        void MapForEach(const void* InMap, void (*Visitor)(const void*, void*, void*), void* UserData)
        {
            const FScriptDynamicMap* Map = static_cast<const FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc == nullptr || Desc->PairStride == 0) { return; }
            const SIZE_T Count = Map->Bytes.size() / Desc->PairStride;
            uint8* Data = const_cast<uint8*>(Map->Bytes.data());
            for (SIZE_T i = 0; i < Count; ++i)
            {
                void* Pair = Data + i * Desc->PairStride;
                Visitor(Desc->KeyAt(Pair), Desc->ValueAt(Pair), UserData);
            }
        }

        void MapAt(void* InMap, SIZE_T Index, const void** OutKey, void** OutValue)
        {
            FScriptDynamicMap* Map = static_cast<FScriptDynamicMap*>(InMap);
            const FScriptMapElementDesc* Desc = Map->Desc;
            if (Desc != nullptr && Desc->PairStride > 0 && Index < Map->Bytes.size() / Desc->PairStride)
            {
                void* Pair = Map->Bytes.data() + Index * Desc->PairStride;
                if (OutKey)   { *OutKey = Desc->KeyAt(Pair); }
                if (OutValue) { *OutValue = Desc->ValueAt(Pair); }
                return;
            }
            if (OutKey)   { *OutKey = nullptr; }
            if (OutValue) { *OutValue = nullptr; }
        }

        void MapConstructKey(void* InMap, void* Dst)
        {
            const FScriptDynamicMap* Map = static_cast<const FScriptDynamicMap*>(InMap);
            if (Map->Desc != nullptr) { Map->Desc->Key.ConstructElement(Dst); }
        }

        void MapDestructKey(void* InMap, void* Dst)
        {
            const FScriptDynamicMap* Map = static_cast<const FScriptDynamicMap*>(InMap);
            if (Map->Desc != nullptr) { Map->Desc->Key.DestructElement(Dst); }
        }

        void FillMapOps(FMapOps& Ops, uint32 KeySize, uint32 ValueSize)
        {
            Ops.Size         = &MapSize;
            Ops.Insert       = &MapInsert;
            Ops.Find         = &MapFind;
            Ops.RemoveByKey  = &MapRemoveByKey;
            Ops.Clear        = &MapClear;
            Ops.Reserve      = &MapReserve;
            Ops.ForEach      = &MapForEach;
            Ops.ConstructKey = &MapConstructKey;
            Ops.DestructKey  = &MapDestructKey;
            Ops.At           = &MapAt;
            Ops.KeySize      = KeySize;
            Ops.ValueSize    = ValueSize;
        }
    }

    CEnum* CScriptStruct::MintEnum(const FScriptExportType& Type)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptEnum_";
        Name += eastl::to_string(Serial.fetch_add(1)).c_str();

        FConstructCObjectParams Params(CEnum::StaticClass());
        Params.Name    = FName(Name);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CEnum> Enum = static_cast<CEnum*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Enum.Get());
        for (const FScriptEnumEntry& Entry : Type.EnumEntries)
        {
            Enum->AddEnum(Entry.Name, (uint64)Entry.Value);
        }
        CEnum* Raw = Enum.Get();
        MintedEnums.push_back(eastl::move(Enum));
        return Raw;
    }

    // Field defaults -> the entry list BuildFromSchema writes into a struct's default buffer. Without this a
    // minted sub-struct or instanced candidate is only default-constructed, i.e. all zeroes.
    static TVector<FScriptPropertyEntry> CollectFieldDefaults(const TVector<FScriptExportField>& Fields)
    {
        TVector<FScriptPropertyEntry> Defaults;
        Defaults.reserve(Fields.size());
        for (const FScriptExportField& Field : Fields)
        {
            if (Field.Default.Kind == EScriptValueKind::Nil)
            {
                continue;
            }
            FScriptPropertyEntry& Entry = Defaults.emplace_back();
            Entry.Name  = Field.Name;
            Entry.Value = Field.Default;
        }
        return Defaults;
    }

    CScriptStruct* CScriptStruct::MintSubStruct(const FScriptExportType& Type)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptSubStruct_";
        Name += eastl::to_string(Serial.fetch_add(1)).c_str();

        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        Params.Name    = FName(Name);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Sub = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Sub.Get());

        FScriptExportSchema SubSchema;
        SubSchema.Fields = Type.Fields;
        const TVector<FScriptPropertyEntry> FieldDefaults = CollectFieldDefaults(SubSchema.Fields);
        if (!Sub->BuildFromSchema(SubSchema, &FieldDefaults))
        {
            return nullptr;
        }
        CScriptStruct* Raw = Sub.Get();
        SubStructs.push_back(eastl::move(Sub));
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceBase(const FName& BaseName)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptInstanceBase_";
        Name += eastl::to_string(Serial.fetch_add(1)).c_str();

        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        Params.Name    = FName(Name);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Base = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Base.Get());

        // A type marker only. The ScriptInstanceBase tag hides it from the picker.
        FScriptExportSchema Empty;
        Base->BuildFromSchema(Empty);
        Base->Metadata.AddValue("ScriptInstanceBase", "");
        if (!BaseName.IsNone())
        {
            Base->Metadata.AddValue("ScriptTypeName", BaseName.c_str());
        }

        CScriptStruct* Raw = Base.Get();
        SubStructs.push_back(eastl::move(Base));
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceCandidate(const FScriptExportInstanceCandidate& Candidate, CScriptStruct* Base)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptInstance_";
        Name += eastl::to_string(Serial.fetch_add(1)).c_str();

        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        Params.Name    = FName(Name);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Sub = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Sub.Get());

        FScriptExportSchema Schema;
        Schema.Fields = Candidate.Fields;
        const TVector<FScriptPropertyEntry> FieldDefaults = CollectFieldDefaults(Schema.Fields);
        if (!Sub->BuildFromSchema(Schema, &FieldDefaults))
        {
            return nullptr;
        }

        // Derive from Base so the IsChildOf picker enumerates this candidate (Base is empty, no relink
        // needed). The stable C# name drives value round-trip.
        Sub->SetSuperStruct(Base);
        Sub->Metadata.AddValue("ScriptTypeName", Candidate.TypeName.c_str());

        CScriptStruct* Raw = Sub.Get();
        SubStructs.push_back(eastl::move(Sub));
        return Raw;
    }

    bool CScriptStruct::ResolveElement(const FScriptExportType& Type, FScriptArrayElementDesc& Out)
    {
        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            Out.Size = ScalarSize;
            Out.Kind = EScriptElementKind::Trivial;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::String)
        {
            Out.Size = sizeof(FString);
            Out.Kind = EScriptElementKind::String;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::SoftObject)
        {
            Out.Size = sizeof(FSoftObjectPath);
            Out.Kind = EScriptElementKind::AssetRef;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::Struct && !Type.NativeName.IsNone())
        {
            CStruct* Native = FindObject<CStruct>(Type.NativeName);
            if (Native == nullptr)
            {
                return false;
            }
            Out.Kind = EScriptElementKind::NativeStruct;
            Out.NativeStruct = Native;
            Out.Size = Native->GetAlignedSize();
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::Struct)
        {
            CScriptStruct* Sub = MintSubStruct(Type);
            if (Sub == nullptr)
            {
                return false;
            }
            Out.Kind = EScriptElementKind::ScriptStruct;
            Out.ScriptStruct = Sub;
            Out.Size = Sub->GetAlignedSize();
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::InstancedStruct)
        {
            // A List<Instanced>/Instanced[] element. Mint the empty base plus one candidate sub-CScriptStruct
            // per selectable C# type, exactly as a single Instance field does; each element is an
            // FInstancedStruct the editor picks into. The base is stashed in ScriptStruct for CreateElement.
            CScriptStruct* Base = MintInstanceBase(Type.BaseName);
            if (Base == nullptr)
            {
                return false;
            }
            for (const FScriptExportInstanceCandidate& Candidate : Type.Candidates)
            {
                MintInstanceCandidate(Candidate, Base);
            }
            Out.Kind = EScriptElementKind::Instance;
            Out.ScriptStruct = Base;
            Out.Size = sizeof(FInstancedStruct);
            return true;
        }
        return false;
    }

    FProperty* CScriptStruct::CreateElement(void* ArrayOwner, const FScriptExportType& Type, FScriptArrayElementDesc& Desc)
    {
        FFieldOwner Owner = OwnerOf(static_cast<FField*>(static_cast<FProperty*>(ArrayOwner)));
        const FName Element("Element");

        if (Type.Kind == EPropertyTypeFlags::Enum)
        {
            return MakeEnum(Owner, Element, 0, MintEnum(Type));
        }

        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            FProperty* Property = MakeScalar(Type.Kind, Owner, Element, 0);
            if (Type.bEntity && Property != nullptr)
            {
                Property->Metadata.AddValue("Entity", "");
                Property->OnMetadataFinalized();
            }
            return Property;
        }
        if (Type.Kind == EPropertyTypeFlags::String)
        {
            return MakeSimple<FStringProperty, EPropertyTypeFlags::String>(Owner, Element, 0);
        }
        if (Type.Kind == EPropertyTypeFlags::SoftObject)
        {
            return MakeSoftObject(Owner, Element, 0, FindObject<CClass>(Type.TargetClass));
        }
        if (Type.Kind == EPropertyTypeFlags::InstancedStruct)
        {
            // ScriptStruct holds the minted base (see ResolveElement); the inner is an FInstancedStructProperty.
            return MakeInstanced(Owner, Element, 0, const_cast<CScriptStruct*>(Desc.ScriptStruct));
        }

        CStruct* Resolved = Desc.NativeStruct != nullptr ? Desc.NativeStruct
            : (Desc.ScriptStruct != nullptr ? const_cast<CScriptStruct*>(Desc.ScriptStruct) : nullptr);
        if (Resolved == nullptr)
        {
            return nullptr;
        }
        return MakeStruct(Owner, Element, 0, Resolved);
    }

    bool CScriptStruct::ResolvePlan(const FScriptExportField& Field, FFieldPlan& Out)
    {
        if (!Field.Type)
        {
            return false;
        }
        const FScriptExportType& Type = *Field.Type;
        Out.Field = &Field;
        Out.Kind = Type.Kind;

        if (Type.Kind == EPropertyTypeFlags::Vector && Type.ElementType)
        {
            FScriptArrayElementDesc* Desc = Memory::New<FScriptArrayElementDesc>();
            ElementDescs.push_back(Desc);
            if (!ResolveElement(*Type.ElementType, *Desc))
            {
                return false;
            }
            FillArrayOps(Desc->Ops, Desc->Size);
            Out.bArray = true;
            Out.ArrayDesc = Desc;
            Out.Size = sizeof(FScriptDynamicArray);
            Out.Align = alignof(FScriptDynamicArray);
            return true;
        }

        if (Type.Kind == EPropertyTypeFlags::Map && Type.KeyType && Type.ValueType)
        {
            FScriptMapElementDesc* Desc = Memory::New<FScriptMapElementDesc>();
            MapDescs.push_back(Desc);
            if (!ResolveElement(*Type.KeyType, Desc->Key) || !ResolveElement(*Type.ValueType, Desc->Value))
            {
                return false;
            }
            // 16-byte pair layout keeps any key/value aligned in the 16-aligned backing buffer.
            Desc->ValueOffset = (uint32)Align(Desc->Key.Size, 16u);
            Desc->PairStride  = (uint32)Align(Desc->ValueOffset + Desc->Value.Size, 16u);
            FillMapOps(Desc->Ops, Desc->Key.Size, Desc->Value.Size);
            Out.bMap = true;
            Out.MapDesc = Desc;
            Out.Size = sizeof(FScriptDynamicMap);
            Out.Align = alignof(FScriptDynamicMap);
            return true;
        }

        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            Out.Size = ScalarSize;
            Out.Align = ScalarAlign;
            Out.Life = EScriptElementKind::Trivial;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::String)
        {
            Out.Size = sizeof(FString);
            Out.Align = alignof(FString);
            Out.Life = EScriptElementKind::String;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::SoftObject)
        {
            Out.Size = sizeof(FSoftObjectPath);
            Out.Align = alignof(FSoftObjectPath);
            Out.Life = EScriptElementKind::AssetRef;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::Struct && !Type.NativeName.IsNone())
        {
            Out.Native = FindObject<CStruct>(Type.NativeName);
            if (Out.Native == nullptr)
            {
                LOG_WARN("Script field '{}' dropped: native struct '{}' not found.", Field.Name.ToString(), Type.NativeName.ToString());
                return false;
            }
            Out.Size = Out.Native->GetAlignedSize();
            Out.Align = Out.Native->GetAlignment();
            FStructOps* Ops = Out.Native->GetStructOps();
            Out.Life = (Ops != nullptr && (Ops->HasConstruct() || Ops->HasDestruct())) ? EScriptElementKind::NativeStruct : EScriptElementKind::Trivial;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::Struct)
        {
            CScriptStruct* Sub = MintSubStruct(Type);
            if (Sub == nullptr)
            {
                return false;
            }
            Out.Sub = Sub;
            Out.Size = Sub->GetAlignedSize();
            Out.Align = Sub->GetAlignment();
            Out.Life = !Sub->FieldInfos.empty() ? EScriptElementKind::ScriptStruct : EScriptElementKind::Trivial;
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::InstancedStruct)
        {
            // Mint an empty base plus one candidate sub-CScriptStruct per selectable C# type. The field
            // is an FInstancedStruct the editor picks into.
            CScriptStruct* Base = MintInstanceBase(Type.BaseName);
            if (Base == nullptr)
            {
                return false;
            }
            for (const FScriptExportInstanceCandidate& Candidate : Type.Candidates)
            {
                MintInstanceCandidate(Candidate, Base);
            }
            Out.Sub = Base;
            Out.Size = sizeof(FInstancedStruct);
            Out.Align = alignof(FInstancedStruct);
            Out.Life = EScriptElementKind::Instance;
            return true;
        }
        return false;
    }

    FProperty* CScriptStruct::CreateProperty(const FFieldPlan& Plan, uint32 Offset)
    {
        const FScriptExportField& Field = *Plan.Field;
        const FScriptExportType& Type = *Field.Type;
        FFieldOwner Owner = OwnerOf(static_cast<CStruct*>(this));

        if (Plan.bArray)
        {
            FProperty* Array = MakeArray(Owner, Field.Name, Offset, &Plan.ArrayDesc->Ops);
            FProperty* Inner = CreateElement(Array, *Type.ElementType, *Plan.ArrayDesc);
            Plan.ArrayDesc->Inner = Inner;
            ApplyMeta(Array, &Field.Meta, nullptr);
            return Array;
        }
        if (Plan.bMap)
        {
            FProperty* Map = MakeMap(Owner, Field.Name, Offset, &Plan.MapDesc->Ops);
            // Key inner FIRST (FMapProperty::AddProperty assigns Key on the first call, Value on the second),
            // then the Value inner. The dynamic-map ops use Key.Inner->Identical, so both must be set here.
            FProperty* KeyInner   = CreateElement(Map, *Type.KeyType, Plan.MapDesc->Key);
            FProperty* ValueInner = CreateElement(Map, *Type.ValueType, Plan.MapDesc->Value);
            Plan.MapDesc->Key.Inner   = KeyInner;
            Plan.MapDesc->Value.Inner = ValueInner;
            ApplyMeta(Map, &Field.Meta, nullptr);
            return Map;
        }
        if (Type.Kind == EPropertyTypeFlags::Enum)
        {
            FProperty* Property = MakeEnum(Owner, Field.Name, Offset, MintEnum(Type));
            ApplyMeta(Property, &Field.Meta, nullptr);
            return Property;
        }

        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            FProperty* Property = MakeScalar(Type.Kind, Owner, Field.Name, Offset);
            ApplyMeta(Property, &Field.Meta, Type.bEntity ? "Entity" : nullptr);
            return Property;
        }
        if (Type.Kind == EPropertyTypeFlags::String)
        {
            FProperty* Property = MakeSimple<FStringProperty, EPropertyTypeFlags::String>(Owner, Field.Name, Offset);
            ApplyMeta(Property, &Field.Meta, nullptr);
            return Property;
        }
        if (Type.Kind == EPropertyTypeFlags::SoftObject)
        {
            FProperty* Property = MakeSoftObject(Owner, Field.Name, Offset, FindObject<CClass>(Type.TargetClass));
            ApplyMeta(Property, &Field.Meta, nullptr);
            return Property;
        }
        if (Type.Kind == EPropertyTypeFlags::InstancedStruct)
        {
            FProperty* Property = MakeInstanced(Owner, Field.Name, Offset, const_cast<CScriptStruct*>(Plan.Sub));
            ApplyMeta(Property, &Field.Meta, nullptr);
            return Property;
        }

        CStruct* Resolved = Plan.Native != nullptr ? Plan.Native
            : (Plan.Sub != nullptr ? const_cast<CScriptStruct*>(Plan.Sub) : nullptr);
        if (Resolved == nullptr)
        {
            return nullptr;
        }
        FProperty* Property = MakeStruct(Owner, Field.Name, Offset, Resolved);
        ApplyMeta(Property, &Field.Meta, nullptr);
        return Property;
    }

    bool CScriptStruct::BuildFromSchema(const FScriptExportSchema& Schema, const TVector<FScriptPropertyEntry>* DefaultValues)
    {
        TVector<FFieldPlan> Plans;
        Plans.reserve(Schema.Fields.size());
        for (const FScriptExportField& Field : Schema.Fields)
        {
            FFieldPlan Plan;
            if (ResolvePlan(Field, Plan))
            {
                Plans.push_back(Plan);
            }
        }

        uint32 RunningSize = 0;
        uint32 MaxAlign = 1;
        for (FFieldPlan& Plan : Plans)
        {
            const uint32 Offset = Align(RunningSize, Plan.Align);
            FProperty* Property = CreateProperty(Plan, Offset);
            if (Property == nullptr)
            {
                continue;
            }
            if (Plan.bArray || Plan.bMap || Plan.Life != EScriptElementKind::Trivial)
            {
                FFieldInfo Info;
                Info.Offset = Offset;
                Info.Kind = Plan.Life;
                Info.NativeStruct = Plan.Native;
                Info.ScriptStruct = Plan.Sub;
                Info.bArray = Plan.bArray;
                Info.ArrayElement = Plan.ArrayDesc;
                Info.bMap = Plan.bMap;
                Info.MapDesc = Plan.MapDesc;
                FieldInfos.push_back(Info);
            }
            RunningSize = Offset + Plan.Size;
            MaxAlign = Math::Max(MaxAlign, Plan.Align);
        }

        Size = (RunningSize > 0) ? Align(RunningSize, MaxAlign) : 0;
        Alignment = MaxAlign;
        Link();

        this->Defaults = (Size > 0) ? static_cast<uint8*>(Memory::Malloc(Size, Alignment)) : nullptr;
        if (this->Defaults != nullptr)
        {
            ConstructInto(this->Defaults);
            if (DefaultValues != nullptr)
            {
                WriteValuesToStruct(this, this->Defaults, *DefaultValues);
            }
        }
        return true;
    }

    void CScriptStruct::ConstructInto(void* Buffer) const
    {
        if (Buffer == nullptr)
        {
            return;
        }
        Memory::Memzero(Buffer, Size);
        for (const FFieldInfo& Info : FieldInfos)
        {
            void* Field = static_cast<uint8*>(Buffer) + Info.Offset;
            if (Info.bArray)
            {
                FScriptDynamicArray* Array = new (Field) FScriptDynamicArray();
                Array->Element = Info.ArrayElement;
                continue;
            }
            if (Info.bMap)
            {
                FScriptDynamicMap* Map = new (Field) FScriptDynamicMap();
                Map->Desc = Info.MapDesc;
                continue;
            }
            switch (Info.Kind)
            {
            case EScriptElementKind::String:   new (Field) FString(); break;
            case EScriptElementKind::AssetRef: new (Field) FSoftObjectPath(); break;
            case EScriptElementKind::NativeStruct:
                if (Info.NativeStruct != nullptr)
                {
                    if (FStructOps* Ops = Info.NativeStruct->GetStructOps())
                    {
                        if (Ops->HasConstruct()) { Ops->Construct(Field); }
                    }
                }
                break;
            case EScriptElementKind::ScriptStruct:
                if (Info.ScriptStruct != nullptr) { Info.ScriptStruct->ConstructInto(Field); }
                break;
            case EScriptElementKind::Instance:
                new (Field) FInstancedStruct();
                break;
            case EScriptElementKind::Trivial:
                break;
            }
        }
    }

    void CScriptStruct::DestructIn(void* Buffer) const
    {
        if (Buffer == nullptr)
        {
            return;
        }
        for (const FFieldInfo& Info : FieldInfos)
        {
            void* Field = static_cast<uint8*>(Buffer) + Info.Offset;
            if (Info.bArray)
            {
                static_cast<FScriptDynamicArray*>(Field)->~FScriptDynamicArray();
                continue;
            }
            if (Info.bMap)
            {
                static_cast<FScriptDynamicMap*>(Field)->~FScriptDynamicMap();
                continue;
            }
            switch (Info.Kind)
            {
            case EScriptElementKind::String:   static_cast<FString*>(Field)->~FString(); break;
            case EScriptElementKind::AssetRef: static_cast<FSoftObjectPath*>(Field)->~FSoftObjectPath(); break;
            case EScriptElementKind::NativeStruct:
                if (Info.NativeStruct != nullptr)
                {
                    if (FStructOps* Ops = Info.NativeStruct->GetStructOps())
                    {
                        if (Ops->HasDestruct()) { Ops->Destruct(Field); }
                    }
                }
                break;
            case EScriptElementKind::ScriptStruct:
                if (Info.ScriptStruct != nullptr) { Info.ScriptStruct->DestructIn(Field); }
                break;
            case EScriptElementKind::Instance:
                static_cast<FInstancedStruct*>(Field)->~FInstancedStruct();
                break;
            case EScriptElementKind::Trivial:
                break;
            }
        }
    }

    void CScriptStruct::CopyInto(void* Dst, const void* Src) const
    {
        if (Dst == nullptr || Src == nullptr)
        {
            return;
        }
        FProperty* Property = LinkedProperty;
        while (Property != nullptr)
        {
            Property->CopyCompleteValue(
                static_cast<uint8*>(Dst) + Property->Offset,
                static_cast<const uint8*>(Src) + Property->Offset);
            Property = static_cast<FProperty*>(Property->Next);
        }
    }

    void CScriptStruct::ResetHotReloadFields(void* Buffer) const
    {
        if (Buffer == nullptr || Defaults == nullptr)
        {
            return;
        }
        for (FProperty* Property = LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
        {
            if (Property->HasMetadata("SkipHotReload"))
            {
                Property->CopyCompleteValue(
                    static_cast<uint8*>(Buffer) + Property->Offset,
                    static_cast<const uint8*>(Defaults) + Property->Offset);
            }
        }
    }

    void CScriptStruct::FreeRuntimeData()
    {
        if (bRuntimeFreed)
        {
            return;
        }
        bRuntimeFreed = true;

        if (Defaults != nullptr)
        {
            DestructIn(Defaults);
            Memory::Free((void*&)Defaults);
            Defaults = nullptr;
        }

        FProperty* Property = LinkedProperty;
        while (Property != nullptr)
        {
            FProperty* Next = static_cast<FProperty*>(Property->Next);
            Memory::Delete(Property);
            Property = Next;
        }
        LinkedProperty = nullptr;

        for (FScriptArrayElementDesc* Desc : ElementDescs)
        {
            Memory::Delete(Desc);
        }
        ElementDescs.clear();

        for (FScriptMapElementDesc* Desc : MapDescs)
        {
            Memory::Delete(Desc);
        }
        MapDescs.clear();

        SubStructs.clear();
        MintedEnums.clear();
    }

    void CScriptStruct::OnDestroy()
    {
        FreeRuntimeData();
        Super::OnDestroy();
    }
}

namespace Lumina::Scripting
{
    void FScriptArrayElementDesc::ConstructElement(void* Element) const
    {
        Memory::Memzero(Element, Size);
        switch (Kind)
        {
        case EScriptElementKind::String:   new (Element) FString(); break;
        case EScriptElementKind::AssetRef: new (Element) FSoftObjectPath(); break;
        case EScriptElementKind::NativeStruct:
            if (NativeStruct != nullptr)
            {
                if (FStructOps* StructOps = NativeStruct->GetStructOps())
                {
                    if (StructOps->HasConstruct()) { StructOps->Construct(Element); }
                }
            }
            break;
        case EScriptElementKind::ScriptStruct:
            if (ScriptStruct != nullptr) { ScriptStruct->ConstructInto(Element); }
            break;
        case EScriptElementKind::Instance:
            new (Element) FInstancedStruct();
            break;
        case EScriptElementKind::Trivial:
            break;
        }
    }

    void FScriptArrayElementDesc::DestructElement(void* Element) const
    {
        switch (Kind)
        {
        case EScriptElementKind::String:   static_cast<FString*>(Element)->~FString(); break;
        case EScriptElementKind::AssetRef: static_cast<FSoftObjectPath*>(Element)->~FSoftObjectPath(); break;
        case EScriptElementKind::NativeStruct:
            if (NativeStruct != nullptr)
            {
                if (FStructOps* StructOps = NativeStruct->GetStructOps())
                {
                    if (StructOps->HasDestruct()) { StructOps->Destruct(Element); }
                }
            }
            break;
        case EScriptElementKind::ScriptStruct:
            if (ScriptStruct != nullptr) { ScriptStruct->DestructIn(Element); }
            break;
        case EScriptElementKind::Instance:
            static_cast<FInstancedStruct*>(Element)->~FInstancedStruct();
            break;
        case EScriptElementKind::Trivial:
            break;
        }
    }

    void FScriptArrayElementDesc::CopyElement(void* Dst, const void* Src) const
    {
        if (Inner != nullptr) { Inner->CopyCompleteValue(Dst, Src); }
    }

    FScriptDynamicArray::~FScriptDynamicArray()
    {
        if (Element != nullptr && Element->Size > 0 && !Bytes.empty())
        {
            const SIZE_T Count = Bytes.size() / Element->Size;
            uint8* Data = Bytes.data();
            for (SIZE_T Index = 0; Index < Count; ++Index)
            {
                Element->DestructElement(Data + Index * Element->Size);
            }
        }
    }

    void FScriptMapElementDesc::ConstructPair(void* Pair) const
    {
        Key.ConstructElement(KeyAt(Pair));
        Value.ConstructElement(ValueAt(Pair));
    }

    void FScriptMapElementDesc::DestructPair(void* Pair) const
    {
        Value.DestructElement(ValueAt(Pair));
        Key.DestructElement(KeyAt(Pair));
    }

    void FScriptMapElementDesc::CopyPair(void* Dst, const void* Src) const
    {
        void* SrcPair = const_cast<void*>(Src);
        Key.CopyElement(KeyAt(Dst), KeyAt(SrcPair));
        Value.CopyElement(ValueAt(Dst), ValueAt(SrcPair));
    }

    FScriptDynamicMap::~FScriptDynamicMap()
    {
        if (Desc != nullptr && Desc->PairStride > 0 && !Bytes.empty())
        {
            const SIZE_T Count = Bytes.size() / Desc->PairStride;
            uint8* Data = Bytes.data();
            for (SIZE_T Index = 0; Index < Count; ++Index)
            {
                Desc->DestructPair(Data + Index * Desc->PairStride);
            }
        }
    }

    const CScriptStruct* FScriptStructRegistry::GetOrBuild(FStringView ScriptClass)
    {
        if (ScriptClass.empty())
        {
            return nullptr;
        }
        const FName Key(ScriptClass.data(), ScriptClass.size());
        if (auto It = Entries.find(Key); It != Entries.end())
        {
            return It->second.Get();
        }

        FScriptExportSchema Schema;
        TVector<FScriptPropertyEntry> Defaults;
        if (!DotNet::GatherScriptSchema(ScriptClass, Schema, Defaults) || !Schema.IsValid())
        {
            return nullptr;
        }

        static TAtomic<uint64> Serial{ 0 };
        FString Name = "Script_";
        Name += eastl::to_string(Serial.fetch_add(1)).c_str();

        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        Params.Name    = FName(Name);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Struct = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Struct.Get());
        if (!Struct->BuildFromSchema(Schema, &Defaults))
        {
            return nullptr;
        }

        auto Inserted = Entries.insert(eastl::make_pair(Key, eastl::move(Struct)));
        return Inserted.first->second.Get();
    }

    void FScriptStructRegistry::Clear()
    {
        Entries.clear();
    }
}
