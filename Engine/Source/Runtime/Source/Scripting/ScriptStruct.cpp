#include "RuntimePCH.h"
#include "ScriptStruct.h"
#include "Memory/Construct.h"

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
        // The property params take a stateless callback, so the value has to be ambient across the call.
        template <typename T>
        class TPendingValue
        {
        public:

            explicit TPendingValue(T InValue)
                : Previous(Current)
            {
                Current = InValue;
            }

            ~TPendingValue()
            {
                Current = Previous;
            }

            LE_NO_COPYMOVE(TPendingValue);

            static T Get() { return Current; }

        private:

            static thread_local T Current;
            T Previous;
        };

        template <typename T>
        thread_local T TPendingValue<T>::Current{};

        // The entries are the layout here, so a reordered or renumbered enum must not reuse the old one.
        FString EnumKey(const FScriptExportType& Type)
        {
            FString Key = Type.EnumName.IsNone() ? FString("?") : FString(Type.EnumName.ToString());
            Key += Format("/{}", (int32)Type.EnumUnderlying).c_str();
            for (const FScriptEnumEntry& Entry : Type.EnumEntries)
            {
                Key += ";";
                Key += Entry.Name.ToString();
                Key += "=";
                Key += Format("{}", Entry.Value).c_str();
            }
            return Key;
        }

        FFieldOwner OwnerOf(CStruct* Owner)
        {
            FFieldOwner FieldOwner;
            FieldOwner.Emplace<CStruct*>(Owner);
            return FieldOwner;
        }

        FFieldOwner OwnerOf(FField* Owner)
        {
            FFieldOwner FieldOwner;
            FieldOwner.Emplace<FField*>(Owner);
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

        struct FKindTag
        {
            const char* Key = nullptr;
            const char* Value = "";
        };

        void ApplyMeta(FProperty* Property, const FScriptExportMeta* Meta, const FKindTag& Extra)
        {
            if (Meta != nullptr)
            {
                for (const FScriptExportMetaArg& Arg : Meta->Entries)
                {
                    Property->Metadata.AddValue(Arg.Key.c_str(), Arg.Value.c_str());
                }
            }
            if (Extra.Key != nullptr)
            {
                Property->Metadata.AddValue(Extra.Key, Extra.Value);
            }
            Property->OnMetadataFinalized();
        }

        // Serialization only gates on NoSerialize, so dropping Editable saves the value and draws nothing.
        void ApplyHidden(FProperty* Property, const FScriptExportMeta& Meta)
        {
            if (Meta.Has("ScriptHidden"))
            {
                EnumRemoveFlags(Property->Flags, EPropertyFlags::Editable);
            }
        }

        // Shared so a field and a container element of the same kind are tagged identically.
        FKindTag KindTag(const FScriptExportType& Type)
        {
            if (Type.bEntity)
            {
                return FKindTag{ "Entity" };
            }
            if (Type.bInputAction)
            {
                return FKindTag{ "Picker", "InputAction" };
            }
            return FKindTag{};
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
            const TPendingValue<CStruct*> Pending(Resolved);
            const FString NameStr = Name.ToString();
            FStructPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Struct, Offset, NameStr.c_str());
            Params.StructFunc    = +[]() -> CStruct* { return TPendingValue<CStruct*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FStructProperty>(Owner, &Params);
        }

        FProperty* MakeInstanced(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CStruct* MetaBase)
        {
            const TPendingValue<CStruct*> Pending(MetaBase);
            const FString NameStr = Name.ToString();
            FInstancedStructPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::InstancedStruct, Offset, NameStr.c_str());
            Params.StructFunc    = +[]() -> CStruct* { return TPendingValue<CStruct*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FInstancedStructProperty>(Owner, &Params);
        }

        FProperty* MakeObject(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            const TPendingValue<CClass*> Pending(TargetClass != nullptr ? TargetClass : CObject::StaticClass());
            const FString NameStr = Name.ToString();
            FObjectPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Object, Offset, NameStr.c_str());
            Params.ClassFunc     = +[]() -> CClass* { return TPendingValue<CClass*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FObjectProperty>(Owner, &Params);
        }

        FProperty* MakeSoftObject(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            const TPendingValue<CClass*> Pending(TargetClass != nullptr ? TargetClass : CObject::StaticClass());
            const FString NameStr = Name.ToString();
            FSoftObjectPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::SoftObject, Offset, NameStr.c_str());
            Params.ClassFunc     = +[]() -> CClass* { return TPendingValue<CClass*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FSoftObjectProperty>(Owner, &Params);
        }

        bool ScalarSizeAlign(EPropertyTypeFlags Kind, uint32& Size, uint32& Align);

        // The slot is the C# underlying type's width, so a script enum lines up with the managed value.
        FProperty* MakeEnum(const FFieldOwner& Owner, const FName& Name, uint32 Offset, CEnum* Resolved,
            EPropertyTypeFlags Underlying)
        {
            FEnumProperty* Property = nullptr;
            {
                const TPendingValue<CEnum*> Pending(Resolved);
                const FString NameStr = Name.ToString();
                FEnumPropertyParams Params{};
                FillBaseParams(Params, EPropertyTypeFlags::Enum, Offset, NameStr.c_str());
                Params.EnumFunc      = +[]() -> CEnum* { return TPendingValue<CEnum*>::Get(); };
                Params.NumMetaData   = 0;
                Params.MetaDataArray = nullptr;
                Property = Memory::New<FEnumProperty>(Owner, &Params);
            }

            uint32 InnerSize = 0;
            uint32 InnerAlign = 0;
            if (!ScalarSizeAlign(Underlying, InnerSize, InnerAlign))
            {
                Underlying = EPropertyTypeFlags::Int64;
                InnerSize = sizeof(int64);
            }

            Property->SetElementSize(InnerSize);
            (void)MakeScalar(Underlying, OwnerOf(static_cast<FField*>(Property)), Name, Offset);
            return Property;
        }

        FProperty* MakeArray(const FFieldOwner& Owner, const FName& Name, uint32 Offset, const FVectorOps* Ops)
        {
            const TPendingValue<const FVectorOps*> Pending(Ops);
            const FString NameStr = Name.ToString();
            FArrayPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Vector, Offset, NameStr.c_str());
            Params.GetOpsFn      = +[]() -> const FVectorOps* { return TPendingValue<const FVectorOps*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FArrayProperty>(Owner, &Params);
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

            // The description owns this ops instance, so the self-reference keeps the two from drifting.
            Ops.ConstructContainer = [](void* Vector, const void* Context)
            {
                FScriptDynamicArray* Array = Memory::ConstructAt(static_cast<FScriptDynamicArray*>(Vector));
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
            const TPendingValue<const FMapOps*> Pending(Ops);
            const FString NameStr = Name.ToString();
            FMapPropertyParams Params{};
            FillBaseParams(Params, EPropertyTypeFlags::Map, Offset, NameStr.c_str());
            Params.GetOpsFn      = +[]() -> const FMapOps* { return TPendingValue<const FMapOps*>::Get(); };
            Params.NumMetaData   = 0;
            Params.MetaDataArray = nullptr;
            return Memory::New<FMapProperty>(Owner, &Params);
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
                    // Order-independent removal, overwriting the hole with the last pair then shrinking.
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

            // Constructing the container is what wires the pair description into it, as in the array ops.
            Ops.ConstructContainer = [](void* Map, const void* Context)
            {
                FScriptDynamicMap* Instance = Memory::ConstructAt(static_cast<FScriptDynamicMap*>(Map));
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

        const FString Key = EnumKey(Type);
        if (auto Cached = EnumsByKey.find(Key); Cached != EnumsByKey.end())
        {
            return Cached->second;
        }

        // The simple name, since reflected type names live in one flat space and a dot reads as a namespace.
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
        EnumsByKey[Key] = Raw;
        return Raw;
    }

    // Without this a minted sub-struct is only default-constructed, meaning all zeroes.
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

        FString Key = "sub:";
        Key += Scripting::DescribeScriptTypeSignature(Type);
        Key += Format("|{}", Type.ManagedSize).c_str();
        if (auto Cached = SubStructsByKey.find(Key); Cached != SubStructsByKey.end())
        {
            return Cached->second;
        }

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

        // The accessor reads the managed struct's size at this offset, so a differing layout reads past the end.
        if (Type.ManagedSize != 0 && Sub->GetAlignedSize() != Type.ManagedSize)
        {
            LOG_ERROR("Script struct layout mismatch: C# is {} bytes over {} field(s), the minted layout is {}. "
                      "The property is dropped rather than read out of bounds.",
                      Type.ManagedSize, Type.Fields.size(), Sub->GetAlignedSize());
            return nullptr;
        }

        CScriptStruct* Raw = Sub.Get();
        SubStructs.push_back(std::move(Sub));
        SubStructsByKey[Key] = Raw;
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceBase(const FName& BaseName)
    {
        static TAtomic<uint64> Serial{ 0 };

        const FString Key = FString("base:") + (BaseName.IsNone() ? FString("?") : FString(BaseName.ToString()));
        if (auto Cached = SubStructsByKey.find(Key); Cached != SubStructsByKey.end())
        {
            return Cached->second;
        }

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
        SubStructsByKey[Key] = Raw;
        return Raw;
    }

    CScriptStruct* CScriptStruct::MintInstanceCandidate(const FScriptExportInstanceCandidate& Candidate, CScriptStruct* Base)
    {
        static TAtomic<uint64> Serial{ 0 };

        // Scoped by the base, since the same candidate under a different base is a different reflected type.
        FString Key = "cand:";
        Key += Base != nullptr ? FString(Base->GetName().ToString()) : FString("?");
        Key += "/";
        Key += Candidate.TypeName.ToString();
        for (const FScriptExportField& Field : Candidate.Fields)
        {
            Key += ";";
            Key += Field.Name.ToString();
            Key += ":";
            Key += Field.Type ? Scripting::DescribeScriptTypeSignature(*Field.Type) : FString("?");
        }
        if (auto Cached = SubStructsByKey.find(Key); Cached != SubStructsByKey.end())
        {
            return Cached->second;
        }

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

        // Derives from the empty base so the picker enumerates this candidate with no relink needed.
        Sub->SetSuperStruct(Base);
        Sub->Metadata.AddValue("ScriptTypeName", Candidate.TypeName.c_str());

        CScriptStruct* Raw = Sub.Get();
        SubStructs.push_back(std::move(Sub));
        SubStructsByKey[Key] = Raw;
        return Raw;
    }

    // See the declaration for why this exists rather than one chain per caller.
    struct CScriptStruct::FKindLayout
    {
        uint32         Size   = 0;
        uint32         Align  = 1;
        CStruct*       Native = nullptr;   // a Struct naming a native type
        CScriptStruct* Script = nullptr;   // a minted sub-struct, or an InstancedStruct's candidate base
    };

    bool CScriptStruct::ResolveKindLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
    {
        // A statement about SIZE only, since MakeForKind still builds a real enum property.
        uint32 ScalarSize = 0;
        uint32 ScalarAlign = 0;

        // An enum occupies its underlying type's width, which is what the managed value is.
        if (Type.Kind == EPropertyTypeFlags::Enum)
        {
            if (!ScalarSizeAlign(Type.EnumUnderlying, ScalarSize, ScalarAlign))
            {
                ScalarSize = sizeof(int64);
                ScalarAlign = alignof(int64);
            }
            Out.Size = ScalarSize;
            Out.Align = ScalarAlign;
            return true;
        }

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
            // The base is what the instanced-struct property takes as its meta-base, hence carrying it out.
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

        // A container reaching here was asked for as an ELEMENT, which native has no property for.
        return false;
    }

    FProperty* CScriptStruct::MakeForKind(const FFieldOwner& Owner, const FName& FieldName, uint32 Offset,
        const FScriptExportType& Type, CStruct* Resolved)
    {
        // An enum property wraps a numeric inner rather than being the bare scalar its size suggests.
        if (Type.Kind == EPropertyTypeFlags::Enum)
        {
            return MakeEnum(Owner, FieldName, Offset, MintEnum(Type), Type.EnumUnderlying);
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
        // Elements pack at their size, in a buffer the container ops align as a whole.
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

        // The author's metadata belongs to the field that OWNS the container and is applied there.
        const FKindTag Tag = KindTag(Type);
        if (Tag.Key != nullptr)
        {
            Property->Metadata.AddValue(Tag.Key, Tag.Value);
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
            ApplyMeta(Array, &Field.Meta, FKindTag{});
            ApplyHidden(Array, Field.Meta);
            return Array;
        }
        if (Plan.bMap)
        {
            FProperty* Map = MakeMap(Owner, Field.Name, Offset, &Plan.MapDesc->Ops);
            // The dynamic map ops use the key inner's compare, so both inners must be set here.
            FProperty* KeyInner   = CreateElement(Map, *Type.KeyType, Plan.MapDesc->Key);
            FProperty* ValueInner = CreateElement(Map, *Type.ValueType, Plan.MapDesc->Value);
            Plan.MapDesc->Key.Inner   = KeyInner;
            Plan.MapDesc->Value.Inner = ValueInner;
            ApplyMeta(Map, &Field.Meta, FKindTag{});
            ApplyHidden(Map, Field.Meta);
            return Map;
        }
        CStruct* Resolved = Plan.Native != nullptr ? Plan.Native
            : const_cast<CScriptStruct*>(Plan.Sub);

        FProperty* Property = MakeForKind(Owner, Field.Name, Offset, Type, Resolved);
        if (Property == nullptr)
        {
            return nullptr;
        }

        // The tag is what makes the editor draw a picker instead of the raw value.
        ApplyMeta(Property, &Field.Meta, KindTag(Type));
        ApplyHidden(Property, Field.Meta);
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
                // The offset field is a uint16, so past that a field would silently alias something else.
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
                // Defaults worth seeding mean a zeroed buffer is not a valid instance of this layout.
                bRequiresLifecycle = true;
            }
        }
        return true;
    }

    // Zero the buffer, which is the whole story for a trivial field, then let owners build their values.
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

        SubStructsByKey.clear();
        EnumsByKey.clear();
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
    // No kind switch, so a new element type is supported by teaching its property rather than this.
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
        const FName Key(ScriptClass);
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
        // A minted class is reused by name across reloads, so this must outlive its live instances.
        struct FScriptClassLayout
        {
            TObjectPtr<CScriptStruct> Record;
            TVector<FProperty*>       Properties;     ///< the appended block, owned alongside the record
            uint32                    ShimSize  = 0;   ///< Target->Size before the block was appended
            uint32                    ShimAlign = 1;
            FString                   Signature;      ///< what the block was built from; see ScriptClassLayoutMatches
            uint64                    RetiredIn = 0;   ///< generation this was superseded in; 0 while live
        };

        THashMap<CClass*, FScriptClassLayout>& GClassLayouts()
        {
            static THashMap<CClass*, FScriptClassLayout> Map;
            return Map;
        }

        // Counts reloads, so a superseded layout can be freed once a whole one has passed.
        uint64& GScriptTypeGeneration()
        {
            static uint64 Generation = 0;
            return Generation;
        }

        // One superseded layout per class, so a pointer that outlived the rebuild reads stale rather than freed.
        THashMap<CClass*, FScriptClassLayout>& GRetiredLayouts()
        {
            static THashMap<CClass*, FScriptClassLayout> Map;
            return Map;
        }

        // The properties point into the record's element descriptions, so the pair has to die together.
        void FreeScriptClassLayout(FScriptClassLayout& Layout)
        {
            for (FProperty* Property : Layout.Properties)
            {
                // Container inners are held by the outer property, so deleting it takes them with it.
                Memory::Delete(Property);
            }
            Layout.Properties.clear();
            Layout.Record = nullptr;
        }

        // Target is only ever a key here, since a caller may already have destroyed the class.
        void DiscardRetiredLayout(CClass* Target)
        {
            if (auto It = GRetiredLayouts().find(Target); It != GRetiredLayouts().end())
            {
                FreeScriptClassLayout(It->second);
                GRetiredLayouts().erase(It);
            }
        }
    }

    uint32 AppendScriptPropertiesToClass(CClass* Target, const FScriptExportSchema& Schema)
    {
        if (Target == nullptr || !Schema.IsValid())
        {
            return 0;
        }

        // A CScriptStruct because that is where the planner lives, and nothing ever instantiates it.
        FConstructCObjectParams Params(CScriptStruct::StaticClass());
        FString RecordName = "ScriptClassLayout_";
        RecordName += Target->GetName().c_str();
        Params.Name    = FName(RecordName);
        Params.Flags   = OF_Transient;
        Params.Package = CPackage::GetTransientPackage();
        Params.Guid    = FGuid::New();

        TObjectPtr<CScriptStruct> Record = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        CObjectForceRegistration(Record.Get());

        // Appended properties start past the shim, so they can never overlap a native member.
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
            // Asking the property means a type that learns to construct is picked up with no change here.
            if (Property->OwnsStorage())
            {
                Target->ScriptLifecycleProperties.push_back(Property);
            }
        }

        // Must happen before the CDO exists, since creating it allocates from the class size.
        Target->Size      = Align(Layout.EndOffset, Math::Max(Layout.Alignment, Target->GetAlignment()));
        Target->Alignment = Math::Max(Layout.Alignment, Target->GetAlignment());

        GClassLayouts()[Target] = FScriptClassLayout{ std::move(Record), Layout.Properties, ShimSize, ShimAlign,
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

        // Metadata is deliberately absent, so retitling a field costs no rebuild but retyping it does.
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
                Out += Candidate.TypeName.ToString();
                for (const FScriptExportField& Nested : Candidate.Fields)
                {
                    AppendFieldSignature(Nested, Out);
                }
            }
        }
    }

    void AdvanceScriptTypeGeneration()
    {
        const uint64 Generation = ++GScriptTypeGeneration();

        // Outliving a whole reload means every consumer that could have cached its properties was rebuilt.
        TVector<CClass*> Expired;
        for (auto& [Target, Layout] : GRetiredLayouts())
        {
            if (Layout.RetiredIn < Generation)
            {
                Expired.push_back(Target);
            }
        }
        for (CClass* Target : Expired)
        {
            DiscardRetiredLayout(Target);
        }
    }

    FString DescribeScriptTypeSignature(const FScriptExportType& Type)
    {
        FString Out;
        AppendTypeSignature(&Type, Out);
        return Out;
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
        // Only the C#-declared block can carry the attribute, and a native shim member is unaffected.
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

        // The caller evacuates first and repopulates after, so refusing here keeps the failure loud.
        int32   LiveInstances = 0;
        FString Blockers;
        GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
        {
            if (Base == nullptr || Base->GetClass() != Target
                || Base->HasAnyFlag(OF_MarkedDestroy) || Base->HasAnyFlag(OF_DefaultObject))
            {
                return;
            }

            ++LiveInstances;

            // Named, because evacuation covers the known holders and only a stray strong reference is left.
            if (LiveInstances <= 4)
            {
                if (!Blockers.empty())
                {
                    Blockers += ", ";
                }
                Blockers += Base->GetName().c_str();
            }
        });

        if (LiveInstances > 0)
        {
            if (LiveInstances > 4)
            {
                Blockers += Format(", and {} more", LiveInstances - 4);
            }

            LOG_WARN("Scriptable: '{}' changed its property set but {} live instance(s) remain ({}); the layout "
                     "was NOT rebuilt, so this edit has not taken. Something outside the world, prefab and game "
                     "instance holders still holds a strong reference.",
                     Target->GetName().c_str(), LiveInstances, Blockers.c_str());
            return false;
        }

        const uint32 ShimSize  = It->second.ShimSize;
        const uint32 ShimAlign = It->second.ShimAlign;

        // The layout this one supersedes has already survived a full rebuild, so nothing can still reach it.
        DiscardRetiredLayout(Target);
        It->second.RetiredIn = GScriptTypeGeneration();
        GRetiredLayouts()[Target] = std::move(It->second);
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
            // The type dropped every property, which is a valid outcome, so rebuild at the shim size.
            Target->GetDefaultObject();
            LOG_DISPLAY("Scriptable '{}': script properties removed; the class is back to its shim layout.",
                Target->GetName().c_str());
            return true;
        }

        const uint32 Count = AppendScriptPropertiesToClass(Target, Schema);

        // Creating the default object allocates from the class size and calls Link, so both come after.
        Target->GetDefaultObject();

        LOG_DISPLAY("Scriptable '{}': rebuilt the script property block ({} propert{}) after a schema change.",
            Target->GetName().c_str(), Count, Count == 1 ? "y" : "ies");
        return true;
    }

    void ForgetScriptClassLayout(CClass* Target)
    {
        DiscardRetiredLayout(Target);

        // The type is gone with no live instances left, so its current block goes too.
        if (auto It = GClassLayouts().find(Target); It != GClassLayouts().end())
        {
            FreeScriptClassLayout(It->second);
            GClassLayouts().erase(It);
        }
    }
}
