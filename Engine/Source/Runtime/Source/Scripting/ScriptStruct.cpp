#include "RuntimePCH.h"
#include "ScriptFunctionMint.h"
#include "ScriptStruct.h"
#include "Memory/Construct.h"

#include "Core/Math/Math.h"
#include "Core/Object/ConstructObjectParams.h"
#include "Core/Object/Field.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/ScriptEnum.h"
#include "Core/Reflection/Type/PropertyRegistry.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/ComponentRefProperty.h"
#include "World/Entity/Components/ComponentRef.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Templates/Align.h"
#include "Core/Threading/Atomic.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/ScriptableObject.h"
#include "Containers/StringFormat.h"

IMPLEMENT_INTRINSIC_CLASS(CScriptStruct, CStruct, RUNTIME_API)

namespace Lumina
{

    using namespace Scripting;

    struct CScriptStruct::FFieldPlan
    {
        const FScriptExportField*   Field = nullptr;
        EPropertyTypeFlags          Kind = EPropertyTypeFlags::None;
        uint32                      Size = 0;
        uint32                      Align = 1;
        CField*                     Type = nullptr;
        bool                        bArray = false;
        FScriptArrayElementDesc*    ArrayDesc = nullptr;
        bool                        bMap = false;
        FScriptMapElementDesc*      MapDesc = nullptr;
        bool                        bOptional = false;
        FScriptOptionalDesc*        OptionalDesc = nullptr;
    };

    namespace
    {
        struct FKindTag
        {
            const char* Key = nullptr;
            const char* Value = "";
        };

        EPropertyEditorFlags EditorFlagFromName(FStringView Name)
        {
            for (uint16 Bit = 0; Bit < PropertyEditorFlagBit::Count; ++Bit)
            {
                if (EqualsIgnoreCase(Name, FStringView(PropertyEditorFlagNames[Bit])))
                {
                    return EPropertyEditorFlags(1u << Bit);
                }
            }
            return EPropertyEditorFlags::None;
        }

        // Built in the arena that owns the property, since a script's keys and values are not static strings.
        void ApplyMeta(FPropertyArena& Arena, FProperty* Property, const FScriptExportMeta* Meta, const FKindTag& Extra)
        {
            const size_t MaxEntries = (Meta != nullptr ? Meta->Entries.size() : 0) + (Extra.Key != nullptr ? 1 : 0);
            if (MaxEntries == 0)
            {
                // A script property never passes through ConstructProperty, so its label is derived here or not at all.
                Property->OnMetadataFinalized(Arena);
                return;
            }

            FMetadataEntry* Entries = static_cast<FMetadataEntry*>(
                Arena.AllocateBytes(MaxEntries * sizeof(FMetadataEntry), alignof(FMetadataEntry)));

            // A script's metadata arrives after its property, so the property takes a descriptor that carries it.
            FPropertyDesc* Desc = Arena.EmplaceData<FPropertyDesc>(Property->GetDesc());

            size_t Count = 0;
            if (Meta != nullptr)
            {
                for (const FScriptExportMetaArg& Arg : Meta->Entries)
                {
                    const FString Key = Arg.Key.ToString();
                    if (const EPropertyEditorFlags Flag = EditorFlagFromName(Key); Flag != EPropertyEditorFlags::None)
                    {
                        Desc->EditorFlags |= Flag;
                        continue;
                    }
                    Entries[Count].Key   = Arena.CopyString(Key);
                    Entries[Count].Value = Arena.CopyString(Arg.Value);
                    ++Count;
                }
            }
            if (Extra.Key != nullptr)
            {
                Entries[Count].Key   = Extra.Key;
                Entries[Count].Value = Extra.Value;
                ++Count;
            }

            Desc->Metadata    = Entries;
            Desc->NumMetadata = (uint16)Count;
            Property->Desc    = Desc;
            Property->OnMetadataFinalized(Arena);
        }

        // Serialization only gates on NoSerialize, so dropping Editable saves the value and draws nothing.
        void ApplyHidden(FProperty* Property, const FScriptExportMeta& Meta)
        {
            if (Meta.Has("ScriptHidden"))
            {
                EnumRemoveFlags(Property->Flags, EPropertyFlags::Editable);
            }
        }

        // The flags the author's [Property] specifiers set, added to rather than replacing what the property's kind decided.
        void ApplyDeclaredFlags(FProperty* Property, uint32 Flags)
        {
            if (Flags != 0)
            {
                EnumAddFlags(Property->Flags, (EPropertyFlags)Flags);
            }
        }

        // Shared so a field and a container element of the same kind are tagged identically.
        FKindTag KindTag(const FScriptExportType& Type)
        {
            if (Type.bInputAction)
            {
                return FKindTag{ "Picker", "InputAction" };
            }
            return FKindTag{};
        }

        // Arena-owned, because a script property outlives the call that minted it.
        const FPropertyDesc& MakeDesc(FPropertyArena& Arena, const FName& Name, uint32 Offset, const FPropertyType* Type)
        {
            FPropertyDesc* Desc = Arena.EmplaceData<FPropertyDesc>();
            Desc->Name   = Arena.CopyString(Name.ToString());
            Desc->Offset = Offset;
            Desc->Flags  = EPropertyFlags::Editable;
            Desc->Type   = Type;
            return *Desc;
        }

