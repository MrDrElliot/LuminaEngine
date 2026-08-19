#pragma once

#include "ConstructObjectParams.h"
#include "ObjectFlags.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"



namespace Lumina
{
    struct FStructOps;
    class CPackage;
    class CStruct;
    class CObjectBase;
    class CEnum;
    class CObject;
    class CClass;
}

namespace Lumina
{

    namespace Concept
    {
        template<typename T>
        concept IsACObject = std::is_base_of_v<CObject, T>;
    }
    
    RUNTIME_API CObject* StaticAllocateObject(const FConstructCObjectParams& Params);

    RUNTIME_API CObject* FindObjectImpl(const FGuid& ObjectGUID);
    RUNTIME_API CObject* FindObjectImpl(const FName& Name, CClass* Class);
    RUNTIME_API CObject* StaticLoadObject(const FGuid& GUID);
    RUNTIME_API void AsyncLoadObject(const FGuid& GUID, const TFunction<void(CObject*)>& Callback);
    RUNTIME_API void AsyncLoadObject(const FName& Name, const TFunction<void(CObject*)>& Callback);
    RUNTIME_API CObject* StaticLoadObject(FStringView Name);

    // Top-level "load this asset and its whole dependency closure in parallel" (CPackage::LoadAssetGraph).
    // Use for big fan-out opens (worlds, level travel); falls back to the inline StaticLoadObject when the
    // target isn't a registered asset. Do NOT call from inside a load to resolve a single reference -- that
    // is StaticLoadObject's job (the graph loader is built on top of it).
    RUNTIME_API CObject* StaticLoadObjectGraph(const FGuid& GUID);
    RUNTIME_API CObject* StaticLoadObjectGraph(FStringView Name);

    RUNTIME_API FFixedString SanitizeObjectName(FStringView Name);

    RUNTIME_API bool IsValid(const CObjectBase* Obj);
    RUNTIME_API bool IsValid(CObjectBase* Obj);
    
    template<Concept::IsACObject T>
    T* FindObject(const FGuid& GUID)
    {
        return static_cast<T*>(FindObjectImpl(GUID));
    }

    template<Concept::IsACObject T>
    T* FindObject(const FName& Name)
    {
        return static_cast<T*>(FindObjectImpl(Name, T::StaticClass()));
    }

    template<Concept::IsACObject T>
    T* FindObject(CClass& Class, const FName& Name)
    {
        return static_cast<T*>(FindObjectImpl(Name, &Class));
    }

    template<Concept::IsACObject T>
    T* LoadObject(const FGuid& GUID)
    {
        return static_cast<T*>(StaticLoadObject(GUID));
    }
    
    template<Concept::IsACObject T>
    T* LoadObject(FStringView Name)
    {
        return static_cast<T*>(StaticLoadObject(Name));
    }

    template<Concept::IsACObject T>
    T* LoadObjectGraph(const FGuid& GUID)
    {
        return static_cast<T*>(StaticLoadObjectGraph(GUID));
    }

    template<Concept::IsACObject T>
    T* LoadObjectGraph(FStringView Name)
    {
        return static_cast<T*>(StaticLoadObjectGraph(Name));
    }

    RUNTIME_API CObject* NewObject(CClass* InClass, CPackage* Package = nullptr, const FName& Name = NAME_None, const FGuid& GUID = FGuid::New(), EObjectFlags Flags = OF_None);
    RUNTIME_API void GetObjectsWithPackage(const CPackage* Package, TVector<CObject*>& OutObjects);


    template<Concept::IsACObject T>
    T* NewObject(EObjectFlags Flags)
    {
        return static_cast<T*>(NewObject(T::StaticClass(), nullptr, NAME_None, FGuid::New(), Flags));
    }
    
    template<Concept::IsACObject T>
    T* NewObject(CPackage* Package = nullptr, const FName& Name = NAME_None, const FGuid& GUID = FGuid::New(), EObjectFlags Flags = OF_None)
    {
        return static_cast<T*>(NewObject(T::StaticClass(), Package, Name, GUID, Flags));
    }

    template<Concept::IsACObject T>
    T* NewObject(CClass* InClass, CPackage* Package = nullptr, const FName& Name = NAME_None, const FGuid& GUID = FGuid::New(), EObjectFlags Flags = OF_None)
    {
        return static_cast<T*>(NewObject(InClass, Package, Name, GUID, Flags));
    }

    template<Concept::IsACObject T>
    T* GetMutableDefault()
    {
        return T::StaticClass()->template GetDefaultObject<T>();
    }

