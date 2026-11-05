#include "RuntimePCH.h"
#include "AssetLibrary.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Threading/Thread.h"
#include "TaskSystem/ThreadedCallback.h"

namespace Lumina
{
    namespace
    {
        uint64 Own(FAssetHandle&& Handle)
        {
            return Handle.IsValid() ? reinterpret_cast<uint64>(Memory::New<FAssetHandle>(Move(Handle))) : 0;
        }

        const FAssetHandle* Borrow(uint64 Handle)
        {
            return reinterpret_cast<const FAssetHandle*>(Handle);
        }

        FStringView ViewOf(const FString& Text)
        {
            return FStringView(*Text, Text.Len());
        }
    }

    uint64 CAssetLibrary::Load(const FString& Path, const FName& Bundle)
    {
        return Own(FAssetManager::Get().Load(ViewOf(Path), Bundle));
    }

    uint64 CAssetLibrary::LoadByName(const FName& Group, const FName& Name, const FName& Bundle)
    {
        return Own(FAssetManager::Get().Load(SAssetId{ Group, Name }, Bundle));
    }

    uint64 CAssetLibrary::LoadGroup(const FName& Group, const FName& Bundle)
    {
        return Own(FAssetManager::Get().LoadGroup(Group, Bundle));
    }

    void CAssetLibrary::Release(uint64 Handle)
    {
        FAssetHandle* Owned = reinterpret_cast<FAssetHandle*>(Handle);
        if (Owned == nullptr)
        {
            return;
        }
        if (Threading::IsMainThread())
        {
            Memory::Delete(Owned);
            return;
        }
        MainThread::Enqueue([Owned]() { Memory::Delete(Owned); });
    }

    bool CAssetLibrary::IsReady(uint64 Handle)
    {
        return Handle != 0 && Borrow(Handle)->IsReady();
    }

    float CAssetLibrary::GetProgress(uint64 Handle)
    {
        return Handle != 0 ? Borrow(Handle)->GetProgress() : 0.0f;
    }

    int32 CAssetLibrary::Num(uint64 Handle)
    {
        return Handle != 0 ? (int32)Borrow(Handle)->Num() : 0;
    }

    uint64 CAssetLibrary::Get(uint64 Handle, int32 Index, TSubclassOf<CObject> Class)
    {
        CObject* Object = Handle != 0 && Index >= 0 ? Borrow(Handle)->Get((uint32)Index) : nullptr;
        return Object != nullptr && (Class.Get() == nullptr || Object->IsA(Class.Get())) ? reinterpret_cast<uint64>(Object) : 0;
    }

    void CAssetLibrary::OnReady(uint64 Handle, SScriptCallback Callback)
    {
        if (Handle == 0)
        {
            Scripting::InvokeScriptCallback(Callback, 0);
            return;
        }
        Borrow(Handle)->OnReady([Callback](const FAssetHandle&) { Scripting::InvokeScriptCallback(Callback, 0); });
    }

    FString CAssetLibrary::GetGroupNames(const FName& Group)
    {
        TVector<SAssetId> Members;
        FAssetManager::Get().GetGroup(Group, Members);

        FString Names;
        for (const SAssetId& Member : Members)
        {
            Names.Append(*Member.Name).Append("\n");
        }
        return Names;
    }

    uint64 CAssetLibrary::LoadSynchronous(const FString& Path)
    {
        return reinterpret_cast<uint64>(StaticLoadObject(ViewOf(Path)));
    }

    bool CAssetLibrary::Exists(const FString& Path)
    {
        return FAssetRegistry::Get().GetAssetByPath(ViewOf(Path)) != nullptr;
    }
}