        // A script type already holds what it names, so it resolves to that rather than linking to it.
        template<typename TProperty>
        FProperty* MakeResolved(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, EPropertyTypeFlags Kind, const void* Resolved)
        {
            FPropertyType* Type = Owner.Arena->EmplaceData<FPropertyType>();
            Type->Kind     = Kind;
            Type->Resolved = Resolved;
            return Owner.Build<TProperty>(MakeDesc(*Owner.Arena, Name, Offset, Type));
        }

        // Every kind that names nothing, which a schema can build from the kind alone.
        FProperty* MakeBasic(EPropertyTypeFlags Kind, const FPropertyOwner& Owner, const FName& Name, uint32 Offset)
        {
            const FPropertyKindOps& Ops = GetPropertyKindOps(Kind);
            if (!Ops.IsValid() || Ops.BasicType == nullptr)
            {
                return nullptr;
            }
            return Ops.Construct(Owner, MakeDesc(*Owner.Arena, Name, Offset, Ops.BasicType));
        }

        FProperty* MakeScalar(EPropertyTypeFlags Kind, const FPropertyOwner& Owner, const FName& Name, uint32 Offset)
        {
            return GetPropertyKindOps(Kind).bArithmetic ? MakeBasic(Kind, Owner, Name, Offset) : nullptr;
        }

        FProperty* MakeStruct(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CStruct* Resolved)
        {
            return MakeResolved<FStructProperty>(Owner, Name, Offset, EPropertyTypeFlags::Struct, Resolved);
        }

        FProperty* MakeComponentRef(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CStruct* Component)
        {
            return MakeResolved<FComponentRefProperty>(Owner, Name, Offset, EPropertyTypeFlags::ComponentRef, Component);
        }

        FProperty* MakeInstanced(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CStruct* MetaBase)
        {
            return MakeResolved<FInstancedStructProperty>(Owner, Name, Offset, EPropertyTypeFlags::InstancedStruct, MetaBase);
        }

        FProperty* MakeObject(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            return MakeResolved<FObjectProperty>(Owner, Name, Offset, EPropertyTypeFlags::Object,
                TargetClass != nullptr ? TargetClass : CObject::StaticClass());
        }

        FProperty* MakeSoftObject(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CClass* TargetClass)
        {
            return MakeResolved<FSoftObjectProperty>(Owner, Name, Offset, EPropertyTypeFlags::SoftObject,
                TargetClass != nullptr ? TargetClass : CObject::StaticClass());
        }

        bool ScalarSizeAlign(EPropertyTypeFlags Kind, uint32& Size, uint32& Align);

        // The slot is the C# underlying type's width, so a script enum lines up with the managed value.
        FProperty* MakeEnum(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, CEnum* Resolved,
            EPropertyTypeFlags Underlying)
        {
            FEnumProperty* Property = static_cast<FEnumProperty*>(
                MakeResolved<FEnumProperty>(Owner, Name, Offset, EPropertyTypeFlags::Enum, Resolved));

            uint32 InnerSize = 0;
            uint32 InnerAlign = 0;
            if (!ScalarSizeAlign(Underlying, InnerSize, InnerAlign))
            {
                Underlying = EPropertyTypeFlags::Int64;
                InnerSize = sizeof(int64);
            }

            Property->SetElementSize(InnerSize);
            (void)MakeScalar(Underlying, Owner.Inner(Property), Name, Offset);
            return Property;
        }

        FProperty* MakeArray(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FVectorOps* Ops)
        {
            return MakeResolved<FArrayProperty>(Owner, Name, Offset, EPropertyTypeFlags::Vector, Ops);
        }

