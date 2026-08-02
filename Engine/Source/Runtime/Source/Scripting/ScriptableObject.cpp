#include "RuntimePCH.h"
#include "ScriptableObject.h"

#include "Containers/Array.h"
#include "DotNet/DotNetHost.h"
#include "Core/Object/Class.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Log/Log.h"

namespace Lumina
{
    
    void FScriptableBridge::Attach(CObject* InSelf)
    {
        Self = InSelf;
    }

    void FScriptableBridge::Bind(CObject* InSelf)
    {
        Self = InSelf;
        // The class default object never has a managed counterpart. Reached lazily (via EnsureBound) after
        // construction, by which point OF_DefaultObject is live on the CDO, so this guard is effective.
        if (Self == nullptr || Self->HasAnyFlag(OF_DefaultObject))
        {
            Generation = DotNet::GetScriptGeneration(); // mark attempted so the CDO isn't retried every dispatch
            return;
        }
        OverrideFlags = 0;
        Handle = DotNet::CreateScriptable(Self->GetClass()->GetName().ToString(), (uint64)(uintptr_t)Self, OverrideFlags);
        Generation = DotNet::GetScriptGeneration();
    }

    void FScriptableBridge::EnsureBound()
    {
        if (Self == nullptr)
        {
            return;
        }
        const int32 Gen = DotNet::GetScriptGeneration();

        if (Generation == Gen)
        {
            return;
        }
        Handle = nullptr;
        Bind(Self);
    }

    bool FScriptableBridge::ShouldDispatch(int32 Bit)
    {
        EnsureBound();
        return Handle != nullptr && (OverrideFlags & (1 << Bit)) != 0;
    }

    void FScriptableBridge::Destroy()
    {
        // Only free a handle owned by the CURRENT generation. A prior-generation handle was already freed by the
        // managed FreeAll on unload, so passing it back would double-free / free a recycled handle.
        if (Handle != nullptr && Generation == DotNet::GetScriptGeneration())
        {
            DotNet::DestroyScriptable(Handle);
        }
        Handle = nullptr;
        Generation = -1;
        Self = nullptr;
    }
    
    namespace
    {
        THashMap<FString, FScriptableNativeInfo>& GNativeInfos()
        {
            static THashMap<FString, FScriptableNativeInfo> Map;
            return Map;
        }
        THashMap<FName, CClass*>& GMintedClasses()
        {
            static THashMap<FName, CClass*> Map;
            return Map;
        }

        void ClearDeadClassRefsInStruct(CStruct* Layout, void* Data, CClass* Dead, const CObject* Owner, int32& OutCleared);

        // Nulls every reflected value inside Value that still points at Dead: FClassProperty (TSubclassOf)
        // directly, recursing through struct values, array elements, and instanced-struct payloads.
        void ClearDeadClassRefsInValue(FProperty* Property, void* Value, CClass* Dead, const CObject* Owner, int32& OutCleared)
        {
            switch (Property->TypeFlags)
            {
            case EPropertyTypeFlags::Class:
            {
                CClass** Ptr = static_cast<CClass**>(Value);
                if (*Ptr == Dead)
                {
                    *Ptr = nullptr;
                    ++OutCleared;
                    LOG_DISPLAY("Scriptable: cleared '{}.{}', it referenced the retired class '{}'.",
                        Owner->GetName().c_str(), Property->Name.c_str(), Dead->GetName().c_str());
                }
                break;
            }
            case EPropertyTypeFlags::Struct:
            {
                FStructProperty* StructProperty = static_cast<FStructProperty*>(Property);
                if (StructProperty->GetStruct() != nullptr)
                {
                    ClearDeadClassRefsInStruct(StructProperty->GetStruct(), Value, Dead, Owner, OutCleared);
                }
                break;
            }
            case EPropertyTypeFlags::Vector:
            {
                FArrayProperty* ArrayProperty = static_cast<FArrayProperty*>(Property);
                if (FProperty* Inner = ArrayProperty->GetInternalProperty())
                {
                    ArrayProperty->ForEach(Value, [&](void* Element, SIZE_T)
                    {
                        ClearDeadClassRefsInValue(Inner, Element, Dead, Owner, OutCleared);
                    });
                }
                break;
            }
            case EPropertyTypeFlags::InstancedStruct:
            {
                FInstancedStruct* Instanced = static_cast<FInstancedStruct*>(Value);
                if (Instanced->GetScriptStruct() != nullptr && Instanced->GetMutableMemory() != nullptr)
                {
                    ClearDeadClassRefsInStruct(Instanced->GetScriptStruct(), Instanced->GetMutableMemory(), Dead, Owner, OutCleared);
                }
                break;
            }
            default:
                break;
            }
        }

