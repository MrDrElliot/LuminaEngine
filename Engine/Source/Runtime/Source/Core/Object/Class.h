#pragma once

#include "Lumina.h"
#include "Object.h"
#include "Class/StructTraits.h"
#include "Containers/Function.h"
#include "Core/Reflection/Type/Metadata/PropertyMetadata.h"
#include "Core/Templates/Align.h"
#include "Initializer/ObjectInitializer.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class FProperty;
    class FNetArchive;
    struct VTransform;
    using FTransform = VTransform;
}

namespace Lumina
{
    
    class CField : public CObject
    {
    public:

        DECLARE_CLASS(Lumina, CField, CObject, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CField)
        
        CField() = default;
        
        
        CField(CPackage* Package, const FName& InName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags)
            : CObject(nullptr, InFlags, Package, InName, FGuid::New())
            , Size(InSize)
            , Alignment(InAlignment)
        {}

        FProperty* LinkedProperty = nullptr;

        RUNTIME_API uint32 GetAlignedSize() const { return Align(Size, Alignment); }
        RUNTIME_API uint32 GetSize() const { return Size; }
        RUNTIME_API uint32 GetAlignment() const { return Alignment; }
        
        RUNTIME_API bool HasMeta(const FName& Key) const;
        RUNTIME_API const FString& GetMeta(const FName& Key) const;

        
        uint32 Size = 0;
        uint32 Alignment = 0;
        
        FMetaDataPair Metadata;
    };


    class CEnum : public CField
    {
    public:
        
        DECLARE_CLASS(Lumina, CEnum, CField, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CEnum)

        CEnum() = default;

        RUNTIME_API FName GetNameAtValue(uint64 Value);
        RUNTIME_API uint64 GetEnumValueByName(const FName& Name);
        RUNTIME_API FFixedString GetValueOrBitFieldAsString(int64 Value);
        
        RUNTIME_API FName GetNameAtIndex(int64 Index) const  { return Names[Index].first; }
        RUNTIME_API uint64 GetValueAtIndex(int64 Index) const { return Names[Index].second; }
        
        void AddEnum(FName Name, uint64 Value);
        void ForEachEnum(const TFunction<void(const TPair<FName, uint64>&)>& Functor);
        FFixedString MakeDisplayName() const override;
        
        NODISCARD bool IsBitmaskEnum() const { return HasMeta("BitMask"); }

        TVector<TPair<FName, uint64>> Names;
        
    };
    

    /** Reflected type with properties; supports single inheritance via SuperStruct. */
    class CStruct : public CField
    {
        friend RUNTIME_API void ConstructCStruct(CStruct** OutStruct, const FStructParams& Params);

        DECLARE_CLASS(Lumina, CStruct, CField, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CStruct)

    public:

        CStruct() = default;

        CStruct(CPackage* Package, const FName& InName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags)
            : CField(Package, InName, InSize, InAlignment, InFlags)
        {}

        RUNTIME_API virtual void SetSuperStruct(CStruct* InSuper);

        RUNTIME_API void RegisterDependencies() override;

        RUNTIME_API CStruct* GetSuperStruct() const { return SuperStruct; }

        /** Searches full inheritance chain. */
        RUNTIME_API FProperty* GetProperty(const FName& Name) const;

        RUNTIME_API virtual void AddProperty(FProperty* Property);

        RUNTIME_API FStructOps* GetStructOps() const { return StructOps.get(); }

        /** Lazy default-constructed instance for property-editor diff/reset. Null if not default-constructible. Never destructed. */
        RUNTIME_API virtual void* GetDefaultInstance();

        //~ Value lifetime for one instance in caller-owned raw memory. Default uses FStructOps, else
        //~ walks the property list. CScriptStruct overrides these. Used by FInstancedStruct.
        RUNTIME_API virtual void InitializeStruct(void* Dest) const;
        RUNTIME_API virtual void DestroyStruct(void* Dest) const;
        RUNTIME_API virtual void CopyStruct(void* Dest, const void* Src) const;
        RUNTIME_API virtual bool CompareStruct(const void* A, const void* B) const;

        /** Reflected-property serialization with tags for versioning/skip support. */
        RUNTIME_API void SerializeTaggedProperties(FArchive& Ar, void* Data) const;

        /** Compact network serialization: walks PROPERTY(Replicated) fields (this struct + supers) in a
         *  fixed, tag-less order, calling each property's NetSerialize. Both peers must share the layout. */
        RUNTIME_API void NetSerializeProperties(FNetArchive& Ar, void* Data) const;

        /** Like NetSerializeProperties but serializes EVERY serializable field (not just Replicated ones),
         *  honoring a custom StructOps serializer. Used by FProperty::NetSerialize for nested structs. */
        RUNTIME_API void NetSerializeAll(FNetArchive& Ar, void* Data) const;

        //~ Per-field net delta helpers. The PROPERTY(Replicated) set/order is identical on both peers (same
        //~ build), so a changed-field bitmask is self-describing on the wire -- no per-field tags or sizes.

        /** Count of replicated fields (the bitmask width). Same walk/filter as NetSerializeProperties. */
        RUNTIME_API uint32 GetNetReplicatedPropertyCount() const;