        bool ScalarSizeAlign(EPropertyTypeFlags Kind, uint32& Size, uint32& Align)
        {
            const FPropertyKindOps& Ops = GetPropertyKindOps(Kind);
            return Ops.bArithmetic && Ops.GetFixedLayout(Size, Align);
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

        // A raw byte move breaks an element that points into itself, such as an inline instanced struct, so a reallocation copies each one through its ops.
        template<typename TCopyInto>
        TVector<uint8> GrowCapacity(TVector<uint8>& Bytes, SIZE_T Stride, SIZE_T WantedBytes, TCopyInto&& CopyInto)
        {
            TVector<uint8> Retired;
            if (WantedBytes <= Bytes.capacity())
            {
                return Retired;
            }

            TVector<uint8> Grown;
            Grown.reserve(Math::Max<SIZE_T>(WantedBytes, Bytes.capacity() * 2));
            Grown.resize(Bytes.size());
            for (SIZE_T Offset = 0; Offset + Stride <= Bytes.size(); Offset += Stride)
            {
                CopyInto(Grown.data() + Offset, Bytes.data() + Offset);
            }

            Retired = Move(Bytes);
            Bytes = Move(Grown);
            return Retired;
        }

        // Run after the caller is done reading a source that may have lived in the old block.
        template<typename TDestruct>
        void DestroyRetired(TVector<uint8>& Retired, SIZE_T Stride, TDestruct&& Destruct)
        {
            for (SIZE_T Offset = 0; Offset + Stride <= Retired.size(); Offset += Stride)
            {
                Destruct(Retired.data() + Offset);
            }
        }

        TVector<uint8> GrowElements(FScriptDynamicArray& Array, SIZE_T WantedBytes)
        {
            const FScriptArrayElementDesc* Element = Array.Element;
            return GrowCapacity(Array.Bytes, Element->Size, WantedBytes, [Element](void* Dst, const void* Src)
            {
                Element->ConstructElement(Dst);
                Element->CopyElement(Dst, Src);
            });
        }

        void DestroyRetiredElements(const FScriptDynamicArray& Array, TVector<uint8>& Retired)
        {
            const FScriptArrayElementDesc* Element = Array.Element;
            DestroyRetired(Retired, Element->Size, [Element](void* Value) { Element->DestructElement(Value); });
        }

        void ArrayPushBack(void* Vector, const void* InElement)
        {
            FScriptDynamicArray* Array = static_cast<FScriptDynamicArray*>(Vector);
            const uint32 ES = Array->Element ? Array->Element->Size : 0;
            if (ES == 0) { return; }
            const SIZE_T Old = Array->Bytes.size();
            TVector<uint8> Retired = GrowElements(*Array, Old + ES);
            Array->Bytes.resize(Old + ES);
            uint8* Slot = Array->Bytes.data() + Old;
            Array->Element->ConstructElement(Slot);
            if (InElement != nullptr) { Array->Element->CopyElement(Slot, InElement); }
            DestroyRetiredElements(*Array, Retired);
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
                TVector<uint8> Retired = GrowElements(*Array, NewCount * ES);
                DestroyRetiredElements(*Array, Retired);
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
            if (ES > 0)
            {
                TVector<uint8> Retired = GrowElements(*Array, NewCount * ES);
                DestroyRetiredElements(*Array, Retired);
            }
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
            Ops.ContainerSize = sizeof(FScriptDynamicArray);
            // The storage counts bytes, so dividing by the element size is what turns it into elements.
            Ops.DataOffset = static_cast<uint32>(offsetof(FScriptDynamicArray, Bytes)) + TVector<uint8>::GetDataOffsetForInterop();
            Ops.CountOffset = static_cast<uint32>(offsetof(FScriptDynamicArray, Bytes)) + TVector<uint8>::GetCountOffsetForInterop();
            Ops.CountDivisor = ElementSize > 0 ? ElementSize : 1;
        }

        // ---- Script dynamic optional (a heap payload, so an engaged optional is a non-null pointer) ----

        bool OptionalHasValue(const void* Container)
        {
            return static_cast<const FScriptDynamicOptional*>(Container)->Payload != nullptr;
        }

        void* OptionalGetValue(void* Container)
        {
            return static_cast<FScriptDynamicOptional*>(Container)->Payload;
        }

        void OptionalReset(void* Container)
        {
            FScriptDynamicOptional* Optional = static_cast<FScriptDynamicOptional*>(Container);
            if (Optional->Payload == nullptr)
            {
                return;
            }

            if (Optional->Desc != nullptr)
            {
                Optional->Desc->Payload.DestructElement(Optional->Payload);
            }
            Memory::Free(Optional->Payload);
            Optional->Payload = nullptr;
        }

        void OptionalSetValue(void* Container, const void* Value)
        {
            FScriptDynamicOptional* Optional = static_cast<FScriptDynamicOptional*>(Container);
            if (Optional->Desc == nullptr || Optional->Desc->Payload.Size == 0)
            {
                return;
            }

            if (Optional->Payload == nullptr)
            {
                Optional->Payload = Memory::Malloc(Optional->Desc->Payload.Size, 16);
                Optional->Desc->Payload.ConstructElement(Optional->Payload);
            }

            if (Value != nullptr)
            {
                Optional->Desc->Payload.CopyElement(Optional->Payload, Value);
            }
        }

        void FillOptionalOps(FScriptOptionalDesc& Desc)
        {
            Desc.Ops.HasValue = &OptionalHasValue;
            Desc.Ops.GetValue = &OptionalGetValue;
            Desc.Ops.SetValue = &OptionalSetValue;
            Desc.Ops.Reset    = &OptionalReset;

            // The description owns the ops, so the self-reference keeps the two from drifting.
            Desc.Ops.ConstructContainer = [](void* Container, const void* Context)
            {
                FScriptDynamicOptional* Optional = Memory::ConstructAt(static_cast<FScriptDynamicOptional*>(Container));
                Optional->Desc = static_cast<const FScriptOptionalDesc*>(Context);
            };
            Desc.Ops.DestructContainer = [](void* Container, const void*)
            {
                static_cast<FScriptDynamicOptional*>(Container)->~FScriptDynamicOptional();
            };
            Desc.Ops.ContainerContext = &Desc;
        }

        FProperty* MakeOptional(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FOptionalOps* Ops)
        {
            return MakeResolved<FOptionalProperty>(Owner, Name, Offset, EPropertyTypeFlags::Optional, Ops);
        }

        // ---- Script dynamic map (type-erased pairs, linear find via the key property's Identical) ----

        FProperty* MakeMap(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FMapOps* Ops)
        {
            return MakeResolved<FMapProperty>(Owner, Name, Offset, EPropertyTypeFlags::Map, Ops);
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

        TVector<uint8> GrowPairs(FScriptDynamicMap& Map, SIZE_T WantedBytes)
        {
            const FScriptMapElementDesc* Desc = Map.Desc;
            return GrowCapacity(Map.Bytes, Desc->PairStride, WantedBytes, [Desc](void* Dst, const void* Src)
            {
                Desc->ConstructPair(Dst);
                Desc->CopyPair(Dst, Src);
            });
        }

        void DestroyRetiredPairs(const FScriptDynamicMap& Map, TVector<uint8>& Retired)
        {
            const FScriptMapElementDesc* Desc = Map.Desc;
            DestroyRetired(Retired, Desc->PairStride, [Desc](void* Pair) { Desc->DestructPair(Pair); });
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
            TVector<uint8> Retired = GrowPairs(*Map, Old + Desc->PairStride);
            Map->Bytes.resize(Old + Desc->PairStride);
            uint8* Pair = Map->Bytes.data() + Old;
            Desc->ConstructPair(Pair);
            if (KeyPtr != nullptr)   { Desc->Key.CopyElement(Desc->KeyAt(Pair), KeyPtr); }
            if (ValuePtr != nullptr) { Desc->Value.CopyElement(Desc->ValueAt(Pair), ValuePtr); }
            DestroyRetiredPairs(*Map, Retired);
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
            if (Map->Desc && Map->Desc->PairStride > 0)
            {
                TVector<uint8> Retired = GrowPairs(*Map, Count * Map->Desc->PairStride);
                DestroyRetiredPairs(*Map, Retired);
            }
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

        bool MapNext(void* InMap, SIZE_T* Cursor, const void** OutKey, void** OutValue)
        {
            MapAt(InMap, (*Cursor)++, OutKey, OutValue);
            return *OutKey != nullptr;
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
            Ops.ContainerSize = sizeof(FScriptDynamicMap);
            Ops.Next = &MapNext;
        }
    }

    namespace
    {
        // The footprint a kind's value occupies, and the reflected struct or enum it names, if any.
        struct FKindLayout
        {
            uint32  Size  = 0;
            uint32  Align = 1;
            CField* Type  = nullptr;
        };

        // How big a kind's value is and which property represents it, answered once for a field and a container element alike.
        struct FKindResolver
        {
            bool (*ResolveLayout)(const FScriptExportType&, const FName&, FKindLayout&) = nullptr;
            FProperty* (*Create)(const FPropertyOwner&, const FName&, uint32, const FScriptExportType&, CField*) = nullptr;
        };

        // An enum occupies its underlying type's width, which is what the managed value is.
        bool ResolveEnumLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
        {
            Out.Type = FindObject<CEnum>(Type.EnumName);
            if (Out.Type == nullptr)
            {
                LOG_WARN("Script property '{}' dropped: enum '{}' is not a reflected type.", DiagName.ToString(), Type.EnumName.ToString());
                return false;
            }
            if (!ScalarSizeAlign(Type.EnumUnderlying, Out.Size, Out.Align))
            {
                Out.Size  = sizeof(int64);
                Out.Align = alignof(int64);
            }
            return true;
        }

        // Every kind whose footprint is a fact the construction table already states.
        bool ResolveFixedLayout(const FScriptExportType& Type, const FName&, FKindLayout& Out)
        {
            return GetPropertyKindOps(Type.Kind).GetFixedLayout(Out.Size, Out.Align);
        }

        // Native or minted from C#, a struct is found by name the same way, since both are reflected types.
        bool ResolveStructLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
        {
            CStruct* Struct = FindObject<CStruct>(Type.NativeName);
            if (Struct == nullptr)
            {
                LOG_WARN("Script property '{}' dropped: struct '{}' is not a reflected type.", DiagName.ToString(), Type.NativeName.ToString());
                return false;
            }
            Out.Type  = Struct;
            Out.Size  = Struct->GetAlignedSize();
            Out.Align = Struct->GetAlignment();
            return true;
        }

        // A reference is as wide as the handle that tracks the component, whatever the component's own size.
        bool ResolveComponentRefLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
        {
            CStruct* Struct = FindObject<CStruct>(Type.NativeName);
            if (Struct == nullptr || Struct->GetComponentOps() == nullptr)
            {
                LOG_WARN("Script property '{}' dropped: '{}' is not a component.", DiagName.ToString(), Type.NativeName.ToString());
                return false;
            }
            Out.Type  = Struct;
            Out.Size  = sizeof(FComponentRef);
            Out.Align = alignof(FComponentRef);
            return true;
        }

        // The base is what the instanced-struct property takes as its meta-base, and any struct deriving it is a candidate.
        bool ResolveInstancedLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
        {
            Out.Type = Type.BaseName.IsNone() ? nullptr : FindObject<CStruct>(Type.BaseName);
            if (!Type.BaseName.IsNone() && Out.Type == nullptr)
            {
                LOG_WARN("Script property '{}' dropped: instanced base '{}' is not a reflected type.", DiagName.ToString(), Type.BaseName.ToString());
                return false;
            }
            Out.Size  = sizeof(FInstancedStruct);
            Out.Align = alignof(FInstancedStruct);
            return true;
        }

        FProperty* CreateBasicProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType& Type, CField*)
        {
            return MakeBasic(Type.Kind, Owner, Name, Offset);
        }

        // An enum property wraps a numeric inner rather than being the bare scalar its size suggests.
        FProperty* CreateEnumProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType& Type, CField* Enum)
        {
            return MakeEnum(Owner, Name, Offset, static_cast<CEnum*>(Enum), Type.EnumUnderlying);
        }

        FProperty* CreateObjectProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType& Type, CField*)
        {
            return MakeObject(Owner, Name, Offset, FScriptableRegistry::FindClass(Type.TargetClass));
        }

