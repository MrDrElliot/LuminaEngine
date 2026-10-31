#include "RuntimePCH.h"
#include "Containers/Algorithm.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <type_traits>

#include "ObjectCore.h"
#include "FileSystem/FileSystem.h"
#include "Class.h"
#include "Object.h"
#include "Cast.h"
#include "ObjectAllocator.h"
#include "ScriptClass.h"
#include "ObjectHash.h"
#include "ObjectIterator.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/PropertyRegistry.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/DelegateProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/StringProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Package/Package.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"
#include "Log/Log.h"

namespace Lumina
{
    /** Raw allocation only; does not construct. */
    static void* AllocateCObjectMemory(const CClass* InClass, EObjectFlags InFlags)
    {
        uint32 Alignment = Math::Max<uint32>(16, InClass->GetAlignment());

        return GCObjectAllocator.AllocateCObject(InClass->GetSize(), Alignment);
    }

    // Reuses the class name's table entry, since looking the string up again cost more than the rest of the name.
    static FName MakeUniqueObjectName(CClass* Class)
    {
        const int32 Unique = std::atomic_ref<int32>(Class->ClassUnique).fetch_add(1, std::memory_order_relaxed) + 1;
        const FName ClassName = Class->GetName();
        return ClassName.HasNumber() ? FName(ClassName.c_str(), (uint32)Unique) : FName::FromIndexAndNumber(ClassName.GetID(), (uint32)Unique);
    }

    /** Seeds a new object's reflected properties from its archetype, which is the class CDO unless one was named. */
    static void InitPropertiesFromTemplate(CObject* Object, const FConstructCObjectParams& Params)
    {
        // A default object is where defaults come from, so it keeps what its own constructor wrote.
        if (EnumHasAnyFlags(Params.Flags, OF_DefaultObject))
        {
            return;
        }

        const TSpan<FProperty* const> Properties = Params.Class->GetProperties();
        if (Properties.empty())
        {
            return;
        }

        const CObject* Template = Params.Template;

        if (Template == nullptr)
        {
            Template = Params.Class->GetDefaultObject();
        }
        // Class's properties sit at the same offsets in a subclass, so only a narrower template reads in bounds.
        else if (Template->GetClass() == nullptr || !Template->GetClass()->IsChildOf(Params.Class))
        {
            LOG_ERROR("Template '{}' is not a '{}', so it cannot seed one; the new object keeps its constructor values.",
                Template->GetName(), Params.Class->GetName());
            return;
        }

        if (Template == nullptr || Template == Object)
        {
            return;
        }

        Params.Class->CopyProperties(Object, Template);
    }

    CObject* StaticAllocateObject(const FConstructCObjectParams& Params)
    {
        LUMINA_PROFILE_SCOPE();

        void* ObjectMemory = AllocateCObjectMemory(Params.Class, Params.Flags);
        Memory::Memzero(ObjectMemory, Params.Class->GetSize());

        CObject* NewObject = nullptr;
        {
            // Scoped around the constructor alone, which is the only thing that reads it.
            FScopedObjectConstruction Constructing(Params);
            NewObject = Params.Class->EmplaceInstance(ObjectMemory);
        }

        NewObject->AddObject();

        // Only a runtime-minted class has a trailing block, so the cast is the test. The flag is what lets
        // the destructor skip its class unless there is trailing storage.
        if (const CScriptClass* ScriptClass = ToScriptClass(Params.Class);
            ScriptClass != nullptr && ScriptClass->ConstructScriptProperties(NewObject))
        {
            NewObject->SetFlag(OF_ScriptProperties);
        }

        // After the script block, whose properties are raw storage until ConstructScriptProperties builds them.
        InitPropertiesFromTemplate(NewObject, Params);

        NewObject->PostInitProperties();

        return NewObject;
    }

    CObject* FindObjectImpl(const FGuid& ObjectGUID)
    {
        return static_cast<CObject*>(FObjectHashTables::Get().FindObject(ObjectGUID));
    }

    CObject* FindObjectImpl(const FName& Name, CClass* Class)
    {
        return static_cast<CObject*>(FObjectHashTables::Get().FindObject(Name, Class));
    }

