#include "pch.h"
#include "ScriptableObject.h"

#include "Containers/Array.h"
#include "DotNet/DotNetHost.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectBase.h"
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
