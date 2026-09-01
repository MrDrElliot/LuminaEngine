#include "RuntimePCH.h"

#include "AssetManager.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/AssetLoadTracker.h"
#include "Platform/Time/PlatformTime.h"
#include "Log/Log.h"

namespace Lumina
{
    FAssetManager& FAssetManager::Get()
    {
        static FAssetManager Instance;
        return Instance;
    }

    void FAssetManager::RecordRequest(const FFixedString& Path, double DurationMs, EAssetLoadOutcome Outcome)
    {
#if USING(WITH_EDITOR)
        FAssetLoadRecord Entry;
        Entry.Name        = FName(Path.c_str());
        Entry.Source      = EAssetLoadSource::Request;
        Entry.Outcome     = Outcome;
        Entry.DurationMs  = DurationMs;
        Entry.CompletedAt = PlatformTime::Seconds();
        Entry.ThreadId    = (uint32)Threading::GetThreadID();
        FAssetLoadTracker::Get().Record(Entry);
#else
        (void)Path; (void)DurationMs; (void)Outcome;
#endif
    }

    FAssetHandle FAssetManager::AcquireLoad(const FGuid& GUID, TPromise<CObject*>& OutPromise, bool& bShouldLoad)
    {
        FFiberScopeLock Lock(RequestMutex);

        if (auto It = InFlight.find(GUID); It != InFlight.end())
        {
            bShouldLoad = false;
            return It->second;
        }

        TPromise<CObject*> Promise;
        FAssetHandle Handle = Promise.GetFuture();
        InFlight.emplace(GUID, Handle);
        OutPromise  = Move(Promise);
        bShouldLoad = true;
        return Handle;
    }

    void FAssetManager::PerformLoad(const FFixedString& Path, const FGuid& GUID, TPromise<CObject*> Promise)
    {
        const uint64 Start = PlatformTime::Cycles();

        CObject* Object = nullptr;
        if (CPackage* Package = CPackage::LoadPackage(Path))
        {
            Object = Package->LoadObject(GUID);
        }

        // Timed around LoadObject as well, so a request reports what the caller waited for rather than
        // only the package read underneath it.
        RecordRequest(Path, PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start),
            Object != nullptr ? EAssetLoadOutcome::Loaded : EAssetLoadOutcome::Failed);

        Promise.SetValue(Object);

        // A request arriving in this window attaches to the satisfied handle instead of loading again.
        FFiberScopeLock Lock(RequestMutex);
        InFlight.erase(GUID);
    }

    FAssetHandle FAssetManager::LoadAssetAsync(const FFixedString& PackagePath, const FGuid& RequestedAsset)
    {
        TPromise<CObject*> Promise;
        bool bShouldLoad = false;
        FAssetHandle Handle = AcquireLoad(RequestedAsset, Promise, bShouldLoad);

        if (bShouldLoad)
        {
            FFixedString Path = PackagePath;
            Task::Async([this, Path, RequestedAsset, P = Move(Promise)]() mutable
            {
                PerformLoad(Path, RequestedAsset, Move(P));
            });
        }

        return Handle;
    }

    CObject* FAssetManager::LoadAssetSynchronous(const FFixedString& PackagePath, const FGuid& RequestedAsset)
    {
        const uint64 Start = PlatformTime::Cycles();

        TPromise<CObject*> Promise;
        bool bShouldLoad = false;
        FAssetHandle Handle = AcquireLoad(RequestedAsset, Promise, bShouldLoad);

        if (bShouldLoad)
        {
            // PerformLoad records this request; the handle is fulfilled immediately, so no scheduling hop.
            PerformLoad(PackagePath, RequestedAsset, Move(Promise));
            return Handle.Get();
        }

        // An async load for this asset is already in flight.
        CObject* Object = FindObject<CObject>(RequestedAsset);

        RecordRequest(PackagePath, PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start),
            Object != nullptr ? EAssetLoadOutcome::Joined : EAssetLoadOutcome::Failed);

        return Object;
    }

    void FAssetManager::FlushAsyncLoading()
    {
        // Loads can queue more loads (dependencies), so drain in waves until the table stays empty.
        for (;;)
        {
            TVector<FAssetHandle> Pending;
            {
                FFiberScopeLock Lock(RequestMutex);
                if (InFlight.empty())
                {
                    return;
                }
                Pending.reserve(InFlight.size());
                for (const auto& Entry : InFlight)
                {
                    Pending.push_back(Entry.second);
                }
            }
            WhenAll(Pending).Get();
        }
    }

    FAssetData* FAssetManager::ResolvePrimaryAsset(const FPrimaryAssetId& Id) const
    {
        if (!Id.IsValid())
        {
            return nullptr;
        }

        const FName Target = Id.GetName();
        const TVector<FAssetData*> Candidates = FAssetRegistry::Get().FindByPredicate(
            [&](const FAssetData& D)
            {
                return HasFlag(D.Flags, EAssetFlags::Primary) && D.AssetName == Target;
            });

        if (Candidates.empty())
        {
            return nullptr;
        }
        if (Candidates.size() > 1)
        {
            LOG_WARN("FAssetManager: primary id '{}' resolves to {} assets; returning the first. Primary names must be unique.",
                Target.ToString(), Candidates.size());
        }
        return Candidates[0];
    }

    CObject* FAssetManager::LoadPrimaryAssetSynchronous(const FPrimaryAssetId& Id)
    {
        FAssetData* Data = ResolvePrimaryAsset(Id);
        if (Data == nullptr)
        {
            return nullptr;
        }
        return LoadAssetSynchronous(Data->Path, Data->AssetGUID);
    }

    FAssetHandle FAssetManager::LoadPrimaryAssetAsync(const FPrimaryAssetId& Id)
    {
        FAssetData* Data = ResolvePrimaryAsset(Id);
        if (Data == nullptr)
        {
            return {};
        }
        return LoadAssetAsync(Data->Path, Data->AssetGUID);
    }
}