        void ClearDeadClassRefsInStruct(CStruct* Layout, void* Data, CClass* Dead, const CObject* Owner, int32& OutCleared)
        {
            for (CStruct* Current = Layout; Current != nullptr; Current = Current->GetSuperStruct())
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    ClearDeadClassRefsInValue(Property, static_cast<uint8*>(Data) + Property->Offset, Dead, Owner, OutCleared);
                });
            }
        }

        // Tears down a minted class whose C# type no longer exists in the current generation, so
        // FindObject / pickers stop seeing it and the name frees up for a future re-mint. Returns false
        // (class kept) while live instances remain; destroying the class under them would dangle their
        // class pointer, so removal is retried on the next reload once they are gone.
        bool TryRetireMintedClass(const FName& NameId, CClass* Class)
        {
            int32 LiveInstances = 0;
            GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
            {
                if (Base != nullptr && Base->GetClass() == Class
                    && !Base->HasAnyFlag(OF_MarkedDestroy) && !Base->HasAnyFlag(OF_DefaultObject))
                {
                    ++LiveInstances;
                }
            });
            if (LiveInstances > 0)
            {
                LOG_WARN("Scriptable: C# class '{}' was removed but {} live instance(s) remain; the class is kept until they are gone.",
                    NameId.c_str(), LiveInstances);
                return false;
            }

            // FClassProperty holds a raw CClass* (TSubclassOf), so any reflected value still naming this
            // class (a settings CDO, for example) must be nulled before the class dies. It serializes by
            // name, so a re-added type re-resolves from config/saves untouched.
            int32 Cleared = 0;
            GObjectArray.ForEachObject([&](CObjectBase* Base, int32)
            {
                if (Base == nullptr || Base == Class || Base->HasAnyFlag(OF_MarkedDestroy))
                {
                    return;
                }
                CObject* Object = static_cast<CObject*>(Base);
                if (Object->GetClass() != nullptr)
                {
                    ClearDeadClassRefsInStruct(Object->GetClass(), Object, Class, Object, Cleared);
                }
            });

            if (CObject* DefaultObject = Class->GetDefaultObjectIfCreated())
            {
                DefaultObject->RemoveFromRoot();
                DefaultObject->ForceDestroyNow();
            }
            Class->RemoveFromRoot();
            Class->ForceDestroyNow();

            LOG_DISPLAY("Scriptable: retired minted class '{}' (its C# type no longer exists).", NameId.c_str());
            return true;
        }
    }

    void FScriptableRegistry::RegisterNative(const char* NativeClassName, const FScriptableNativeInfo& Info)
    {
        // Static-init context (see GNativeInfos): key by string, never FName.
        GNativeInfos()[FString(NativeClassName)] = Info;
    }

    CClass* FScriptableRegistry::Mint(FStringView TypeName, FStringView NativeBaseName)
    {
        const FString Name(TypeName.data(), TypeName.size());
        const FName NameId(Name.c_str());

        if (auto Cached = GMintedClasses().find(NameId); Cached != GMintedClasses().end())
        {
            return Cached->second; // C# type names are stable across reloads -> reuse
        }
        if (FindObject<CClass>(NameId) != nullptr)
        {
            return nullptr; // a class with this name already exists (native, or a collision) - never shadow it
        }

        const FString BaseName(NativeBaseName.data(), NativeBaseName.size());
        auto It = GNativeInfos().find(BaseName);
        if (It == GNativeInfos().end())
        {
            LOG_WARN("Scriptable '{}': native base '{}' is not a REFLECT(Scriptable) class; not minted.",
                Name.c_str(), BaseName.c_str());
            return nullptr;
        }

        const FScriptableNativeInfo& Info = It->second;
        CClass* Minted = nullptr;
        AllocateStaticClass(TEXT("/Script"), UTF8_TO_TCHAR(Name.c_str()), &Minted,
            Info.ShimSize, Info.ShimAlign, Info.GetBaseClass, Info.Factory);
        if (Minted != nullptr)
        {
            GMintedClasses()[NameId] = Minted;
        }
        return Minted;
    }

    void FScriptableRegistry::RefreshMintedClasses()
    {
        TVector<DotNet::FScriptableTypeDesc> Descs;
        DotNet::GatherScriptableTypes(Descs);

        // Retire minted classes whose C# type vanished this generation (source deleted or renamed), so a
        // deleted script's class doesn't linger in FindObject / editor pickers forever.
        THashSet<FName> LiveNames;
        for (const DotNet::FScriptableTypeDesc& Desc : Descs)
        {
            LiveNames.insert(FName(Desc.TypeName.c_str()));
        }
        TVector<FName> StaleNames;
        for (const auto& [Name, Class] : GMintedClasses())
        {
            if (LiveNames.find(Name) == LiveNames.end())
            {
                StaleNames.push_back(Name);
            }
        }
        for (const FName& Name : StaleNames)
        {
            if (TryRetireMintedClass(Name, GMintedClasses()[Name]))
            {
                GMintedClasses().erase(Name);
            }
        }

        bool bMintedAny = false;
        for (const DotNet::FScriptableTypeDesc& Desc : Descs)
        {
            if (Mint(Desc.TypeName, Desc.NativeBaseName) != nullptr)
            {
                bMintedAny = true;
            }
        }

        // Finalize registration + create CDOs so FindObject<CClass>(name) / NewObject(class) work.
        if (bMintedAny)
        {
            ProcessNewlyLoadedCObjects();
        }
    }
}
