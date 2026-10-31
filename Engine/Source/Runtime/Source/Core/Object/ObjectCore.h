#pragma once

#include "ConstructObjectParams.h"
#include "ObjectFlags.h"
#include "PropertyFlags.h"
#include "Core/Reflection/ReflectionDesc.h"
#include "Containers/Vector.h"
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
    class FProperty;
    struct FPropertyOwner;
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

    // Seeds the new object's reflected properties from Template rather than from the class CDO.
    RUNTIME_API CObject* NewObjectFromTemplate(const CObject* Template, CPackage* Package = nullptr, const FName& Name = NAME_None, const FGuid& GUID = FGuid::New(), EObjectFlags Flags = OF_None);

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
    T* NewObjectFromTemplate(const T* Template, CPackage* Package = nullptr, const FName& Name = NAME_None, const FGuid& GUID = FGuid::New(), EObjectFlags Flags = OF_None)
    {
        return static_cast<T*>(NewObjectFromTemplate(static_cast<const CObject*>(Template), Package, Name, GUID, Flags));
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
    

    // The kind a name spells, for a file that stored types as text, None if it spells nothing.
    RUNTIME_API EPropertyTypeFlags PropertyTypeFromName(const FName& TypeName);

    // Whether Value survives a conversion into Type, which is what gates a numeric property's migration.
    RUNTIME_API bool IsValueValidForType(double Value, EPropertyTypeFlags Type);
    RUNTIME_API bool IsPropertyNumeric(EPropertyTypeFlags Type);

    // A number held at the width it was read, so converting between integer kinds never rounds through a double.
    struct FNumericValue
    {
        enum class EKind : uint8 { Signed, Unsigned, Float };

        EKind   Kind = EKind::Signed;
        int64   Signed = 0;
        uint64  Unsigned = 0;
        double  Float = 0.0;
    };

    // Bytes a numeric kind occupies, or zero for anything else.
    NODISCARD RUNTIME_API uint32 GetNumericSize(EPropertyTypeFlags Type);
    RUNTIME_API bool ReadNumber(EPropertyTypeFlags Type, const void* Memory, FNumericValue& Out);

    // False, leaving Memory untouched, when the value does not fit Type.
    RUNTIME_API bool WriteNumber(EPropertyTypeFlags Type, void* Memory, const FNumericValue& Value);

    // Builds the property and every inner its type describes, attached wherever Owner says.
    RUNTIME_API FProperty* ConstructProperty(const FPropertyOwner& Owner, const FPropertyDesc& Desc);

    RUNTIME_API void InitializeAndCreateFProperties(CStruct* Outer, const FPropertyDesc* Properties, uint32 NumProperties);
    RUNTIME_API void InitializeAndCreateFFunctions(CStruct* Outer, const FFunctionDesc* Functions, uint32 NumFunctions);

    RUNTIME_API void ConstructCClass(CClass** OutClass, const FClassDesc& Desc);
    RUNTIME_API void ConstructCEnum(CEnum** OutEnum, const FEnumDesc& Desc);
    RUNTIME_API void ConstructCStruct(CStruct** OutStruct, const FStructDesc& Desc);
    
}