    CObject* StaticLoadObject(const FGuid& GUID)
    {
        LUMINA_PROFILE_SCOPE();

        // Transient-package objects have stable GUIDs but no registry entry; in-memory lookup is the only path.
        if (CObject* Existing = FindObjectImpl(GUID))
        {
            return Existing;
        }

        if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(GUID))
        {
            if (CPackage* Package = CPackage::LoadPackage(Data->Path))
            {
                return Package->LoadObject(GUID);
            }
        }

        return nullptr;
    }

    CObject* StaticLoadObject(FStringView Name)
    {
        if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Name))
        {
            return StaticLoadObject(Data->AssetGUID);
        }

        return nullptr;
    }

    CObject* StaticLoadObjectGraph(const FGuid& GUID)
    {
        return CPackage::LoadAssetGraph(GUID);
    }

    CObject* StaticLoadObjectGraph(FStringView Name)
    {
        if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Name))
        {
            return CPackage::LoadAssetGraph(Data->AssetGUID);
        }

        // Not a registered asset, so there is nothing to fan out and the plain inline load stands.
        return StaticLoadObject(Name);
    }

    FFixedString SanitizeObjectName(FStringView Name)
    {
        // Only the package extension, since a dot in a folder or file name is part of the name and a second pass must change nothing.
        constexpr FStringView PackageExt = ".lasset";
        if (VFS::HasExtension(Name, PackageExt))
        {
            Name = Name.substr(0, Name.size() - PackageExt.size());
        }

        FFixedString Result;
        Result.reserve(Name.size());

        for (char c : Name)
        {
            const bool bInvalid =
                c < 32 ||
                c == '<' || c == '>' ||
                c == ':' || c == '"' ||
                c == '|' || c == '?' ||
                c == '*';

            Result.push_back(bInvalid ? '_' : c);
        }

        while (!Result.empty() && (Result.back() == ' ' || Result.back() == '.'))
        {
            Result.pop_back();
        }

        if (Result.empty())
        {
            Result = "Object";
        }

        return Result;
    }

    // Lifetime only.
    bool IsValid(const CObjectBase* Obj)
    {
        return Obj != nullptr && !Obj->HasAnyFlag(OF_MarkedDestroy);
    }

    bool IsValid(CObjectBase* Obj)
    {
        return IsValid(static_cast<const CObjectBase*>(Obj));
    }
    
    CObject* NewObject(CClass* InClass, CPackage* Package, const FName& Name, const FGuid& GUID, EObjectFlags Flags)
    {
        if (InClass != nullptr && InClass->IsAbstract())
        {
            LOG_ERROR("NewObject: '{}' is abstract, so only its subclasses can be created.", InClass->GetName().c_str());
            return nullptr;
        }
        if (InClass != nullptr && InClass->IsSuperseded())
        {
            LOG_WARN("NewObject: '{}' was replaced or removed by a script reload, so nothing new is built on it.", InClass->GetName().c_str());
            return nullptr;
        }

        FConstructCObjectParams Params(InClass);

        Params.Name = (Name == NAME_None) ? MakeUniqueObjectName(InClass) : Name;

        
        Params.Guid = GUID;
        Params.Flags = Flags;
        Params.Package = Package;

        return StaticAllocateObject(Params);
    }

    CObject* NewObjectFromTemplate(const CObject* Template, CPackage* Package, const FName& Name, const FGuid& GUID, EObjectFlags Flags)
    {
        if (Template == nullptr || Template->GetClass() == nullptr)
        {
            return nullptr;
        }

        FConstructCObjectParams Params(Template->GetClass());

        Params.Name = (Name == NAME_None) ? MakeUniqueObjectName(Template->GetClass()) : Name;

        Params.Guid     = GUID;
        Params.Flags    = Flags;
        Params.Package  = Package;
        Params.Template = Template;

        return StaticAllocateObject(Params);
    }

    void GetObjectsWithPackage(const CPackage* Package, TVector<CObject*>& OutObjects)
    {
        ASSERT(Package != nullptr);
        
        for (TObjectIterator<CObject> It; It; ++It)
        {
            CObjectBase* Object = *It;
            if (Object && Object->GetPackage() == Package)
            {
                OutObjects.push_back(static_cast<CObject*>(Object));
            }
        }
    }

    EPropertyTypeFlags PropertyTypeFromName(const FName& TypeName)
    {
        // only reached by a pre-PACKAGE_NAME_TABLE load, so the table is built on the first such file
        struct FNames
        {
            FNames()
            {
                for (size_t Index = 0; Index < std::size(Entries); ++Index)
                {
                    Entries[Index] = FName(PropertyTypeFlagNames[Index]);
                }
            }

            FName Entries[(size_t)EPropertyTypeFlags::Count];
        };
        static const FNames Table;

        for (size_t Index = 0; Index < std::size(Table.Entries); ++Index)
        {
            if (Table.Entries[Index] == TypeName)
            {
                return (EPropertyTypeFlags)Index;
            }
        }

        return EPropertyTypeFlags::None;
    }

    bool IsValueValidForType(double Value, EPropertyTypeFlags Type)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:   return Value >= INT8_MIN && Value <= INT8_MAX;
        case EPropertyTypeFlags::Int16:  return Value >= INT16_MIN && Value <= INT16_MAX;
        case EPropertyTypeFlags::Int32:  return Value >= INT32_MIN && Value <= INT32_MAX;
        case EPropertyTypeFlags::Int64:  return Value >= (double)INT64_MIN && Value < 9223372036854775808.0;
        case EPropertyTypeFlags::UInt8:  return Value >= 0 && Value <= UINT8_MAX;
        case EPropertyTypeFlags::UInt16: return Value >= 0 && Value <= UINT16_MAX;
        case EPropertyTypeFlags::UInt32: return Value >= 0 && Value <= UINT32_MAX;
        case EPropertyTypeFlags::UInt64: return Value >= 0 && Value < 18446744073709551616.0;
        case EPropertyTypeFlags::Float:
        case EPropertyTypeFlags::Double: return true;
        default:                         return false;
        }
    }

    namespace ObjectCoreInternal
    {
        template<typename T>
        bool StoreNumber(void* Memory, const FNumericValue& Value)
        {
            if constexpr (std::is_floating_point_v<T>)
            {
                const double Wide = Value.Kind == FNumericValue::EKind::Float ? Value.Float
                                  : Value.Kind == FNumericValue::EKind::Signed ? static_cast<double>(Value.Signed)
                                  : static_cast<double>(Value.Unsigned);
                *static_cast<T*>(Memory) = static_cast<T>(Wide);
                return true;
            }
            else
            {
                using FLimits = std::numeric_limits<T>;
                if (Value.Kind == FNumericValue::EKind::Signed)
                {
                    if constexpr (std::is_signed_v<T>)
                    {
                        if (Value.Signed < static_cast<int64>(FLimits::min()) || Value.Signed > static_cast<int64>(FLimits::max()))
                        {
                            return false;
                        }
                    }
                    else if (Value.Signed < 0 || static_cast<uint64>(Value.Signed) > static_cast<uint64>(FLimits::max()))
                    {
                        return false;
                    }
                    *static_cast<T*>(Memory) = static_cast<T>(Value.Signed);
                    return true;
                }

                if (Value.Kind == FNumericValue::EKind::Unsigned)
                {
                    if (Value.Unsigned > static_cast<uint64>(FLimits::max()))
                    {
                        return false;
                    }
                    *static_cast<T*>(Memory) = static_cast<T>(Value.Unsigned);
                    return true;
                }

                // Bounds are powers of two, which a double holds exactly, unlike the limits themselves at 64 bits.
                const double Whole = std::trunc(Value.Float);
                const double Upper = std::ldexp(1.0, FLimits::digits);
                const double Lower = std::is_signed_v<T> ? -Upper : 0.0;
                if (!std::isfinite(Whole) || Whole < Lower || Whole >= Upper)
                {
                    return false;
                }
                *static_cast<T*>(Memory) = static_cast<T>(Whole);
                return true;
            }
        }
    }

    uint32 GetNumericSize(EPropertyTypeFlags Type)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::UInt8:  return 1;
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::UInt16: return 2;
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::Float:  return 4;
        case EPropertyTypeFlags::Int64:
        case EPropertyTypeFlags::UInt64:
        case EPropertyTypeFlags::Double: return 8;
        default:                         return 0;
        }
    }

    bool ReadNumber(EPropertyTypeFlags Type, const void* Memory, FNumericValue& Out)
    {
        using EKind = FNumericValue::EKind;
        Out = FNumericValue();
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:   Out.Kind = EKind::Signed;   Out.Signed = *static_cast<const int8*>(Memory);    return true;
        case EPropertyTypeFlags::Int16:  Out.Kind = EKind::Signed;   Out.Signed = *static_cast<const int16*>(Memory);   return true;
        case EPropertyTypeFlags::Int32:  Out.Kind = EKind::Signed;   Out.Signed = *static_cast<const int32*>(Memory);   return true;
        case EPropertyTypeFlags::Int64:  Out.Kind = EKind::Signed;   Out.Signed = *static_cast<const int64*>(Memory);   return true;
        case EPropertyTypeFlags::UInt8:  Out.Kind = EKind::Unsigned; Out.Unsigned = *static_cast<const uint8*>(Memory);  return true;
        case EPropertyTypeFlags::UInt16: Out.Kind = EKind::Unsigned; Out.Unsigned = *static_cast<const uint16*>(Memory); return true;
        case EPropertyTypeFlags::UInt32: Out.Kind = EKind::Unsigned; Out.Unsigned = *static_cast<const uint32*>(Memory); return true;
        case EPropertyTypeFlags::UInt64: Out.Kind = EKind::Unsigned; Out.Unsigned = *static_cast<const uint64*>(Memory); return true;
        case EPropertyTypeFlags::Float:  Out.Kind = EKind::Float;    Out.Float = *static_cast<const float*>(Memory);     return true;
        case EPropertyTypeFlags::Double: Out.Kind = EKind::Float;    Out.Float = *static_cast<const double*>(Memory);    return true;
        default:                         return false;
        }
    }

    bool WriteNumber(EPropertyTypeFlags Type, void* Memory, const FNumericValue& Value)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:   return ObjectCoreInternal::StoreNumber<int8>(Memory, Value);
        case EPropertyTypeFlags::Int16:  return ObjectCoreInternal::StoreNumber<int16>(Memory, Value);
        case EPropertyTypeFlags::Int32:  return ObjectCoreInternal::StoreNumber<int32>(Memory, Value);
        case EPropertyTypeFlags::Int64:  return ObjectCoreInternal::StoreNumber<int64>(Memory, Value);
        case EPropertyTypeFlags::UInt8:  return ObjectCoreInternal::StoreNumber<uint8>(Memory, Value);
        case EPropertyTypeFlags::UInt16: return ObjectCoreInternal::StoreNumber<uint16>(Memory, Value);
        case EPropertyTypeFlags::UInt32: return ObjectCoreInternal::StoreNumber<uint32>(Memory, Value);
        case EPropertyTypeFlags::UInt64: return ObjectCoreInternal::StoreNumber<uint64>(Memory, Value);
        case EPropertyTypeFlags::Float:  return ObjectCoreInternal::StoreNumber<float>(Memory, Value);
        case EPropertyTypeFlags::Double: return ObjectCoreInternal::StoreNumber<double>(Memory, Value);
        default:                         return false;
        }
    }

    bool IsPropertyNumeric(EPropertyTypeFlags Type)
    {
        switch (Type)
        {
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::Int64:
        case EPropertyTypeFlags::UInt8:
        case EPropertyTypeFlags::UInt16:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::UInt64:
        case EPropertyTypeFlags::Float:
        case EPropertyTypeFlags::Double: return true;
        default:                         return false;
        }
    }

    // Sized from the widest property, so one type's whole set lands in a single block; an undershoot only
    // costs another block, never a move, so nothing that already points into the arena is disturbed.
    static constexpr size_t kArenaBytesPerProperty = 192;

    // The function itself plus the pointer array for its parameters, which are counted as properties above.
    static constexpr size_t kArenaBytesPerFunction = 128;

    namespace
    {
        const char* InnerSuffix(EPropertyTypeFlags Kind, uint8 Index)
        {
            constexpr const char* DelegateArgs[] = { "_Arg0", "_Arg1", "_Arg2", "_Arg3", "_Arg4", "_Arg5" };
            switch (Kind)
            {
            case EPropertyTypeFlags::Map:      return Index == 0 ? "_KeyInner" : "_ValueInner";
            case EPropertyTypeFlags::Delegate: return DelegateArgs[Index];
            default:                           return "_Inner";
            }
        }

        // An inner shares its owner's storage, except a delegate argument, which sits in the broadcast pack.
        const FPropertyDesc& MakeInnerDesc(FPropertyArena& Arena, const FPropertyDesc& Owner, uint8 Index)
        {
            const FPropertyType& OwnerType = *Owner.Type;

            FString Name(Owner.Name);
            Name += InnerSuffix(OwnerType.Kind, Index);

            FPropertyDesc* Inner = Arena.EmplaceData<FPropertyDesc>();
            Inner->Name   = Arena.CopyString(Name);
            Inner->Offset = OwnerType.InnerOffsets != nullptr ? OwnerType.InnerOffsets[Index] : 0;
            Inner->Flags  = EPropertyFlags::SubField;
            Inner->Type   = OwnerType.Inner[Index];
            return *Inner;
        }
    }

    FProperty* ConstructProperty(const FPropertyOwner& Owner, const FPropertyDesc& Desc)
    {
        const FPropertyKindOps& Ops = GetPropertyKindOps(Desc.Type->Kind);
        if (!Ops.IsValid())
        {
            LOG_CRITICAL("Unsupported property type found while creating: {}", Desc.Name);
            return nullptr;
        }

        FProperty* Property = Ops.Construct(Owner, Desc);

#if USING(WITH_EDITOR)
        // Only the editor labels a property, and an inner is drawn under its owner's label.
        if (!EnumHasAnyFlags(Desc.Flags, EPropertyFlags::SubField))
        {
            Property->OnMetadataFinalized(*Owner.Arena);
        }
#endif

        for (uint8 Index = 0; Index < Desc.Type->NumInner; ++Index)
        {
            ConstructProperty(Owner.Inner(Property), MakeInnerDesc(*Owner.Arena, Desc, Index));
        }

        // The enum only learns its width from the integer inner, which is attached after it.
        if (Desc.Type->Kind == EPropertyTypeFlags::Enum)
        {
            FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(Property);
            if (const FNumericProperty* Inner = EnumProperty->GetInnerProperty())
            {
                EnumProperty->SetElementSize(Inner->GetElementSize());
            }
        }

        return Property;
    }

    void InitializeAndCreateFProperties(CStruct* Outer, const FPropertyDesc* Properties, uint32 NumProperties)
    {
        FPropertyArena& Arena = Outer->GetPropertyArena();
        Arena.Reserve(NumProperties * kArenaBytesPerProperty);

        // Declaration order, since AddProperty appends. Packages written in the old reversed order still load by name.
        const FPropertyOwner Owner{ &Arena, Outer, nullptr };
        for (uint32 Index = 0; Index < NumProperties; ++Index)
        {
            ConstructProperty(Owner, Properties[Index]);
        }
    }

    void InitializeAndCreateFFunctions(CStruct* Outer, const FFunctionDesc* Functions, uint32 NumFunctions)
    {
        if (NumFunctions == 0)
        {
            return;
        }

        FPropertyArena& Arena = Outer->GetPropertyArena();
        Arena.Reserve(NumFunctions * kArenaBytesPerFunction);

        TVector<FProperty*> Params;
        for (uint32 FunctionIndex = 0; FunctionIndex < NumFunctions; ++FunctionIndex)
        {
            const FFunctionDesc& Desc = Functions[FunctionIndex];

            Params.clear();
            const FPropertyOwner Owner{ &Arena, nullptr, nullptr, &Params };
            for (uint16 ParamIndex = 0; ParamIndex < Desc.NumParams; ++ParamIndex)
            {
                ConstructProperty(Owner, Desc.Params[ParamIndex]);
            }

            if (FFunction* Function = FFunctionBuilder::Build(Arena, Outer, Desc, Params))
            {
                Outer->AddFunction(Function);
            }
        }
    }

    static CPackage* FindOrCreatePackage(const TCHAR* PackageName)
    {
        CPackage* Package = nullptr;
        if (PackageName && PackageName[0] != '\0')
        {
            Package = FindObject<CPackage>(PackageName);
            if (Package == nullptr)
            {
                Package = NewObject<CPackage>(nullptr, PackageName);
            }
        }

        return Package;
    }
    
    namespace
    {
        void AddTypeMetadata(CField* Type, const FMetadataEntry* Entries, uint16 Count)
        {
            for (uint16 Index = 0; Index < Count; ++Index)
            {
                Type->Metadata.AddValue(Entries[Index].Key, Entries[Index].Value);
            }
        }
    }

    void ConstructCClass(CClass** OutClass, const FClassDesc& Desc)
    {
        CClass*& FinalClass = *OutClass;
        if (FinalClass != nullptr)
        {
            return;
        }

        FinalClass = Desc.StaticClass();

        InitializeAndCreateFProperties(FinalClass, Desc.Properties, Desc.NumProperties);
        InitializeAndCreateFFunctions(FinalClass, Desc.Functions, Desc.NumFunctions);
        AddTypeMetadata(FinalClass, Desc.Metadata, Desc.NumMetadata);

        // Last, so a walk of the object array never reaches a class whose properties are still arriving.
        FinishRegistration(FinalClass);
    }

    void ConstructCEnum(CEnum** OutEnum, const FEnumDesc& Desc)
    {
        FConstructCObjectParams ObjectParms(CEnum::StaticClass());
        ObjectParms.Name        = Desc.Name;
        ObjectParms.Flags       = OF_None;
        ObjectParms.Package     = FindOrCreatePackage(CEnum::StaticPackage());
        ObjectParms.Guid        = FGuid::New();

        CEnum* NewEnum = (CEnum*)StaticAllocateObject(ObjectParms);
        *OutEnum = NewEnum;

        AddTypeMetadata(NewEnum, Desc.Metadata, Desc.NumMetadata);
        for (uint32 Index = 0; Index < Desc.NumEnumerators; ++Index)
        {
            NewEnum->AddEnum(Desc.Enumerators[Index].Name, Desc.Enumerators[Index].Value);
        }

        NewEnum->AddToRoot();
    }

    void ConstructCStruct(CStruct** OutStruct, const FStructDesc& Desc)
    {
        FConstructCObjectParams ObjectParms(CStruct::StaticClass());
        ObjectParms.Name        = Desc.Name;
        ObjectParms.Flags       = OF_None;
        ObjectParms.Package     = FindOrCreatePackage(CStruct::StaticPackage());
        ObjectParms.Guid        = FGuid::New();

        CStruct* FinalStruct = (CStruct*)StaticAllocateObject(ObjectParms);
        FinalStruct->Size = Desc.Size;
        FinalStruct->Alignment = Desc.Alignment;
        FinalStruct->StructOps.reset(Desc.MakeOps());
        if (FinalStruct->StructOps == nullptr)
        {
            FinalStruct->StructOps.reset(Memory::New<FStructOps>());
        }

        *OutStruct = FinalStruct;

        InitializeAndCreateFProperties(FinalStruct, Desc.Properties, Desc.NumProperties);
        InitializeAndCreateFFunctions(FinalStruct, Desc.Functions, Desc.NumFunctions);
        AddTypeMetadata(FinalStruct, Desc.Metadata, Desc.NumMetadata);

        if (Desc.Super != nullptr)
        {
            FinalStruct->SetSuperStruct(Desc.Super());
        }

        FinalStruct->Link();
        FinalStruct->AddToRoot();
    }
}
