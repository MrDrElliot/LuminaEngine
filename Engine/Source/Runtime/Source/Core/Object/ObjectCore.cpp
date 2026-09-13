#include "RuntimePCH.h"
#include "Containers/Algorithm.h"

#include <atomic>

#include "ObjectCore.h"
#include "Class.h"
#include "Object.h"
#include "Cast.h"
#include "ObjectAllocator.h"
#include "ScriptClass.h"
#include "ObjectHash.h"
#include "ObjectIterator.h"
#include "Assets/AssetManager/AssetManager.h"
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

    CObject* StaticAllocateObject(const FConstructCObjectParams& Params)
    {
        LUMINA_PROFILE_SCOPE();
        
        void* ObjectMemory = AllocateCObjectMemory(Params.Class, Params.Flags);
        Memory::Memzero(ObjectMemory, Params.Class->GetSize());
        
        CObject* NewObject = Params.Class->EmplaceInstance(ObjectMemory);

        NewObject->ConstructInternal(FObjectInitializer(Params.Package, Params));

        // Only a runtime-minted class has a trailing block, so the cast is the test. The flag is what lets
        // the destructor skip its class unless there is trailing storage.
        if (const CScriptClass* ScriptClass = ToScriptClass(Params.Class);
            ScriptClass != nullptr && ScriptClass->ConstructScriptProperties(NewObject))
        {
            NewObject->SetFlag(OF_ScriptProperties);
        }

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

    void AsyncLoadObject(const FGuid& GUID, const TFunction<void(CObject*)>& Callback)
    {
        Task::AsyncTask(1, 1, [GUID, Callback](uint32, uint32, uint32)
        {
            if (CObject* Existing = FindObjectImpl(GUID))
            {
                MainThread::Enqueue([Existing, Callback]
                {
                    Callback(Existing);
                });
                return;
            }

            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(GUID))
            {
                if (CPackage* Package = CPackage::LoadPackage(Data->Path))
                {
                    if (CObject* Object = Package->LoadObject(GUID))
                    {
                        MainThread::Enqueue([Object, Callback]
                        {
                            Callback(Object);
                        });
                    }
                }
            }
        });
    }

    void AsyncLoadObject(const FName& Name, const TFunction<void(CObject*)>& Callback)
    {
        Task::AsyncTask(1, 1, [Name, Callback](uint32, uint32, uint32)
        {
            if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByPath(Name.c_str()))
            {
                if (CPackage* Package = CPackage::LoadPackage(Data->Path))
                {
                    if (CObject* Object = Package->LoadObject(Data->AssetGUID))
                    {
                        MainThread::Enqueue([Object, Callback]
                        {
                            Callback(Object);
                        });
                    }
                }
            }
        });
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
        const size_t ExtPos = Name.find_last_of('.');
        if (ExtPos != FString::npos)
        {
            Name = Name.substr(0, ExtPos);
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
        FConstructCObjectParams Params(InClass);

        if (Name == NAME_None)
        {
            int32 Unique = std::atomic_ref<int32>(InClass->ClassUnique).fetch_add(1, std::memory_order_relaxed) + 1;
            Params.Name = FName(InClass->GetName().c_str(), Unique);
        }
        else
        {
            Params.Name = Name;
        }

        
        Params.Guid = GUID;
        Params.Flags = Flags;
        Params.Package = Package;

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
        case EPropertyTypeFlags::Int64:  return Value >= (double)INT64_MIN && Value <= (double)INT64_MAX;
        case EPropertyTypeFlags::UInt8:  return Value >= 0 && Value <= UINT8_MAX;
        case EPropertyTypeFlags::UInt16: return Value >= 0 && Value <= UINT16_MAX;
        case EPropertyTypeFlags::UInt32: return Value >= 0 && Value <= UINT32_MAX;
        case EPropertyTypeFlags::UInt64: return Value >= 0 && Value <= (double)UINT64_MAX;
        case EPropertyTypeFlags::Float:
        case EPropertyTypeFlags::Double: return true;
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

    // The emitter's table is a static constexpr array that outlives the binary, so it is pointed at, not copied.
    static void ConstructPropertyMetadata(FPropertyArena& Arena, FProperty* NewProperty, uint16 NumMetadata, const FMetaDataPairParam* ParamArray)
    {
        NewProperty->SetMetadata(ParamArray, NumMetadata);
        NewProperty->OnMetadataFinalized(Arena);
    }

    // Sized from the widest property, so one type's whole set lands in a single block; an undershoot only
    // costs another block, never a move, so nothing that already points into the arena is disturbed.
    static constexpr size_t kArenaBytesPerProperty = 192;

    // The function itself plus the pointer array for its parameters; the parameters are properties and
    // already counted through the reservation above.
    static constexpr size_t kArenaBytesPerFunction = 128;

    void ConstructProperties(const FPropertyOwner& Owner, const FPropertyParams* const*& Properties, uint32& NumProperties)
    {
        const FPropertyParams* Param = *--Properties;
        const FPropertyKindOps& Ops = GetPropertyKindOps(Param->TypeFlags);

        FProperty* NewProperty = nullptr;
        if (!Ops.IsValid())
        {
            LOG_CRITICAL("Unsupported property type found while creating: {}", Param->Name);
        }
        else
        {
            NewProperty = Ops.Construct(Owner, Param);

            uint16 NumMetaData = 0;
            const FMetaDataPairParam* MetaDataArray = nullptr;
            Ops.GetMetadata(Param, NumMetaData, MetaDataArray);
            if (NumMetaData != 0)
            {
                ConstructPropertyMetadata(*Owner.Arena, NewProperty, NumMetaData, MetaDataArray);
            }
        }

        --NumProperties;

        // An inner is the next params entry, and the kind is what says how many of them to expect.
        for (uint8 Remaining = Ops.NumInnerParams; Remaining != 0 && NewProperty != nullptr; --Remaining)
        {
            ConstructProperties(Owner.Inner(NewProperty), Properties, NumProperties);
        }

        // The inner only gets its own size after it has already been attached to the enum property.
        if (Param->TypeFlags == EPropertyTypeFlags::Enum && NewProperty != nullptr)
        {
            FEnumProperty* EnumProperty = static_cast<FEnumProperty*>(NewProperty);
            if (const FNumericProperty* Inner = EnumProperty->GetInnerProperty())
            {
                EnumProperty->SetElementSize(Inner->GetElementSize());
            }
        }
    }

    void InitializeAndCreateFProperties(CStruct* Outer, const FPropertyParams* const* PropertyArray, uint32 NumProperties)
    {
        FPropertyArena& Arena = Outer->GetPropertyArena();
        Arena.Reserve(NumProperties * kArenaBytesPerProperty);

        const FPropertyOwner Owner{ &Arena, Outer, nullptr };

        // Iterates backwards.
        PropertyArray += NumProperties;
        while (NumProperties)
        {
            ConstructProperties(Owner, PropertyArray, NumProperties);
        }
    }

    void InitializeAndCreateFFunctions(CStruct* Outer, const FFunctionParams* const* FunctionArray, uint32 NumFunctions)
    {
        if (NumFunctions == 0)
        {
            return;
        }

        FPropertyArena& Arena = Outer->GetPropertyArena();
        Arena.Reserve(NumFunctions * kArenaBytesPerFunction);

        TVector<FProperty*> Collected;
        TVector<FProperty*> Ordered;

        for (uint32 Index = 0; Index < NumFunctions; ++Index)
        {
            const FFunctionParams& Params = *FunctionArray[Index];

            Ordered.clear();

            // One call consumes exactly one top-level parameter plus its inners, and only the top-level one
            // reaches the collector, so taking them one at a time is what makes the argument order exact
            // rather than inferred from where inners happen to sit.
            const FPropertyParams* const* Walk = Params.Params + Params.NumParamEntries;
            uint32 Remaining = Params.NumParamEntries;
            while (Remaining != 0)
            {
                Collected.clear();
                const FPropertyOwner Owner{ &Arena, nullptr, nullptr, &Collected };
                ConstructProperties(Owner, Walk, Remaining);

                if (Collected.size() != 1)
                {
                    LOG_CRITICAL("Reflected function '{}' produced {} top-level parameters from one entry",
                        Params.Name, Collected.size());
                    continue;
                }

                Ordered.push_back(Collected[0]);
            }

            // The walk runs from the end of the array, so reversing lands declaration order.
            Algo::Reverse(Ordered);

            FFunction* Function = FFunctionBuilder::Build(Arena, Outer, Params, Ordered);
            if (Function != nullptr)
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
    
    void ConstructCClass(CClass** OutClass, const FClassParams& Params)
    {
        CClass*& FinalClass = *OutClass;
        if (FinalClass != nullptr)
        {
            return;
        }
        
        FinalClass = Params.RegisterFunc();

        CObjectForceRegistration(FinalClass);
        
        InitializeAndCreateFProperties(FinalClass, Params.Params, Params.NumProperties);
        InitializeAndCreateFFunctions(FinalClass, Params.Functions, Params.NumFunctions);

        for (uint16 i = 0; i < Params.NumMetaData; ++i)
        {
            const FMetaDataPairParam& Param = Params.MetaDataArray[i];
            FinalClass->Metadata.AddValue(Param.NameUTF8, Param.ValueUTF8);
        }
    }

    void ConstructCEnum(CEnum** OutEnum, const FEnumParams& Params)
    {
        FConstructCObjectParams ObjectParms(CEnum::StaticClass());
        ObjectParms.Name        = Params.Name;
        ObjectParms.Flags       = OF_None;
        ObjectParms.Package     = FindOrCreatePackage(CEnum::StaticPackage());
        ObjectParms.Guid        = FGuid::New();

        
        CEnum* NewEnum = (CEnum*)StaticAllocateObject(ObjectParms);
        
        *OutEnum = NewEnum;
        
        for (uint16 i = 0; i < Params.NumMetaData; ++i)
        {
            const FMetaDataPairParam& Param = Params.MetaDataArray[i];
            NewEnum->Metadata.AddValue(Param.NameUTF8, Param.ValueUTF8);
        }
        
        for (int16 i = 0; i < Params.NumParams; i++)
        {
            const FEnumeratorParam* Param = &Params.Params[i];
            NewEnum->AddEnum(Param->NameUTF8, Param->Value);
        }

        NewEnum->AddToRoot();
    }
    
    void ConstructCStruct(CStruct** OutStruct, const FStructParams& Params)
    {
        FConstructCObjectParams ObjectParms(CStruct::StaticClass());
        ObjectParms.Name        = Params.Name;
        ObjectParms.Flags       = OF_None;
        ObjectParms.Package     = FindOrCreatePackage(CStruct::StaticPackage());
        ObjectParms.Guid        = FGuid::New();
        
        CStruct* FinalClass = (CStruct*)StaticAllocateObject(ObjectParms);
        FinalClass->Size = Params.SizeOf;
        FinalClass->Alignment = Params.AlignOf;
        FinalClass->StructOps.reset(Params.StructOpsFn());
        if (FinalClass->StructOps == nullptr)
        {
            FinalClass->StructOps.reset(Memory::New<FStructOps>());
        }
        
        
        *OutStruct = FinalClass;
        
        CObjectForceRegistration(FinalClass);
        
        InitializeAndCreateFProperties(FinalClass, Params.Params, Params.NumProperties);
        InitializeAndCreateFFunctions(FinalClass, Params.Functions, Params.NumFunctions);

        for (uint16 i = 0; i < Params.NumMetaData; ++i)
        {
            const FMetaDataPairParam& Param = Params.MetaDataArray[i];
            FinalClass->Metadata.AddValue(Param.NameUTF8, Param.ValueUTF8);
        }

        if (Params.SuperFunc)
        {
            CStruct* SuperStruct = Params.SuperFunc();
            FinalClass->SetSuperStruct(SuperStruct);
        }
        
        FinalClass->Link();

        FinalClass->AddToRoot();
    }
    
}
