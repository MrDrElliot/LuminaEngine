#pragma once

#include "Assets/AssetManager/AssetManager.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/SoftObjectPtr.h"
#include "TaskSystem/Coroutine.h"

namespace Lumina::Coro
{
    // Waits for Handle and hands it back, so whatever it loaded stays resident while the caller keeps it.
    inline TCoroutine<FAssetHandle> Load(FAssetHandle Handle)
    {
        if (Handle.IsValid() && !Handle.IsReady())
        {
            co_await WaitForReady(Handle);
        }
        co_return Handle;
    }

    // Invalid once loaded when the path resolves to nothing.
    inline TCoroutine<FAssetHandle> Load(FStringView Path, FName Bundle = NAME_None)
    {
        return Load(FAssetManager::Get().Load(Path, Bundle));
    }

    inline TCoroutine<FAssetHandle> Load(const FGuid& Asset, FName Bundle = NAME_None)
    {
        return Load(FAssetManager::Get().Load(Asset, Bundle));
    }

    inline TCoroutine<FAssetHandle> Load(const SAssetId& Id, FName Bundle = NAME_None)
    {
        return Load(FAssetManager::Get().Load(Id, Bundle));
    }

    inline TCoroutine<FAssetHandle> LoadGroup(FName Group, FName Bundle = NAME_None)
    {
        return Load(FAssetManager::Get().LoadGroup(Group, Bundle));
    }

    // Null when the load failed or the object is not a T. The pointer keeps it resident after the load's handle goes.
    template<typename T>
    TCoroutine<TStrongObjectPtr<T>> LoadObject(FAssetHandle Handle)
    {
        const FAssetHandle Loaded = co_await Load(Move(Handle));
        co_return TStrongObjectPtr<T>(Loaded.IsValid() ? Loaded.template Get<T>() : nullptr);
    }

    template<typename T>
    TCoroutine<TStrongObjectPtr<T>> LoadObject(FStringView Path)
    {
        return LoadObject<T>(FAssetManager::Get().Load(Path));
    }

    template<typename T>
    TCoroutine<TStrongObjectPtr<T>> LoadObject(const FGuid& Asset)
    {
        return LoadObject<T>(FAssetManager::Get().Load(Asset));
    }

    // Resolves through the cached GUID, so a soft pointer whose asset was renamed still loads.
    template<typename T>
    TCoroutine<TStrongObjectPtr<T>> LoadObject(const TSoftObjectPtr<T>& Object)
    {
        return LoadObject<T>(Object.GetSoftPath().Load());
    }

    inline TCoroutine<TStrongObjectPtr<CObject>> LoadObject(const FSoftObjectPath& Path)
    {
        return LoadObject<CObject>(Path.Load());
    }
}