        /** Writer-side diff support: serialize each replicated field into its own whole-byte buffer so the
         *  caller can compare against the last-sent baseline. Net-index hooks are copied from HookSource so
         *  object/asset/name refs mint exactly as they would on the live archive. */
        RUNTIME_API void NetSerializeReplicatedToBuffers(const FNetArchive& HookSource, void* Data, TVector<TVector<uint8>>& OutPerField) const;

        /** Reader side: for each field whose Mask bit is set, deserialize it then byte-align (matching the
         *  whole-byte field buffers the writer emitted). Fields whose bit is clear keep their current value. */
        RUNTIME_API void NetReadReplicatedMasked(FNetArchive& Ar, void* Data, const uint8* Mask) const;

        /** Structured (named-field) variant; drives each property's SerializeItem. Used by the
         *  JSON backend so reflected data round-trips through human-readable named fields. */
        RUNTIME_API void SerializeTaggedProperties(IStructuredArchive::FRecord& Record, void* Data, void const* Defaults = nullptr) const;

        void Serialize(FArchive& Ar) override { }
        void Serialize(IStructuredArchive::FRecord& Slot) override { }
    
        /** Caller must ensure the cast is valid; no type check. */
        template<typename PropertyType>
        PropertyType* GetProperty(const FName& Name)
        {
            return static_cast<PropertyType*>(GetProperty(Name));
        }

        template<typename PropertyType, typename TFunc>
        requires (eastl::is_base_of_v<FProperty, PropertyType> && eastl::is_invocable_v<TFunc, PropertyType*>)
        void ForEachProperty(TFunc&& Func)
        {
            PropertyType* Current = static_cast<PropertyType*>(LinkedProperty);
            while (Current != nullptr)
            {
                eastl::invoke(Func, Current);
                Current = static_cast<PropertyType*>(Current->Next);
            }
        }

        template<class T>
        bool IsChildOf() const
        {
            return IsChildOf(T::StaticClass());
        }

        RUNTIME_API bool IsChildOf(const CStruct* Base) const;

        /** Finalizes the property list. Must run after all AddProperty calls and before runtime use. */
        RUNTIME_API virtual void Link();

        RUNTIME_API FFixedString MakeDisplayName() const override;

    private:

        TUniquePtr<FStructOps> StructOps;
        CStruct* SuperStruct = nullptr;
        bool bLinked = false;

        /** Lazy default-constructed instance for property-editor diff/reset. Allocated once, never freed. */
        void* DefaultInstance = nullptr;
    };


    /** Final class for fields and functions. */
    class CClass final : public CStruct
    {
    public:

        DECLARE_CLASS(Lumina, CClass, CStruct, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CClass)

        using FactoryFunctionType = CObject*(*)(void*);
                
        CClass() = default;

        CClass(CPackage* Package, const FName& InName, uint32 InSize, uint32 InAlignment, EObjectFlags InFlags, FactoryFunctionType InFactory)
            : CStruct(Package, InName, InSize, InAlignment, InFlags)
            , FactoryFunction(InFactory)
        {}


        RUNTIME_API CObject* EmplaceInstance(void* Memory) const;

        RUNTIME_API CClass* GetSuperClass() const;

        RUNTIME_API CObject* GetDefaultObject() const;

        /** The CDO if one has been created; never creates it (unlike GetDefaultObject). */
        RUNTIME_API CObject* GetDefaultObjectIfCreated() const { return ClassDefaultObject; }

        /** Routes to the CDO so object and struct details panels share one path. */
        RUNTIME_API void* GetDefaultInstance() override;

        template<typename T>
        T* GetDefaultObject() const
        {
            return static_cast<T*>(GetDefaultObject());
        }


        mutable int32   ClassUnique = 0;

        FactoryFunctionType FactoryFunction = nullptr;

    protected:

        RUNTIME_API CObject* CreateDefaultObject();

    private:

        CObject*        ClassDefaultObject = nullptr;

    };

    template<class T>
    void InternalConstructor(const FObjectInitializer& IO)
    { 
        T::__DefaultConstructor(IO);
    }

    template<class T>
    void InternalAllocator(const FObjectInitializer& IO)
    { 
        T::__DefaultAllocator(IO);
    }

    RUNTIME_API void AllocateStaticClass(const TCHAR* Package, const TCHAR* Name, CClass** OutClass, uint32 Size, uint32 Alignment, CClass* (*SuperClassFn)(), CClass::FactoryFunctionType FactoryFunc);
    

    template<typename Class>
    FORCEINLINE FString GetClassName()
    {
    	return Class::StaticClass()->GetName();
    }
    
    template <typename T>
    struct TBaseStructureBase
    {
        static CStruct* Get()
        {
            return T::StaticStruct();
        }
    };

    template <typename T>
    struct TBaseStructure : TBaseStructureBase<T>
    {
    };

    template<> struct TBaseStructure<FVector2>
    {
        static RUNTIME_API CStruct* Get();
    };

    template<> struct TBaseStructure<FVector3>
    {
        static RUNTIME_API CStruct* Get();
    };

    template<> struct TBaseStructure<FVector4>
    {
        static RUNTIME_API CStruct* Get();
    };

    template<> struct TBaseStructure<FQuat>
    {
        static RUNTIME_API CStruct* Get();
    };

    template<> struct TBaseStructure<FTransform>
    {
        static RUNTIME_API CStruct* Get();
    };
}
