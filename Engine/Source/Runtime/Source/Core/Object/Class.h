#pragma once

#include "Lumina.h"
#include "Object.h"
#include "Class/StructTraits.h"
#include "Containers/Function.h"
#include "Core/Reflection/Type/Metadata/PropertyMetadata.h"
#include "Core/Templates/Align.h"
#include "Initializer/ObjectInitializer.h"
#include "Memory/SmartPtr.h"

RUNTIME_API Lumina::CClass* Construct_CClass_Lumina_CStruct();

namespace Lumina
{
    class FProperty;
    class FNetArchive;
    struct VTransform;
    using FTransform = VTransform;
}

namespace Lumina
{
    
    class LUMINA_VISIBLE_TYPE CField : public CObject
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


    class LUMINA_VISIBLE_TYPE CEnum : public CField
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
    class LUMINA_VISIBLE_TYPE CStruct : public CField
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

        /** Whether a zeroed buffer is NOT already a valid instance of this struct, i.e. an instance has to be
         *  constructed and destructed rather than just memzeroed. Asked instead of testing for particular
         *  struct kinds, so FStructProperty can answer OwnsStorage without knowing what it points at. */
        RUNTIME_API virtual bool RequiresValueLifecycle() const;

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

        using Super::Serialize;
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

        /**
         * Drops this struct's property list and clears the linked latch, so AddProperty + Link can run again.
         *
         * Only the HEAD is dropped. Link splices the super's chain onto this struct's tail, so the list this
         * walks is partly borrowed; the super still owns its own properties and is untouched. Whatever this
         * struct itself added is the caller's to dispose of, and this does not free it.
         *
         * Exists for one caller: rebuilding a runtime-minted class's appended property block when a C# hot
         * reload changes the script's property set. Do not use it on a compile-time class.
         */
        RUNTIME_API void Unlink();

        RUNTIME_API FFixedString MakeDisplayName() const override;

    private:

        // Memory::Delete, not the default delete: MakeStructOps allocates through Memory::New so the
        // block is owned by Runtime's allocator no matter which module built the struct. The two have
        // to be named together or the pairing silently rots.
        TUniquePtr<FStructOps, smart_ptr_deleter<FStructOps>> StructOps;
        CStruct* SuperStruct = nullptr;
        bool bLinked = false;

        /** Lazy default-constructed instance for property-editor diff/reset. Allocated once, never freed. */
        void* DefaultInstance = nullptr;
    };


    /** Final class for fields and functions. */
    class LUMINA_VISIBLE_TYPE CClass final : public CStruct
    {
    public:

        DECLARE_CLASS(Lumina, CClass, CStruct, "/Script/Engine", RUNTIME_API)
        DEFINE_CLASS_FACTORY(CClass)

        using FactoryFunctionType = CObject*(*)(void*);

        /** For a runtime-minted C# subclass of a REFLECT(Scriptable) native class: which ScriptEvents the
         *  subclass actually overrides (bit i == the wrapper's [ScriptEvent(i)]). Zero for every native class,
         *  which is exactly what makes a non-overridden event cost one predictable class-level test in the
         *  generated shim instead of a per-instance lookup. Set once at mint (FScriptableRegistry). */
        uint64 ScriptOverrides = 0;

        /** Every property appended to this class from a script type's schema, in layout order. They live past
         *  the C++ shim the class was minted from. Empty for every native class. */
        TVector<FProperty*> ScriptProperties;

        /** The subset of ScriptProperties whose values own storage, so they need construction/destruction over
         *  the object's trailing block. Holds the properties themselves rather than any description of them:
         *  each one knows how to build its own value (FProperty::ConstructValue), so adding a new property
         *  kind teaches that kind and changes nothing here. */
        TVector<FProperty*> ScriptLifecycleProperties;

        /** Placement-constructs every script-appended property. Called from StaticAllocateObject once the
         *  object's class is known, before PostInitProperties -- so a script's first callback sees valid
         *  values rather than a memzeroed FString, which is a plausible-looking empty string that corrupts
         *  on the first assignment. Returns whether anything was constructed, which is what stamps
         *  OF_ScriptProperties and therefore what decides if the destructor comes back here at all. */
        RUNTIME_API bool ConstructScriptProperties(void* Object) const;

        /** Mirror of the above, from ~CObjectBase. Must be kept in lockstep: a construct without its destruct
         *  leaks, the reverse double-frees. */
        RUNTIME_API void DestructScriptProperties(void* Object) const;

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

        /**
         * Destroys the CDO and forgets it, so the next GetDefaultObject builds a fresh one.
         *
         * CreateDefaultObject asserts the slot is empty, and it allocates from GetSize(), so a class whose
         * layout changed cannot reuse the old CDO: it is the wrong size and holds the old property set.
         * Paired with Unlink by the minted-class rebuild; nothing else should need it.
         */
        RUNTIME_API void DiscardDefaultObject();

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
