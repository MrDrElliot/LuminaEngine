#pragma once

#include "Containers/Span.h"
#include "Containers/Function.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectMacros.h"
#include "GUID/GUID.h"
#include "Memory/SmartPtr.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Sync.h"
#include "AssetManager.generated.h"


namespace Lumina
{
    class CObject;
    struct FAssetLoad;

    // An asset of a REFLECT(Group = ...) class, named by that group and its asset name.
    REFLECT()
    struct RUNTIME_API SAssetId
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Group;

        PROPERTY(Editable)
        FName Name;

        NODISCARD bool IsValid() const { return !Group.IsNone() && !Name.IsNone(); }
        bool operator==(const SAssetId& Other) const { return Group == Other.Group && Name == Other.Name; }
    };

    NODISCARD FORCEINLINE uint64 GetTypeHash(const SAssetId& Id) noexcept
    {
        size_t Seed = GetTypeHash(Id.Group);
        Hash::HashCombine(Seed, GetTypeHash(Id.Name));
        return Seed;
    }

    // Keeps everything it loaded resident until its last copy goes, which is all unloading is.
    class RUNTIME_API FAssetHandle
    {
    public:

        NODISCARD bool IsValid() const { return State != nullptr; }
        NODISCARD bool IsReady() const;

        // Loaded over requested, one once ready.
        NODISCARD float GetProgress() const;

        void Wait() const;

        // Runs on the game thread once everything has loaded, and right away there when it already has or nothing resolved.
        void OnReady(TFunction<void(const FAssetHandle&)> Callback) const;

        // In request order, the assets first and then what their bundle added. Null where a load failed.
        NODISCARD uint32 Num() const;
        NODISCARD CObject* Get(uint32 Index = 0) const;

        template<typename T>
        NODISCARD T* Get(uint32 Index = 0) const { return Cast<T>(Get(Index)); }

        void Reset() { State.Reset(); }

    private:

        friend class FAssetManager;

        TSharedPtr<FAssetLoad> State;
    };

    // Owns nothing itself; an asset stays loaded while a handle, a component or anything else holds it.
    class RUNTIME_API FAssetManager final
    {
    public:

        FAssetManager();
        ~FAssetManager() = default;
        LE_NO_COPYMOVE(FAssetManager);

        static FAssetManager& Get();

        // The assets, plus every soft reference their properties put in Bundle when one is named.
        FAssetHandle Load(TSpan<const FGuid> Assets, FName Bundle = NAME_None);
        FAssetHandle Load(const FGuid& Asset, FName Bundle = NAME_None);
        // Invalid when the path or id resolves to nothing.
        FAssetHandle Load(FStringView Path, FName Bundle = NAME_None);
        FAssetHandle Load(const SAssetId& Id, FName Bundle = NAME_None);

        // Every asset in the group, each with its Bundle when one is named.
        FAssetHandle LoadGroup(FName Group, FName Bundle = NAME_None);

        // Loads inline on the caller, or finds the object when an async load of it is already running.
        CObject* LoadSynchronous(const FGuid& Asset);

        // Answered from the registry, so nothing loads.
        void GetGroups(TVector<FName>& Out);
        void GetGroup(FName Group, TVector<SAssetId>& Out);
        NODISCARD FGuid Resolve(const SAssetId& Id);

        // The group a REFLECT(Group = ...) on the class or a base puts its assets in, or None.
        NODISCARD FName GetGroupOfClass(FName ClassName);

    private:

        struct FWaiter
        {
            TSharedPtr<FAssetLoad> Load;
            int32                  Index = 0;
        };

        void Request(const FGuid& Asset, FWaiter Waiter);
        void DrainQueuedLoads();
        void PerformLoad(const FGuid& Asset);
        // Called holding GroupMutex, so a reader never sees the tables half built.
        void RebuildGroupsIfDirty();

    private:

        FMutex                             RequestMutex;

        // Everyone waiting on an asset that is loading, so a second request joins the first.
        THashMap<FGuid, TVector<FWaiter>>       InFlight;

        // A streaming burst asks for hundreds of assets in one frame, so they queue for one draining task.
        TVector<FGuid>                          QueuedLoads;
        bool                                    bDrainScheduled = false;

        // Taken in this order, groups, then the registry, then classes, which is how a rebuild nests them.
        FMutex                             GroupMutex;
        THashMap<FName, TVector<FGuid>>         GroupMembers;
        THashMap<SAssetId, FGuid>               GroupIds;
        std::atomic<bool>                       bGroupsDirty = true;

        TMutex<THashMap<FName, FName>>          GroupOfClass;
    };
}
