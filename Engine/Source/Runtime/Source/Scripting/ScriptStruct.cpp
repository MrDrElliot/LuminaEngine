#include "RuntimePCH.h"
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
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Templates/Align.h"
#include "Core/Threading/Atomic.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptValueBridge.h"
#include "Containers/StringFormat.h"

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

        // The metadata key a kind carries on its OWN, independent of the author's [Property] arguments: both
        // are what make the editor draw a picker instead of the raw value. Shared so a field and a container
        // element of the same kind are tagged identically. The two flags are set on disjoint kinds -- bEntity
        // on the uint32 an Entity handle is, bInputAction on the string an input binding is.
        const char* KindTag(const FScriptExportType& Type)
        {
            if (Type.bEntity)
            {
                return "Entity";
            }
            if (Type.bInputAction)
            {
                return "InputAction";
            }
            return nullptr;
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

        FProperty* MakeObject(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            GPendingClass = TargetClass != nullptr ? TargetClass : CObject::StaticClass();
            const FString NameStr = Name.ToString();
            FObjectPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Object, Offset, NameStr.c_str());
            Params.ClassFunc     = +[]() -> CClass* { return GPendingClass; };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            FProperty* Property = Memory::New<FObjectProperty>(Owner, &Params);
            GPendingClass = nullptr;
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

        void FillArrayOps(FVectorOps& Ops, uint32 ElementSize, const FScriptArrayElementDesc* Desc)
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

            // Constructing the container is what wires the element description into it -- every other op above
            // reads Element, so a script array built any other way is a null deref waiting to happen. The
            // description is handed through the ops table's Context (it is the description that owns this very
            // Ops instance, so the self-reference is fine and keeps the two from ever drifting apart).
            Ops.ConstructContainer = [](void* Vector, const void* Context)
            {
                FScriptDynamicArray* Array = new (Vector) FScriptDynamicArray();
                Array->Element = static_cast<const FScriptArrayElementDesc*>(Context);
            };
            Ops.DestructContainer = [](void* Vector, const void*)
            {
                static_cast<FScriptDynamicArray*>(Vector)->~FScriptDynamicArray();
            };
            Ops.ContainerContext = Desc;
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

        void FillMapOps(FMapOps& Ops, uint32 KeySize, uint32 ValueSize, const FScriptMapElementDesc* Desc)
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

            // See FillArrayOps: constructing the container is what wires the pair description into it.
            Ops.ConstructContainer = [](void* Map, const void* Context)
            {
                FScriptDynamicMap* Instance = new (Map) FScriptDynamicMap();
                Instance->Desc = static_cast<const FScriptMapElementDesc*>(Context);
            };
            Ops.DestructContainer = [](void* Map, const void*)
            {
                static_cast<FScriptDynamicMap*>(Map)->~FScriptDynamicMap();
            };
            Ops.ContainerContext = Desc;
        }
    }

    CEnum* CScriptStruct::MintEnum(const FScriptExportType& Type)
    {
        static TAtomic<uint64> Serial{ 0 };

        // The C# enum's own name, so anything that stores an enum type by name (a blackboard key's
        // EnumType, for one) keeps a value that means something and survives a reload. The serial is
        // only a fallback for an unnamed enum, or one whose name a native enum already claims -- a
        // minted type must never shadow a native one.
        // The simple name, not the namespace-qualified one C# ships: reflected type names live in one
        // flat space (a blackboard key stores a bare name), and a qualified name would also read as a
        // namespace to anything that splits on '.'.
        FString Name = Type.EnumName.IsNone() ? FString() : FString(Type.EnumName.c_str());
        if (const size_t Dot = Name.find_last_of('.'); Dot != FString::npos)
        {
            Name.erase(0, Dot + 1);
        }

        if (Name.empty() || FindObject<CEnum>(FName(Name.c_str())) != nullptr)
        {
            Name = "ScriptEnum_";
            Name += Format("{}", Serial.fetch_add(1)).c_str();
        }

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
        MintedEnums.push_back(std::move(Enum));
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
        Name += Format("{}", Serial.fetch_add(1)).c_str();

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
        SubStructs.push_back(std::move(Sub));
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceBase(const FName& BaseName)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptInstanceBase_";
        Name += Format("{}", Serial.fetch_add(1)).c_str();

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
        SubStructs.push_back(std::move(Base));
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceCandidate(const FScriptExportInstanceCandidate& Candidate, CScriptStruct* Base)
    {
        static TAtomic<uint64> Serial{ 0 };
        FString Name = "ScriptInstance_";
        Name += Format("{}", Serial.fetch_add(1)).c_str();

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
        SubStructs.push_back(std::move(Sub));
        return Raw;
    }

    // Size, alignment and resolved struct for one non-container export kind. See the declaration in
    // ScriptStruct.h for why this exists rather than one chain per caller.
    struct CScriptStruct::FKindLayout
    {
        uint32         Size   = 0;
        uint32         Align  = 1;
        CStruct*       Native = nullptr;   // a Struct naming a native type
        CScriptStruct* Script = nullptr;   // a minted sub-struct, or an InstancedStruct's candidate base
    };

    bool CScriptStruct::ResolveKindLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
    {
        // Claims Enum too, which is a 64-bit slot whatever the C# underlying type is. That is only a
        // statement about SIZE -- MakeForKind still builds an FEnumProperty, not a bare int64.
        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            Out.Size = ScalarSize;
            Out.Align = ScalarAlign;
            return true;
        }

        switch (Type.Kind)
        {
        case EPropertyTypeFlags::String:
            Out.Size = sizeof(FString);             Out.Align = alignof(FString);             return true;
        case EPropertyTypeFlags::Name:
            Out.Size = sizeof(FName);               Out.Align = alignof(FName);               return true;
        case EPropertyTypeFlags::SoftObject:
            Out.Size = sizeof(FSoftObjectPath);     Out.Align = alignof(FSoftObjectPath);     return true;
        case EPropertyTypeFlags::Object:
            Out.Size = sizeof(TObjectPtr<CObject>); Out.Align = alignof(TObjectPtr<CObject>); return true;
        default:
            break;
        }

        if (Type.Kind == EPropertyTypeFlags::Struct && !Type.NativeName.IsNone())
        {
            Out.Native = FindObject<CStruct>(Type.NativeName);
            if (Out.Native == nullptr)
            {
                LOG_WARN("Script property '{}' dropped: native struct '{}' not found.",
                    DiagName.ToString(), Type.NativeName.ToString());
                return false;
            }
            Out.Size = Out.Native->GetAlignedSize();
            Out.Align = Out.Native->GetAlignment();
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::Struct)
        {
            Out.Script = MintSubStruct(Type);
            if (Out.Script == nullptr)
            {
                return false;
            }
            Out.Size = Out.Script->GetAlignedSize();
            Out.Align = Out.Script->GetAlignment();
            return true;
        }
        if (Type.Kind == EPropertyTypeFlags::InstancedStruct)
        {
            // Mint an empty base plus one candidate sub-CScriptStruct per selectable C# type; the value is
            // an FInstancedStruct the editor picks into. The base is what MakeForKind hands the
            // FInstancedStructProperty as its meta-base, which is why it is carried out of here.
            CScriptStruct* Base = MintInstanceBase(Type.BaseName);
            if (Base == nullptr)
            {
                return false;
            }
            for (const FScriptExportInstanceCandidate& Candidate : Type.Candidates)
            {
                MintInstanceCandidate(Candidate, Base);
            }
            Out.Script = Base;
            Out.Size = sizeof(FInstancedStruct);
            Out.Align = alignof(FInstancedStruct);
            return true;
        }

        // Everything left is a container (Vector/Map) or a kind nothing maps to. A container reaching here
        // means it was asked for as an ELEMENT, and native has no nested-container property -- refusing is
        // the enforcement, and the C# classifier refuses the same shape at the declaration.
        return false;
    }

    FProperty* CScriptStruct::MakeForKind(const FFieldOwner& Owner, const FName& FieldName, uint32 Offset,
        const FScriptExportType& Type, CStruct* Resolved)
    {
        // Before the scalar test, which also claims Enum: an enum property is an FEnumProperty wrapping an
        // int64 inner, not the bare scalar its size makes it look like.
        if (Type.Kind == EPropertyTypeFlags::Enum)
        {
            return MakeEnum(Owner, FieldName, Offset, MintEnum(Type));
        }

        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;
        if (ScalarSizeAlign(Type.Kind, ScalarSize, ScalarAlign))
        {
            return MakeScalar(Type.Kind, Owner, FieldName, Offset);
        }

        switch (Type.Kind)
        {
        case EPropertyTypeFlags::String:
            return MakeSimple<FStringProperty, EPropertyTypeFlags::String>(Owner, FieldName, Offset);
        case EPropertyTypeFlags::Name:
            return MakeSimple<FNameProperty, EPropertyTypeFlags::Name>(Owner, FieldName, Offset);
        case EPropertyTypeFlags::SoftObject:
            return MakeSoftObject(Owner, FieldName, Offset, FindObject<CClass>(Type.TargetClass));
        case EPropertyTypeFlags::Object:
            return MakeObject(Owner, FieldName, Offset, FindObject<CClass>(Type.TargetClass));
        case EPropertyTypeFlags::InstancedStruct:
            return MakeInstanced(Owner, FieldName, Offset, Resolved);
        default:
            break;
        }

        // Whatever is left is a struct, native or minted, and ResolveKindLayout already found which.
        return Resolved != nullptr ? MakeStruct(Owner, FieldName, Offset, Resolved) : nullptr;
    }

    bool CScriptStruct::ResolveElement(const FScriptExportType& Type, const FName& DiagName, FScriptArrayElementDesc& Out)
    {
        FKindLayout Layout;
        if (!ResolveKindLayout(Type, DiagName, Layout))
        {
            return false;
        }
        // No alignment: elements are packed at their size, in a buffer the map/array ops align as a whole.
        Out.Size = Layout.Size;
        Out.NativeStruct = Layout.Native;
        Out.ScriptStruct = Layout.Script;
        return true;
    }

    FProperty* CScriptStruct::CreateElement(void* ArrayOwner, const FScriptExportType& Type, FScriptArrayElementDesc& Desc)
    {
        FFieldOwner Owner = OwnerOf(static_cast<FField*>(static_cast<FProperty*>(ArrayOwner)));
        CStruct* Resolved = Desc.NativeStruct != nullptr ? Desc.NativeStruct
            : const_cast<CScriptStruct*>(Desc.ScriptStruct);

        FProperty* Property = MakeForKind(Owner, FName("Element"), 0, Type, Resolved);
        if (Property == nullptr)
        {
            return nullptr;
        }

        // An element carries the kind's own tag and nothing else: the author's [Property] metadata belongs
        // to the field that OWNS the container, and is applied there.
        if (const char* Tag = KindTag(Type))
        {
            Property->Metadata.AddValue(Tag, "");
            Property->OnMetadataFinalized();
        }
        return Property;
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
            if (!ResolveElement(*Type.ElementType, Field.Name, *Desc))
            {
                return false;
            }
            FillArrayOps(Desc->Ops, Desc->Size, Desc);
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
            if (!ResolveElement(*Type.KeyType, Field.Name, Desc->Key)
                || !ResolveElement(*Type.ValueType, Field.Name, Desc->Value))
            {
                return false;
            }
            // 16-byte pair layout keeps any key/value aligned in the 16-aligned backing buffer.
            Desc->ValueOffset = (uint32)Align(Desc->Key.Size, 16u);
            Desc->PairStride  = (uint32)Align(Desc->ValueOffset + Desc->Value.Size, 16u);
            FillMapOps(Desc->Ops, Desc->Key.Size, Desc->Value.Size, Desc);
            Out.bMap = true;
            Out.MapDesc = Desc;
            Out.Size = sizeof(FScriptDynamicMap);
            Out.Align = alignof(FScriptDynamicMap);
            return true;
        }

        FKindLayout Layout;
        if (!ResolveKindLayout(Type, Field.Name, Layout))
        {
            return false;
        }
        Out.Size = Layout.Size;
        Out.Align = Layout.Align;
        Out.Native = Layout.Native;
        Out.Sub = Layout.Script;
        return true;
    }

    FProperty* CScriptStruct::CreateProperty(CStruct* Target, const FFieldPlan& Plan, uint32 Offset)
    {
        const FScriptExportField& Field = *Plan.Field;
        const FScriptExportType& Type = *Field.Type;
        FFieldOwner Owner = OwnerOf(Target);

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
        CStruct* Resolved = Plan.Native != nullptr ? Plan.Native
            : const_cast<CScriptStruct*>(Plan.Sub);

        FProperty* Property = MakeForKind(Owner, Field.Name, Offset, Type, Resolved);
        if (Property == nullptr)
        {
            return nullptr;
        }

        // A field carries the author's [Property] metadata AND the kind's own tag -- the tag is what makes
        // the editor draw an entity or input-action picker instead of the raw value.
        ApplyMeta(Property, &Field.Meta, KindTag(Type));
        return Property;
    }

    CScriptStruct::FEmittedLayout CScriptStruct::EmitLayoutInto(CStruct* Target, uint32 BaseOffset, const FScriptExportSchema& Schema)
    {
        FEmittedLayout Result;
        Result.EndOffset = BaseOffset;
        Result.Alignment = 1;

        if (Target == nullptr)
        {
            return Result;
        }

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

        uint32 RunningSize = BaseOffset;
        uint32 MaxAlign = 1;
        for (FFieldPlan& Plan : Plans)
        {
            const uint32 Offset = Align(RunningSize, Plan.Align);
            if (Offset + Plan.Size > UINT16_MAX)
            {
                // FPropertyParams::Offset is a uint16; past that the offset silently wraps and the field
                // would alias something inside the object. Stop rather than emit a corrupt layout.
                LOG_ERROR("Script layout '{}': field '{}' lands past the 64KB property-offset limit; dropped.",
                    Target->GetName().c_str(), Plan.Field->Name.c_str());
                break;
            }

            FProperty* Property = CreateProperty(Target, Plan, Offset);
            if (Property == nullptr)
            {
                continue;
            }

            Result.Properties.push_back(Property);
            RunningSize = Offset + Plan.Size;
            MaxAlign = Math::Max(MaxAlign, Plan.Align);
        }

        Result.EndOffset = RunningSize;
        Result.Alignment = MaxAlign;
        return Result;
    }

    bool CScriptStruct::BuildFromSchema(const FScriptExportSchema& Schema, const TVector<FScriptPropertyEntry>* DefaultValues)
    {
        const FEmittedLayout Layout = EmitLayoutInto(this, 0, Schema);

        Size = (Layout.EndOffset > 0) ? Align(Layout.EndOffset, Layout.Alignment) : 0;
        Alignment = Layout.Alignment;
        Link();

        for (FProperty* Property : Layout.Properties)
        {
            if (Property->OwnsStorage())
            {
                bRequiresLifecycle = true;
                break;
            }
        }

        this->Defaults = (Size > 0) ? static_cast<uint8*>(Memory::Malloc(Size, Alignment)) : nullptr;
        if (this->Defaults != nullptr)
        {
            ConstructInto(this->Defaults);
            if (DefaultValues != nullptr && !DefaultValues->empty())
            {
                WriteValuesToStruct(this, this->Defaults, *DefaultValues);
                // Defaults worth seeding means a memzeroed buffer is not a valid instance of this layout
                // either -- InitializeStruct has to run to copy them in.
                bRequiresLifecycle = true;
            }
        }
        return true;
    }

    // Same shape as FScriptArrayElementDesc::ConstructElement, one level up: zero the buffer (which is the
    // whole story for every trivial field) and let each property that owns storage build its own value.
    void CScriptStruct::ConstructInto(void* Buffer) const
    {
        if (Buffer == nullptr)
        {
            return;
        }
        Memory::Memzero(Buffer, Size);
        for (FProperty* Property = LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
        {
            if (Property->OwnsStorage())
            {
                Property->ConstructValue(static_cast<uint8*>(Buffer) + Property->Offset);
            }
        }
    }

    void CScriptStruct::DestructIn(void* Buffer) const
    {
        if (Buffer == nullptr)
        {
            return;
        }
        for (FProperty* Property = LinkedProperty; Property != nullptr; Property = static_cast<FProperty*>(Property->Next))
        {
            if (Property->OwnsStorage())
            {
                Property->DestructValue(static_cast<uint8*>(Buffer) + Property->Offset);
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
    // Zero first, because that is already the whole story for a trivial element, then let the element's own
    // property finish the job. No kind switch: a new element type is supported by teaching its FProperty.
    void FScriptArrayElementDesc::ConstructElement(void* Element) const
    {
        Memory::Memzero(Element, Size);
        if (Inner != nullptr && Inner->OwnsStorage())
        {
            Inner->ConstructValue(Element);
        }
    }

    void FScriptArrayElementDesc::DestructElement(void* Element) const
    {
        if (Inner != nullptr && Inner->OwnsStorage())
        {
            Inner->DestructValue(Element);
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
        Name += Format("{}", Serial.fetch_add(1)).c_str();

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

        auto Inserted = Entries.insert(Lumina::Containers::MakePair(Key, std::move(Struct)));
        return Inserted.first->second.Get();
    }

    void FScriptStructRegistry::Clear()
    {
        Entries.clear();
    }

    namespace
    {
        // The CScriptStruct that owns everything a minted class's appended properties point at: element
        // descriptions, minted sub-structs, minted enums. Rooted and process-lifetime, exactly like the
        // FPropertys themselves -- a minted class is reused by name across hot reloads and its live instances
        // keep pointing at this layout, so it must outlive them. Dropped only when the class is retired.
        struct FScriptClassLayout
        {
            TObjectPtr<CScriptStruct> Record;
            uint32                    ShimSize  = 0;   ///< Target->Size before the block was appended
            uint32                    ShimAlign = 1;
            FString                   Signature;      ///< what the block was built from; see ScriptClassLayoutMatches
        };

        THashMap<CClass*, FScriptClassLayout>& GClassLayouts()
        {
            static THashMap<CClass*, FScriptClassLayout> Map;
            return Map;
        }

        // Layout records a rebuild replaced. Kept, not freed: the FPropertys the old block emitted are not
        // freed either (a stale FProperty* may still be cached anywhere from an editor row to a managed
        // token), and those properties point INTO this record's element descriptions and minted sub-structs.
        // Retiring the pair together means a stale pointer is merely stale, never dangling. Bounded by
        // "reloads that changed a script's property set", which is a developer-time action.
        TVector<TObjectPtr<CScriptStruct>>& GRetiredLayouts()
        {
            static TVector<TObjectPtr<CScriptStruct>> Records;
            return Records;
        }
    }

    uint32 AppendScriptPropertiesToClass(CClass* Target, const FScriptExportSchema& Schema)
    {
        if (Target == nullptr || !Schema.IsValid())
        {
            return 0;
        }

        // The layout record owns the side data the emitted properties point at. It is a CScriptStruct because
        // that is where the planner lives -- it is NOT this class's layout, and nothing ever instantiates it.
        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        FString RecordName = "ScriptClassLayout_";
        RecordName += Target->GetName().c_str();
        Params.Name    = FName(RecordName);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Record = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Record.Get());

        // Everything the C++ shim itself occupies. Appended properties start past it, so they can never
        // overlap a native member.
        const uint32 ShimSize  = Target->GetSize();
        const uint32 ShimAlign = Target->GetAlignment();
        const CScriptStruct::FEmittedLayout Layout = Record->EmitLayoutInto(Target, ShimSize, Schema);
        if (Layout.Properties.empty())
        {
            return 0;
        }

        Target->ScriptProperties = Layout.Properties;
        for (FProperty* Property : Layout.Properties)
        {
            // Ask the property whether its value owns storage rather than testing kinds here: a property type
            // that learns ConstructValue is picked up with no change to this loop.
            if (Property->OwnsStorage())
            {
                Target->ScriptLifecycleProperties.push_back(Property);
            }
        }

        // Grow the class so StaticAllocateObject allocates AND memzeroes the appended block. Must happen
        // before the CDO is created: CreateDefaultObject allocates from GetSize() and calls Link().
        Target->Size      = Align(Layout.EndOffset, Math::Max(Layout.Alignment, Target->GetAlignment()));
        Target->Alignment = Math::Max(Layout.Alignment, Target->GetAlignment());

        GClassLayouts()[Target] = FScriptClassLayout{ std::move(Record), ShimSize, ShimAlign,
                                                      DescribeScriptSchemaLayout(Schema) };
        return (uint32)Layout.Properties.size();
    }


    namespace
    {
        void AppendTypeSignature(const FScriptExportType* Type, FString& Out);

        void AppendFieldSignature(const FScriptExportField& Field, FString& Out)
        {
            Out += Field.Name.ToString();
            Out += ":";
            AppendTypeSignature(Field.Type.get(), Out);
            Out += ";";
        }

        // A string that changes exactly when the LAYOUT does. Metadata is deliberately absent: retitling a
        // field or widening its Min/Max must not cost a rebuild, while retyping it must.
        void AppendTypeSignature(const FScriptExportType* Type, FString& Out)
        {
            if (Type == nullptr)
            {
                Out += "?";
                return;
            }
            Out += Format("{}", (int32)Type->Kind).c_str();
            if (Type->bEntity)      { Out += "e"; }
            if (Type->bInputAction) { Out += "a"; }
            if (!Type->EnumName.IsNone())    { Out += "#"; Out += Type->EnumName.ToString(); }
            if (!Type->NativeName.IsNone())  { Out += "@"; Out += Type->NativeName.ToString(); }
            if (!Type->TargetClass.IsNone()) { Out += ">"; Out += Type->TargetClass.ToString(); }
            if (!Type->BaseName.IsNone())    { Out += "^"; Out += Type->BaseName.ToString(); }

            if (Type->ElementType) { Out += "["; AppendTypeSignature(Type->ElementType.get(), Out); Out += "]"; }
            if (Type->KeyType)     { Out += "{"; AppendTypeSignature(Type->KeyType.get(), Out); Out += "}"; }
            if (Type->ValueType)   { Out += "("; AppendTypeSignature(Type->ValueType.get(), Out); Out += ")"; }

            // A nested struct's members are part of this type's layout, so a change inside one is a change here.
            if (!Type->Fields.empty())
            {
                Out += "<";
                for (const FScriptExportField& Nested : Type->Fields)
                {
                    AppendFieldSignature(Nested, Out);
                }
                Out += ">";
            }
            for (const FScriptExportInstanceCandidate& Candidate : Type->Candidates)
            {
                Out += "~";
                Out += Candidate.TypeName;
                for (const FScriptExportField& Nested : Candidate.Fields)
                {
                    AppendFieldSignature(Nested, Out);
                }
            }
        }
    }

    void ResetSkipHotReloadProperties(CObject* Object)
    {
        if (Object == nullptr)
        {
            return;
        }
        CClass* Class = Object->GetClass();
        if (Class == nullptr)
        {
            return;
        }
        CObject* Defaults = Class->GetDefaultObjectIfCreated();
        if (Defaults == nullptr)
        {
            return;
        }
        // ScriptProperties, not the whole chain: only the C#-declared block can carry the attribute, and a
        // native member of the shim has nothing to do with a script reload.
        for (FProperty* Property : Class->ScriptProperties)
        {
            if (Property != nullptr && Property->HasMetadata("SkipHotReload"))
            {
                Property->CopyCompleteValue_InContainer(Object, Defaults);
            }
        }
    }

    FString DescribeScriptSchemaLayout(const FScriptExportSchema& Schema)
    {
        FString Signature;
        for (const FScriptExportField& Field : Schema.Fields)
        {
            AppendFieldSignature(Field, Signature);
        }
        return Signature;
    }

    bool ScriptClassLayoutMatches(CClass* Target, const FScriptExportSchema& Schema)
    {
        auto It = GClassLayouts().find(Target);
        if (It == GClassLayouts().end())
        {
            // Nothing appended yet, so "matches" only if the new schema wants nothing either.
            return !Schema.IsValid() || Schema.Fields.empty();
        }
        return It->second.Signature == DescribeScriptSchemaLayout(Schema);
    }

    bool MigrateMintedClassLayout(CClass* Target, const FScriptExportSchema& Schema)
    {
        if (Target == nullptr)
        {
            return false;
        }

        auto It = GClassLayouts().find(Target);
        if (It == GClassLayouts().end())
        {
            return false;   // never had an appended block; the caller wants AppendScriptPropertiesToClass
        }

        // Live instances are laid out at the OLD size, and StaticAllocateObject sizes an object once, at
        // creation, from Class->GetSize(). So the block cannot be rebuilt under them: the caller evacuates
        // first (serializing the owning components) and repopulates after. Refusing here mirrors what
        // TryRetireMintedClass does for the same reason, and keeps the failure loud instead of corrupting.
        int32 LiveInstances = 0;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base != nullptr && Base->GetClass() == Target
                && !Base->HasAnyFlag(OF_MarkedDestroy) && !Base->HasAnyFlag(OF_DefaultObject))
            {
                ++LiveInstances;
            }
        });
        if (LiveInstances > 0)
        {
            LOG_WARN("Scriptable: '{}' changed its property set but {} live instance(s) remain; "
                     "the layout was not rebuilt.", Target->GetName().c_str(), LiveInstances);
            return false;
        }

        const uint32 ShimSize  = It->second.ShimSize;
        const uint32 ShimAlign = It->second.ShimAlign;

        // Retire, do not free. The emitted FPropertys are not freed either, and they point into this
        // record's element descriptions; keeping the pair alive together means a pointer cached anywhere is
        // stale rather than dangling. See GRetiredLayouts.
        GRetiredLayouts().push_back(std::move(It->second.Record));
        GClassLayouts().erase(Target);

        // The CDO is the old size and carries the old property set, so it cannot survive the rebuild.
        Target->DiscardDefaultObject();

        Target->ScriptProperties.clear();
        Target->ScriptLifecycleProperties.clear();

        // Drops the appended list AND the super chain Link spliced onto its tail; the super keeps its own.
        Target->Unlink();
        Target->Size      = ShimSize;
        Target->Alignment = ShimAlign;

        if (!Schema.IsValid() || Schema.Fields.empty())
        {
            // The type dropped every [Property]. The class is back to just its shim, which is a valid
            // outcome, so re-link and rebuild the CDO at the shim size.
            Target->GetDefaultObject();
            LOG_DISPLAY("Scriptable '{}': script properties removed; the class is back to its shim layout.",
                Target->GetName().c_str());
            return true;
        }

        const uint32 Count = AppendScriptPropertiesToClass(Target, Schema);

        // Link + CDO, in that order, and only now: CreateDefaultObject allocates from GetSize() and is what
        // calls Link, so both have to come after the new block is in place.
        Target->GetDefaultObject();

        LOG_DISPLAY("Scriptable '{}': rebuilt the script property block ({} propert{}) after a schema change.",
            Target->GetName().c_str(), Count, Count == 1 ? "y" : "ies");
        return true;
    }

    void ForgetScriptClassLayout(CClass* Target)
    {
        GClassLayouts().erase(Target);
    }
}