    template<Concept::IsACObject T>
    const T* GetDefault()
    {
        return T::StaticClass()->template GetDefaultObject<T>();
    }
    
    enum class EPropertyFlags : uint16
    {
        None                = 0,
        Editable            = BIT(0),
        ReadOnly            = BIT(1),
        NoSerialize         = BIT(2),
        Const               = BIT(3),
        Private             = BIT(4),
        Protected           = BIT(5),
        SubField            = BIT(6),
        Trivial             = BIT(7),
        Builtin             = BIT(9),
        BulkSerialize       = BIT(10),
        // Property exists only for editor tooling. Stripped from cooked
        // packages (see CStruct::SerializeTaggedProperties + FArchive::IsCooking).
        EditorOnly          = BIT(11),
        // Property participates in network replication (PROPERTY(Replicated)). Read by
        // CStruct::NetSerializeProperties; a flag test, not a metadata lookup.
        Replicated          = BIT(12),

        //~ Script (C#) interop specifiers, independent of the editor flags above. Read by the Reflector's
        //  C# binding emitter to shape the generated wrapper member, NOT the editor property grid.
        ScriptReadOnly      = BIT(13), // C# wrapper emits a getter only (no setter), even if editor-editable.
        ScriptWritable      = BIT(14), // C# wrapper emits a setter even if the property is editor ReadOnly/Const.
        ScriptHidden        = BIT(15), // No C# wrapper member is emitted for this property at all.
    };

    ENUM_CLASS_FLAGS(EPropertyFlags);

    /** The reflected property-type taxonomy. Single-sourced from EPropertyTypeFlags.inl so the enum and its
     *  name tables can never drift. Also mirrored by LuminaSharp.EPropertyType (validated at bootstrap) and by
     *  the Reflector's isolated copy in ReflectedType.h/PropertyFlags.h. */
    enum class EPropertyTypeFlags : uint8
    {
        None = 0,
#define LE_PROPERTY_TYPE(Name) Name,
#include "EPropertyTypeFlags.inl"
#undef LE_PROPERTY_TYPE
        Count,
    };

    ENUM_CLASS_FLAGS(EPropertyTypeFlags);

    inline constexpr const char* PropertyTypeFlagNames[] =
    {
        "None",
#define LE_PROPERTY_TYPE(Name) #Name "Property",
#include "EPropertyTypeFlags.inl"
#undef LE_PROPERTY_TYPE
    };

    // Plain enum-member names ("Int8", "Struct", ...), parallel to the enum, for the C#/C++ interop validation
    // (LuminaSharp.EPropertyType checks each name -> value against the native side at bootstrap).
    inline constexpr const char* PropertyTypePlainNames[] =
    {
        "None",
#define LE_PROPERTY_TYPE(Name) #Name,
#include "EPropertyTypeFlags.inl"
#undef LE_PROPERTY_TYPE
    };

    static_assert(std::size(PropertyTypeFlagNames) == (size_t)EPropertyTypeFlags::Count, "PropertyTypeFlagNames must match number of flags in EPropertyTypeFlags");
    static_assert(std::size(PropertyTypePlainNames) == (size_t)EPropertyTypeFlags::Count, "PropertyTypePlainNames must match number of flags in EPropertyTypeFlags");
    
    inline const char* PropertyTypeToString(EPropertyTypeFlags Flag)
    {
        uint16 Index = static_cast<uint16>(Flag);
        return PropertyTypeFlagNames[Index];
    }
    
    RUNTIME_API EPropertyTypeFlags PropertyStringToType(FName String);
    
    RUNTIME_API bool IsValueValidForType(double Value, const FName& TypeName);
    RUNTIME_API bool IsPropertyNumeric(const FName& Type);
    
    template <typename T>
    struct TRegistrationInfo
    {
        using TType = T;
        
        /** Allocates the class memory in stage 1. */
        TType* InnerSingleton = nullptr;

        /** Stage 2: holds property/function reflection initialization. */
        TType* OuterSingleton = nullptr;

    };

    using FStructRegistrationInfo   = TRegistrationInfo<CStruct>;
    using FClassRegistrationInfo    = TRegistrationInfo<CClass>;
    using FEnumRegistrationInfo     = TRegistrationInfo<CEnum>;

    struct FMetaDataPairParam
    {
        const char* NameUTF8;
        const char* ValueUTF8;
    };

    typedef void (*SetterFuncPtr)(void* InContainer, const void* InValue);
    typedef void (*GetterFuncPtr)(const void* InContainer, void* OutValue);

