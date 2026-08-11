#include "RuntimePCH.h"

#include <atomic>

#include "ObjectCore.h"
#include "Class.h"
#include "Object.h"
#include "ObjectAllocator.h"
#include "ObjectHash.h"
#include "ObjectIterator.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Engine/Engine.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Type/LuminaTypes.h"
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

        // Not a registered asset: nothing to fan out, fall back to the plain inline load.
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

    bool IsValueValidForType(double Value, const FName& TypeName)
    {
        if (TypeName == "Int8Property")
        {
            return Value >= INT8_MIN && Value <= INT8_MAX;
        }
        if (TypeName == "Int16Property")
        {
            return Value >= INT16_MIN && Value <= INT16_MAX;
        }
        if (TypeName == "IntProperty")
        {
            return Value >= INT32_MIN && Value <= INT32_MAX;
        }
        if (TypeName == "Int64Property")
        {
            return Value >= (double)INT64_MIN && Value <= (double)INT64_MAX;
        }
        if (TypeName == "UInt8Property")
        {
            return Value >= 0 && Value <= UINT8_MAX;
        }
        if (TypeName == "UInt16Property")
        {
            return Value >= 0 && Value <= UINT16_MAX;
        }
        if (TypeName == "UInt32Property")
        {
            return Value >= 0 && Value <= UINT32_MAX;
        }
        if (TypeName == "UInt64Property")
        {
            return Value >= 0 && Value <= (double)UINT64_MAX;
        }
        if (TypeName == "FloatProperty" || TypeName == "DoubleProperty")
        {
            return true;
        }
        return false;
    }

    bool IsPropertyNumeric(const FName& Type)
    {
        return Type == "Int8Property" ||
            Type == "Int16Property" ||
            Type == "Int32Property" ||
            Type == "Int64Property" ||
            Type == "UInt8Property" ||
            Type == "UInt16Property" ||
            Type == "UInt32Property" ||
            Type == "UInt64Property" ||
            Type == "FloatProperty" || 
            Type == "DoubleProperty";
    }

    static void ConstructPropertyMetadata(FProperty* NewProperty, uint16 NumMetadata, const FMetaDataPairParam* ParamArray)
    {
        for (uint16 i = 0; i < NumMetadata; ++i)
        {
            const FMetaDataPairParam& Param = ParamArray[i];
            NewProperty->Metadata.AddValue(Param.NameUTF8, Param.ValueUTF8);
        }
        
        NewProperty->OnMetadataFinalized();
    }

    template<typename TPropertyType, typename TPropertyParamType>
    TPropertyType* NewFProperty(FFieldOwner Owner, const FPropertyParams* Param)
    {
        const TPropertyParamType* TypedParam = static_cast<const TPropertyParamType*>(Param);
        TPropertyType* Type = nullptr;
        
        if(Param->GetterFunc || Param->SetterFunc)
        {
            Type = Memory::New<TPropertyWithSetterAndGetter<TPropertyType>>(Owner, TypedParam);
        }
        else
        {
            Type = Memory::New<TPropertyType>(Owner, TypedParam);
        }
        
        if (TypedParam->NumMetaData)
        {
            ConstructPropertyMetadata(Type, TypedParam->NumMetaData, TypedParam->MetaDataArray);
        }
        return Type;
    }
    
    
    void ConstructProperties(const FFieldOwner& FieldOwner, const FPropertyParams* const*& Properties, uint32& NumProperties)
    {
        const FPropertyParams* Param = *--Properties;

        // Non-zero if property has an inner property next in Properties list.
        uint32 ReadMore = 0;

        
        FProperty* NewProperty = nullptr;
    
        switch (Param->TypeFlags)
        {
        case EPropertyTypeFlags::Int8:
            NewFProperty<FInt8Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Int16:
            NewFProperty<FInt16Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Int32:
            NewFProperty<FInt32Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Int64:
            NewFProperty<FInt64Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::UInt8:
            NewFProperty<FUInt8Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::UInt16:
            NewFProperty<FUInt16Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::UInt32:
            NewFProperty<FUInt32Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::UInt64:
            NewFProperty<FUInt64Property, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Float:
            NewFProperty<FFloatProperty, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Double:
            NewFProperty<FDoubleProperty, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Bool:
            NewFProperty<FBoolProperty, FNumericPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Object:
            NewFProperty<FObjectProperty, FObjectPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::SoftObject:
            NewFProperty<FSoftObjectProperty, FSoftObjectPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Class:
            NewFProperty<FClassProperty, FClassPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Name:
            NewFProperty<FNameProperty, FNamePropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::String:
            NewFProperty<FStringProperty, FStringPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Struct:
            NewFProperty<FStructProperty, FStructPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::SubStruct:
            NewFProperty<FSubStructProperty, FSubStructPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::InstancedStruct:
            NewFProperty<FInstancedStructProperty, FInstancedStructPropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Delegate:
            NewFProperty<FDelegateProperty, FDelegatePropertyParams>(FieldOwner, Param);
            break;
        case EPropertyTypeFlags::Enum:
            {
                NewProperty = NewFProperty<FEnumProperty, FEnumPropertyParams>(FieldOwner, Param);
                
                ReadMore = 1;
            }
            break;
        case EPropertyTypeFlags::Vector:
            {
                NewProperty = NewFProperty<FArrayProperty, FArrayPropertyParams>(FieldOwner, Param);

                ReadMore = 1;
            }
            break;
        case EPropertyTypeFlags::Map:
            {
                // Two inners follow (backward: Key first, then Value). The emitter lays them out as
                // [Value, Key, Map] so the ReadMore=2 recursion attaches Key on the first pass, Value the second.
                NewProperty = NewFProperty<FMapProperty, FMapPropertyParams>(FieldOwner, Param);

                ReadMore = 2;
            }
            break;
        case EPropertyTypeFlags::Optional:
            {
                // Like Vector/Enum, the next FPropertyParams describes the inner payload type.
                NewProperty = NewFProperty<FOptionalProperty, FOptionalPropertyParams>(FieldOwner, Param);

                ReadMore = 1;
            }
            break;
        default:
            {
                LOG_CRITICAL("Unsupported property type found while creating: {}", Param->Name);
            }
            break;
        }

        --NumProperties;
        for (; ReadMore; --ReadMore)
        {
            FFieldOwner Owner;
            Owner.emplace<FField*>(NewProperty);
            ConstructProperties(Owner, Properties, NumProperties);
        }
    }

    void InitializeAndCreateFProperties(CStruct* Outer, const FPropertyParams* const* PropertyArray, uint32 NumProperties)
    {
        // Iterates backwards.
        PropertyArray += NumProperties;
        while (NumProperties)
        {
            FFieldOwner Owner;
            Owner.emplace<CStruct*>(Outer);
            ConstructProperties(Owner, PropertyArray, NumProperties);
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