        FProperty* CreateSoftObjectProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType& Type, CField*)
        {
            return MakeSoftObject(Owner, Name, Offset, FScriptableRegistry::FindClass(Type.TargetClass));
        }

        FProperty* CreateStructProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType&, CField* Struct)
        {
            return Struct != nullptr ? MakeStruct(Owner, Name, Offset, static_cast<CStruct*>(Struct)) : nullptr;
        }

        FProperty* CreateComponentRefProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType&, CField* Component)
        {
            return Component != nullptr ? MakeComponentRef(Owner, Name, Offset, static_cast<CStruct*>(Component)) : nullptr;
        }

        FProperty* CreateInstancedProperty(const FPropertyOwner& Owner, const FName& Name, uint32 Offset, const FScriptExportType&, CField* Base)
        {
            return MakeInstanced(Owner, Name, Offset, static_cast<CStruct*>(Base));
        }

        // Null for a container kind, which is what refuses one as a container element.
        const FKindResolver* FindKindResolver(EPropertyTypeFlags Kind)
        {
            static const auto Table = []
            {
                TVector<FKindResolver> Entries((size_t)EPropertyTypeFlags::Count);

                for (size_t Index = 0; Index < Entries.size(); ++Index)
                {
                    if (GetPropertyKindOps((EPropertyTypeFlags)Index).bArithmetic)
                    {
                        Entries[Index] = { &ResolveFixedLayout, &CreateBasicProperty };
                    }
                }

                Entries[(size_t)EPropertyTypeFlags::String]          = { &ResolveFixedLayout, &CreateBasicProperty };
                Entries[(size_t)EPropertyTypeFlags::Name]            = { &ResolveFixedLayout, &CreateBasicProperty };
                Entries[(size_t)EPropertyTypeFlags::Object]          = { &ResolveFixedLayout, &CreateObjectProperty };
                Entries[(size_t)EPropertyTypeFlags::SoftObject]      = { &ResolveFixedLayout, &CreateSoftObjectProperty };
                Entries[(size_t)EPropertyTypeFlags::Enum]            = { &ResolveEnumLayout, &CreateEnumProperty };
                Entries[(size_t)EPropertyTypeFlags::Struct]          = { &ResolveStructLayout, &CreateStructProperty };
                Entries[(size_t)EPropertyTypeFlags::InstancedStruct] = { &ResolveInstancedLayout, &CreateInstancedProperty };
                Entries[(size_t)EPropertyTypeFlags::ComponentRef]    = { &ResolveComponentRefLayout, &CreateComponentRefProperty };
                return Entries;
            }();

            const size_t Index = (size_t)Kind;
            return Index < Table.size() && Table[Index].ResolveLayout != nullptr ? &Table[Index] : nullptr;
        }

        bool ResolveKindLayout(const FScriptExportType& Type, const FName& DiagName, FKindLayout& Out)
        {
            const FKindResolver* Resolver = FindKindResolver(Type.Kind);
            return Resolver != nullptr && Resolver->ResolveLayout(Type, DiagName, Out);
        }

        FProperty* MakeForKind(const FPropertyOwner& Owner, const FName& FieldName, uint32 Offset, const FScriptExportType& Type, CField* Resolved)
        {
            const FKindResolver* Resolver = FindKindResolver(Type.Kind);
            return Resolver != nullptr ? Resolver->Create(Owner, FieldName, Offset, Type, Resolved) : nullptr;
        }
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
        Out.ElementType = Layout.Type;
        return true;
    }

    FProperty* CScriptStruct::CreateElement(void* ArrayOwner, const FScriptExportType& Type, FScriptArrayElementDesc& Desc)
    {
        const FPropertyOwner Owner = FPropertyOwner{ &GetPropertyArena() }.Inner(static_cast<FProperty*>(ArrayOwner));
        FProperty* Property = MakeForKind(Owner, FName("Element"), 0, Type, Desc.ElementType);
        if (Property == nullptr)
        {
            return nullptr;
        }

        // The author's metadata belongs to the field that OWNS the container and is applied there.
        const FKindTag Tag = KindTag(Type);
        if (Tag.Key != nullptr)
        {
            ApplyMeta(GetPropertyArena(), Property, nullptr, Tag);
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

        if (Type.Kind == EPropertyTypeFlags::Optional && Type.ElementType)
        {
            FScriptOptionalDesc* Desc = Memory::New<FScriptOptionalDesc>();
            OptionalDescs.push_back(Desc);
            if (!ResolveElement(*Type.ElementType, Field.Name, Desc->Payload))
            {
                return false;
            }
            FillOptionalOps(*Desc);
            Out.bOptional = true;
            Out.OptionalDesc = Desc;
            Out.Size = sizeof(FScriptDynamicOptional);
            Out.Align = alignof(FScriptDynamicOptional);
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
        Out.Type = Layout.Type;
        return true;
    }

    FProperty* CScriptStruct::CreateProperty(CStruct* Target, const FFieldPlan& Plan, uint32 Offset, TVector<FProperty*>* Collector)
    {
        const FScriptExportField& Field = *Plan.Field;
        const FScriptExportType& Type = *Field.Type;

        // Either the target owns the property as a member or the collector takes it as a parameter, never both.
        const FPropertyOwner Owner{ &GetPropertyArena(), Collector != nullptr ? nullptr : Target, nullptr, Collector };

        if (Plan.bArray)
        {
            FProperty* Array = MakeArray(Owner, Field.Name, Offset, &Plan.ArrayDesc->Ops);
            FProperty* Inner = CreateElement(Array, *Type.ElementType, *Plan.ArrayDesc);
            Plan.ArrayDesc->Inner = Inner;
            ApplyMeta(GetPropertyArena(), Array, &Field.Meta, FKindTag{});
            ApplyDeclaredFlags(Array, Field.Flags);
            ApplyHidden(Array, Field.Meta);
            return Array;
        }
        if (Plan.bOptional)
        {
            FProperty* Optional = MakeOptional(Owner, Field.Name, Offset, &Plan.OptionalDesc->Ops);
            FProperty* Inner = CreateElement(Optional, *Type.ElementType, Plan.OptionalDesc->Payload);
            Plan.OptionalDesc->Payload.Inner = Inner;
            ApplyMeta(GetPropertyArena(), Optional, &Field.Meta, FKindTag{});
            ApplyDeclaredFlags(Optional, Field.Flags);
            ApplyHidden(Optional, Field.Meta);
            return Optional;
        }
        if (Plan.bMap)
        {
            FProperty* Map = MakeMap(Owner, Field.Name, Offset, &Plan.MapDesc->Ops);
            // The dynamic map ops use the key inner's compare, so both inners must be set here.
            FProperty* KeyInner   = CreateElement(Map, *Type.KeyType, Plan.MapDesc->Key);
            FProperty* ValueInner = CreateElement(Map, *Type.ValueType, Plan.MapDesc->Value);
            Plan.MapDesc->Key.Inner   = KeyInner;
            Plan.MapDesc->Value.Inner = ValueInner;
            ApplyMeta(GetPropertyArena(), Map, &Field.Meta, FKindTag{});
            ApplyDeclaredFlags(Map, Field.Flags);
            ApplyHidden(Map, Field.Meta);
            return Map;
        }
        FProperty* Property = MakeForKind(Owner, Field.Name, Offset, Type, Plan.Type);
        if (Property == nullptr)
        {
            return nullptr;
        }

        // The tag is what makes the editor draw a picker instead of the raw value.
        ApplyMeta(GetPropertyArena(), Property, &Field.Meta, KindTag(Type));
        ApplyDeclaredFlags(Property, Field.Flags);
        ApplyHidden(Property, Field.Meta);
        return Property;
    }

    CScriptStruct::FEmittedLayout CScriptStruct::EmitLayoutInto(CStruct* Target, uint32 BaseOffset, const FScriptExportSchema& Schema, TVector<FProperty*>* Collector)
    {
        FEmittedLayout Result;
        Result.EndOffset = BaseOffset;
        Result.Alignment = 1;

        if (Target == nullptr)
        {
            return Result;
        }

        // Fields plus their container inners, so a schema's whole property set lands in one block.
        GetPropertyArena().Reserve(Schema.Fields.size() * 3 * 192);

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

            FProperty* Property = CreateProperty(Target, Plan, Offset, Collector);
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

    bool CScriptStruct::BuildFromSchema(const FScriptExportSchema& Schema, CStruct* Base)
    {
        NativeBase = Base;
        if (Base != nullptr)
        {
            SetSuperStruct(Base);
        }

        const uint32 BaseSize = Base != nullptr ? Base->GetSize() : 0;
        const FEmittedLayout Layout = EmitLayoutInto(this, BaseSize, Schema);
        OwnProperties = Layout.Properties;

        const uint32 BaseAlign = Base != nullptr ? Base->GetAlignment() : 1;
        Alignment = Math::Max(Layout.Alignment, BaseAlign);
        const uint32 EndOffset = Math::Max(Layout.EndOffset, BaseSize);
        Size = (EndOffset > 0) ? Align(EndOffset, Alignment) : 0;
        Link();

        bRequiresLifecycle = Base != nullptr && Base->RequiresValueLifecycle();
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
            // The C# initializers land in the defaults afterwards, so a zeroed buffer is never a valid instance.
            bRequiresLifecycle = true;
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
        if (NativeBase != nullptr)
        {
            NativeBase->InitializeStruct(Buffer);
        }
        for (FProperty* Property : OwnProperties)
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
        for (FProperty* Property : OwnProperties)
        {
            if (Property->OwnsStorage())
            {
                Property->DestructValue(static_cast<uint8*>(Buffer) + Property->Offset);
            }
        }
        if (NativeBase != nullptr)
        {
            NativeBase->DestroyStruct(Buffer);
        }
    }

    void CScriptStruct::CopyInto(void* Dst, const void* Src) const
    {
        if (Dst == nullptr || Src == nullptr)
        {
            return;
        }
        if (NativeBase != nullptr)
        {
            NativeBase->CopyStruct(Dst, Src);
        }
        for (FProperty* Property : OwnProperties)
        {
            Property->CopyCompleteValue(
                static_cast<uint8*>(Dst) + Property->Offset,
                static_cast<const uint8*>(Src) + Property->Offset);
        }
    }

    void CScriptStruct::ResetHotReloadFields(void* Buffer) const
    {
        if (Buffer == nullptr || Defaults == nullptr)
        {
            return;
        }
        for (FProperty* Property : GetProperties())
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

        // Every property this record emitted lives in its arena, whichever type they were attached to.
        OwnProperties.clear();
        GetPropertyArena().Reset();
        Unlink();

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

        for (FScriptOptionalDesc* Desc : OptionalDescs)
        {
            Memory::Delete(Desc);
        }
        OptionalDescs.clear();

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

    FScriptDynamicOptional::~FScriptDynamicOptional()
    {
        if (Payload != nullptr)
        {
            if (Desc != nullptr)
            {
                Desc->Payload.DestructElement(Payload);
            }
            Memory::Free(Payload);
            Payload = nullptr;
        }
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

    namespace
    {
        // Metadata and defaults are compared apart from shape, so each gets its own remedy on a reload.
        bool FieldMetadataMatches(const FScriptExportMeta& A, const FScriptExportMeta& B)
        {
            if (A.Entries.size() != B.Entries.size())
            {
                return false;
            }
            for (const FScriptExportMetaArg& Entry : A.Entries)
            {
                const FString* Other = B.Find(Entry.Key);
                if (Other == nullptr || *Other != Entry.Value)
                {
                    return false;
                }
            }
            return true;
        }

        CScriptStruct* GetLayoutRecord(const CScriptClass* Target)
        {
            return Target != nullptr ? Cast<CScriptStruct>(Target->LayoutRecord.Get()) : nullptr;
        }
    }

    uint32 AppendScriptPropertiesToClass(CScriptClass* Target, const FScriptExportSchema& Schema)
    {
        // A behavior-only type still gets a record, since its functions are laid out in it.
        if (Target == nullptr || (Schema.Fields.empty() && Schema.Functions.empty()))
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

        TStrongObjectPtr<CScriptStruct> Record = static_cast<CScriptStruct*>(StaticAllocateObject(Params));
        FinishRegistration(Record.Get());
        Record->MarkClassLayoutRecord();

        // A class minted before its script parent was laid out still has the parent's old size, so it starts past the parent's block now.
        if (const CClass* Parent = Target->GetSuperClass())
        {
            Target->Alignment = Math::Max(Target->GetAlignment(), Parent->GetAlignment());
            Target->Size      = Math::Max(Target->GetSize(), (uint32)Align(Parent->GetSize(), Target->GetAlignment()));
        }

        // Appended properties start past the shim, so they can never overlap a native member.
        const uint32 ShimSize  = Target->GetSize();
        const uint32 ShimAlign = Target->GetAlignment();
        const CScriptStruct::FEmittedLayout Layout = Record->EmitLayoutInto(Target, ShimSize, Schema);

        // A behaviour-only type has no block to append, and still wants its record and its functions.
        if (!Layout.Properties.empty())
        {
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
        }

        // The record's arena owns the properties, so anchoring it on the class is what keeps them alive.
        Record->SetAppliedSchema(Schema);
        Target->LayoutRecord = static_cast<CStruct*>(Record.Get());

        // After the block, since a function's frame lives in the same record, and before the caller links, which flattens functions.
        for (const FScriptExportFunction& Declared : Schema.Functions)
        {
            FScriptExportSchema ParamSchema;
            ParamSchema.Fields = Declared.Params;

            MintScriptFunction(*Target, *Record, Declared.Name, ParamSchema, Declared.ReturnIndex,
                &ScriptFunctionThunk, static_cast<EFunctionFlags>(Declared.Flags), Declared.Meta);
        }
        Target->ShimSize = ShimSize;
        Target->ShimAlign = ShimAlign;
        Target->bHasAppendedBlock = !Layout.Properties.empty();

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
            if (Type->bInputAction) { Out += "a"; }
            if (!Type->EnumName.IsNone())    { Out += "#"; Out += Type->EnumName.ToString(); Out += Format("/{}", (int32)Type->EnumUnderlying).c_str(); }
            if (!Type->NativeName.IsNone())  { Out += "@"; Out += Type->NativeName.ToString(); }
            if (!Type->TargetClass.IsNone()) { Out += ">"; Out += Type->TargetClass.ToString(); }
            if (!Type->BaseName.IsNone())    { Out += "^"; Out += Type->BaseName.ToString(); }

            if (Type->ElementType) { Out += "["; AppendTypeSignature(Type->ElementType.get(), Out); Out += "]"; }
            if (Type->KeyType)     { Out += "{"; AppendTypeSignature(Type->KeyType.get(), Out); Out += "}"; }
            if (Type->ValueType)   { Out += "("; AppendTypeSignature(Type->ValueType.get(), Out); Out += ")"; }

        }
    }

    void ResetSkipHotReloadProperties(CObject* Object)
    {
        if (Object == nullptr)
        {
            return;
        }
        CScriptClass* ScriptClass = ToScriptClass(Object->GetClass());
        if (ScriptClass == nullptr)
        {
            return;
        }
        CObject* Defaults = ScriptClass->GetDefaultObjectIfCreated();
        if (Defaults == nullptr)
        {
            return;
        }
        // Only the C#-declared block can carry the attribute, and a native shim member is unaffected.
        ScriptClass->ForEachScriptProperty([&](FProperty* Property)
        {
            if (Property != nullptr && Property->HasMetadata("SkipHotReload"))
            {
                Property->CopyCompleteValue_InContainer(Object, Defaults);
            }
        });
    }

    namespace
    {
        bool FunctionsMatch(const TVector<FScriptExportFunction>& Old, const TVector<FScriptExportFunction>& New)
        {
            if (Old.size() != New.size())
            {
                return false;
            }
            for (size_t Index = 0; Index < New.size(); ++Index)
            {
                const FScriptExportFunction& A = Old[Index];
                const FScriptExportFunction& B = New[Index];
                if (A.Name != B.Name || A.ReturnIndex != B.ReturnIndex || A.Flags != B.Flags || A.Params.size() != B.Params.size())
                {
                    return false;
                }
                for (size_t Param = 0; Param < B.Params.size(); ++Param)
                {
                    FString OldShape;
                    FString NewShape;
                    AppendFieldSignature(A.Params[Param], OldShape);
                    AppendFieldSignature(B.Params[Param], NewShape);
                    if (OldShape != NewShape)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    namespace
    {
        bool IsSupersededScriptStruct(const CStruct* Struct)
        {
            const CScriptStruct* Script = Struct != nullptr ? Cast<CScriptStruct>(Struct) : nullptr;
            return Script != nullptr && Script->IsSuperseded();
        }

        // A field names its struct rather than carrying its shape, so a reshaped struct is found through the property instead.
        bool ReferencesSupersededStruct(const FProperty* Property)
        {
            if (Property == nullptr)
            {
                return false;
            }
            if (Property->GetType() == EPropertyTypeFlags::Struct
                && IsSupersededScriptStruct(static_cast<const FStructProperty*>(Property)->GetStruct()))
            {
                return true;
            }
            if (Property->GetType() == EPropertyTypeFlags::InstancedStruct
                && IsSupersededScriptStruct(static_cast<const FInstancedStructProperty*>(Property)->GetMetaStruct()))
            {
                return true;
            }
            if (Property->GetType() == EPropertyTypeFlags::Vector)
            {
                return ReferencesSupersededStruct(static_cast<const FArrayProperty*>(Property)->GetInternalProperty());
            }
            if (Property->GetType() == EPropertyTypeFlags::Optional)
            {
                return ReferencesSupersededStruct(static_cast<const FOptionalProperty*>(Property)->GetInternalProperty());
            }
            if (Property->GetType() == EPropertyTypeFlags::Map)
            {
                const FMapProperty* Map = static_cast<const FMapProperty*>(Property);
                return ReferencesSupersededStruct(Map->GetKeyProperty()) || ReferencesSupersededStruct(Map->GetValueProperty());
            }
            return false;
        }
    }

    EScriptTypeDirty DiffScriptLayout(const FScriptExportSchema& Applied, TSpan<FProperty* const> Emitted, const FScriptExportSchema& Schema)
    {
        for (const FProperty* Property : Emitted)
        {
            if (ReferencesSupersededStruct(Property))
            {
                return EScriptTypeDirty::Layout;
            }
        }

        if (Applied.Fields.size() != Schema.Fields.size())
        {
            return EScriptTypeDirty::Layout;
        }

        // Functions are minted with the block, so one added, dropped or reshaped rebuilds the class the way a field would.
        if (!FunctionsMatch(Applied.Functions, Schema.Functions))
        {
            return EScriptTypeDirty::Layout;
        }

        EScriptTypeDirty Dirty = EScriptTypeDirty::None;
        for (size_t Index = 0; Index < Schema.Fields.size(); ++Index)
        {
            const FScriptExportField& Old = Applied.Fields[Index];
            const FScriptExportField& New = Schema.Fields[Index];

            // Name and shape decide the layout, and one differing field makes the rest moot.
            FString OldShape;
            FString NewShape;
            AppendFieldSignature(Old, OldShape);
            AppendFieldSignature(New, NewShape);
            if (OldShape != NewShape)
            {
                return EScriptTypeDirty::Layout;
            }

            if (!FieldMetadataMatches(Old.Meta, New.Meta))
            {
                EnumAddFlags(Dirty, EScriptTypeDirty::Metadata);
            }
        }
        return Dirty;
    }

    EScriptTypeDirty DiffScriptClassLayout(const CScriptClass* Target, const FScriptExportSchema& Schema)
    {
        const CScriptStruct* Record = GetLayoutRecord(Target);
        if (Target == nullptr || Record == nullptr)
        {
            // Nothing appended yet, so anything the schema asks for is a layout change.
            return Schema.IsValid() ? EScriptTypeDirty::Layout : EScriptTypeDirty::None;
        }
        return DiffScriptLayout(Record->GetAppliedSchema(), Target->ScriptProperties, Schema);
    }

    void RefreshScriptPropertyMetadata(CScriptClass* Target, const FScriptExportSchema& Schema)
    {
        CScriptStruct* Record = GetLayoutRecord(Target);
        if (Record == nullptr || Target->ScriptProperties.size() != Schema.Fields.size())
        {
            return;
        }

        // Positional, which is exactly what the caller has already proven by finding no Layout change.
        for (size_t Index = 0; Index < Schema.Fields.size(); ++Index)
        {
            ApplyMeta(Record->GetPropertyArena(), Target->ScriptProperties[Index],
                &Schema.Fields[Index].Meta, KindTag(*Schema.Fields[Index].Type));
        }
        Record->SetAppliedSchema(Schema);
    }

    bool ScriptClassLayoutMatches(const CScriptClass* Target, const FScriptExportSchema& Schema)
    {
        return !EnumHasAnyFlags(DiffScriptClassLayout(Target, Schema), EScriptTypeDirty::Layout);
    }

    void ForgetScriptClassLayout(CScriptClass* Target)
    {
        if (Target == nullptr)
        {
            return;
        }

        // The type is gone with no live instances, so releasing the record frees the arena its properties live in.
        Target->ScriptProperties.clear();
        Target->ScriptLifecycleProperties.clear();
        Target->LayoutRecord = nullptr;
        Target->bHasAppendedBlock = false;
    }
}