    // The full operation set for a reflected TVector<T> is now a single shared ops table (Containers/
    // ContainerOps.h, also used by C# TVector<T>) rather than per-property hand-rolled fn-ptrs.
    struct FVectorOps;
    struct FMapOps;

    typedef bool (*OptionalHasValuePtr)(const void* InContainer);
    /** Only valid when HasValue returned true. */
    typedef void* (*OptionalGetValuePtr)(void* InContainer);
    /** Engages the optional; copies InValue if non-null, else default-constructs. */
    typedef void (*OptionalSetValuePtr)(void* InContainer, const void* InValue);
    typedef void (*OptionalResetPtr)(void* InContainer);


    struct FPropertyParams
    {
        const char*         Name;
        EPropertyFlags      PropertyFlags;
        EPropertyTypeFlags  TypeFlags;
        SetterFuncPtr       SetterFunc;
        GetterFuncPtr       GetterFunc;
        uint16              Offset;
    };

    struct FNumericPropertyParams : FPropertyParams
    {
        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    struct FStringPropertyParams : FPropertyParams
    {
        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    struct FNamePropertyParams : FPropertyParams
    {
        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    struct FObjectPropertyParams : FPropertyParams
    {
        CClass*                     (*ClassFunc)();

        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    // Soft equivalent of FObjectPropertyParams; ClassFunc returns the target class
    // (T from TSoftObjectPtr<T>, or CObject::StaticClass for bare FSoftObjectPath).
    struct FSoftObjectPropertyParams : FPropertyParams
    {
        CClass*                     (*ClassFunc)();

        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    struct FClassPropertyParams : FPropertyParams
    {
        CClass*                     (*ClassFunc)();

        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };

    struct FStructPropertyParams : FPropertyParams
    {
        CStruct*            (*StructFunc)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    // TSubStructOf<T>: StructFunc returns the base struct T every assignable value must derive from.
    struct FSubStructPropertyParams : FPropertyParams
    {
        CStruct*            (*StructFunc)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    // TInstancedStruct<T>. StructFunc returns the base struct every owned instance must derive from.
    struct FInstancedStructPropertyParams : FPropertyParams
    {
        CStruct*            (*StructFunc)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    // TScriptDelegate<T> event; PayloadStructFunc returns the payload struct (null when no payload).
    struct FDelegatePropertyParams : FPropertyParams
    {
        CStruct*            (*PayloadStructFunc)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FEnumPropertyParams : FPropertyParams
    {
        CEnum*              (*EnumFunc)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FArrayPropertyParams : FPropertyParams
    {
        // Returns the shared element-type ops table (GetVectorOps<T>). One forwarder per array property.
        const FVectorOps* (*GetOpsFn)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FMapPropertyParams : FPropertyParams
    {
        // Returns the shared key/value ops table (GetMapOps<K,V>). One forwarder per map property.
        const FMapOps* (*GetOpsFn)();

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };

    struct FOptionalPropertyParams : FPropertyParams
    {
        OptionalHasValuePtr HasValueFn;
        OptionalGetValuePtr GetValueFn;
        OptionalSetValuePtr SetValueFn;
        OptionalResetPtr    ResetFn;

        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };
    
    struct FClassParams
    {
        CClass*                         (*RegisterFunc)();

        const FPropertyParams* const*   Params;
        uint32                          NumProperties;
        
        uint16                          NumMetaData;
        const FMetaDataPairParam*       MetaDataArray;
    };

    struct FStructParams
    {
        CStruct*                        (*SuperFunc)();
        FStructOps*                     (*StructOpsFn)();
        const char*                     Name;
        const FPropertyParams* const*   Params;
        uint32                          NumProperties;
        uint16                          SizeOf;
        uint16                          AlignOf;
        
        uint16 NumMetaData;
        const FMetaDataPairParam* MetaDataArray;
    };
    
    struct FEnumeratorParam
    {
        const char*               NameUTF8;
        int64                     Value;
    };
    
    struct FEnumParams
    {
        const char*                 Name;
        const FEnumeratorParam*     Params;
        int16                       NumParams;
        
        uint16                      NumMetaData;
        const FMetaDataPairParam*   MetaDataArray;
    };
    

    

    RUNTIME_API void ConstructCClass(CClass** OutClass, const FClassParams& Params);
    RUNTIME_API void ConstructCEnum(CEnum** OutEnum, const FEnumParams& Params);
    RUNTIME_API void ConstructCStruct(CStruct** OutStruct, const FStructParams& Params);
    
}
