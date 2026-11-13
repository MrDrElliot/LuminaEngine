#include "RuntimePCH.h"

#include "AssetManager.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Core/Profiler/AssetLoadTracker.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/ThreadedCallback.h"

namespace Lumina
{
    struct FAssetLoad
    {
        TVector<TStrongObjectPtr<CObject>>                  Objects;

        // One more than the loads still running until the request finishes issuing them, so none completes it early.
        std::atomic<uint32>                                 Pending = 1;

        Jobs::FTaskEvent                                    Done;

        FMutex                                              CallbackMutex;
        TVector<TFunction<void(const FAssetHandle&)>>       Callbacks;
        bool                                                bDispatched = false;
    };

    namespace
    {
        bool IsResident(const CObject* Object)
        {
            return Object != nullptr && !Object->HasAnyFlag(OF_NeedsLoad | OF_Loading | OF_NeedsPostLoad);
        }

        void RecordRequest(const FAssetData* Data, double DurationMs, EAssetLoadOutcome Outcome)
        {
#if USING(WITH_EDITOR)
            FAssetLoadRecord Entry;
            Entry.Name        = Data != nullptr ? FName(*Data->Path) : FName("<unregistered>");
            Entry.Source      = EAssetLoadSource::Request;
            Entry.Outcome     = Outcome;
            Entry.DurationMs  = DurationMs;
            Entry.CompletedAt = PlatformTime::Seconds();
            Entry.ThreadId    = (uint32)Threading::GetThreadID();
            FAssetLoadTracker::Get().Record(Entry);
#else
            (void)Data; (void)DurationMs; (void)Outcome;
#endif
        }

        // Runs the waiting callbacks on the game thread, then lets later OnReady calls run straight away.
        void Dispatch(const TSharedPtr<FAssetLoad>& State, const FAssetHandle& Handle)
        {
            TVector<TFunction<void(const FAssetHandle&)>> Callbacks;
            {
                FScopeLock Lock(State->CallbackMutex);
                State->bDispatched = true;
                Callbacks = Move(State->Callbacks);
            }
            for (TFunction<void(const FAssetHandle&)>& Callback : Callbacks)
            {
                Callback(Handle);
            }
        }
    }

    bool FAssetHandle::IsReady() const
    {
        return State != nullptr && State->Pending.load(std::memory_order_acquire) == 0;
    }

    float FAssetHandle::GetProgress() const
    {
        if (State == nullptr || State->Objects.IsEmpty())
        {
            return IsReady() ? 1.0f : 0.0f;
        }
        const uint32 Remaining = State->Pending.load(std::memory_order_acquire);
        const uint32 Total = static_cast<uint32>(State->Objects.Num());
        return 1.0f - (float)Math::Min(Remaining, Total) / (float)Total;
    }

    void FAssetHandle::Wait() const
    {
        if (State != nullptr)
        {
            State->Done.Wait();
        }
    }

    void FAssetHandle::OnReady(TFunction<void(const FAssetHandle&)> Callback) const
    {
        if (!Callback)
        {
            return;
        }
        if (State != nullptr)
        {
            FScopeLock Lock(State->CallbackMutex);
            if (!State->bDispatched)
            {
                State->Callbacks.Add(Move(Callback));
                return;
            }
        }
        if (Threading::IsMainThread())
        {
            Callback(*this);
            return;
        }
        MainThread::Enqueue([Handle = *this, Callback = Move(Callback)]() { Callback(Handle); });
    }

    uint32 FAssetHandle::Num() const
    {
        return State != nullptr ? (uint32)State->Objects.Num() : 0;
    }

    CObject* FAssetHandle::Get(uint32 Index) const
    {
        return IsReady() && Index < static_cast<uint32>(State->Objects.Num()) ? State->Objects[Index].Get() : nullptr;
    }

    FAssetManager::FAssetManager()
    {
        // New assets, new script classes and new modules can each move an asset into a group.
        auto MarkGroupsDirty = [this]() { bGroupsDirty.store(true, std::memory_order_release); };
        (void)FAssetRegistry::Get().GetOnAssetRegistryUpdated().AddLambda(MarkGroupsDirty);
        (void)FCoreDelegates::OnScriptsWillReload.AddLambda(MarkGroupsDirty);
        (void)FCoreDelegates::OnModuleLoaded.AddLambda([MarkGroupsDirty](FModuleInfo*) { MarkGroupsDirty(); });
    }

    FAssetManager& FAssetManager::Get()
    {
        static FAssetManager Instance;
        return Instance;
    }

    FAssetHandle FAssetManager::Load(TSpan<const FGuid> Assets, FName Bundle)
    {
        LUMINA_PROFILE_SCOPE();
        TVector<FGuid> Requested(Assets.begin(), Assets.end());
        if (!Bundle.IsNone())
        {
            THashSet<FGuid> Seen;
            for (const FGuid& Asset : Assets)
            {
                Seen.Add(Asset);
            }
            for (const FGuid& Asset : Assets)
            {
                const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByGUID(Asset);
                if (Data == nullptr)
                {
                    continue;
                }
                for (const FAssetBundleEntry& Entry : Data->Bundles)
                {
                    if (Entry.Bundle == Bundle && Seen.TryAdd(Entry.Target))
                    {
                        Requested.Add(Entry.Target);
                    }
                }
            }
        }

        FAssetHandle Handle;
        Handle.State = MakeShared<FAssetLoad>();
        FAssetLoad& State = *Handle.State;
        State.Objects.SetNum(Requested.Num());
        State.Pending.fetch_add((uint32)Requested.Num(), std::memory_order_relaxed);

        uint32 Resident = 0;
        for (int32 Index = 0; Index < Requested.Num(); ++Index)
        {
            // An id the registry could not resolve stays null rather than queuing a load that would fail.
            if (!Requested[Index].IsValid())
            {
                ++Resident;
                continue;
            }
            if (CObject* Object = FindObject<CObject>(Requested[Index]); IsResident(Object))
            {
                State.Objects[Index] = Object;
                ++Resident;
                continue;
            }
            Request(Requested[Index], FWaiter{ Handle.State, Index });
        }

        if (State.Pending.fetch_sub(Resident + 1, std::memory_order_acq_rel) == Resident + 1)
        {
            State.Done.Signal();
            Dispatch(Handle.State, Handle);
        }
        return Handle;
    }

    FAssetHandle FAssetManager::Load(const FGuid& Asset, FName Bundle)
    {
        return Load(TSpan<const FGuid>(&Asset, 1), Bundle);
    }

    FAssetHandle FAssetManager::Load(FStringView Path, FName Bundle)
    {
        const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByPath(Path);
        return Data != nullptr ? Load(Data->AssetGUID, Bundle) : FAssetHandle();
    }

    FAssetHandle FAssetManager::Load(const SAssetId& Id, FName Bundle)
    {
        const FGuid Asset = Resolve(Id);
        return Asset.IsValid() ? Load(Asset, Bundle) : FAssetHandle();
    }

    FAssetHandle FAssetManager::LoadGroup(FName Group, FName Bundle)
    {
        TVector<FGuid> Members;
        {
            FScopeLock Lock(GroupMutex);
            RebuildGroupsIfDirty();
            if (auto It = GroupMembers.Find(Group); It != nullptr)
            {
                Members = (*It);
            }
        }
        return Load(TSpan<const FGuid>(Members.GetData(), Members.Num()), Bundle);
    }

    void FAssetManager::Request(const FGuid& Asset, FWaiter Waiter)
    {
        bool bSchedule = false;
        {
            FScopeLock Lock(RequestMutex);
            const bool bInserted = InFlight.TryAdd(Asset);
            InFlight.FindChecked(Asset).Add(Move(Waiter));
            if (bInserted)
            {
                QueuedLoads.Add(Asset);
                bSchedule = !bDrainScheduled;
                bDrainScheduled = true;
            }
        }
        if (bSchedule)
        {
            Jobs::Spawn(Jobs::EJobPriority::Normal, [](FAssetManager* Self) -> Jobs::TTask<void>
            {
                Self->DrainQueuedLoads();
                co_return;
            }, this);
        }
    }

    void FAssetManager::DrainQueuedLoads()
    {
        LUMINA_PROFILE_SCOPE();
        TVector<FGuid> Batch;
        for (;;)
        {
            {
                FScopeLock Lock(RequestMutex);
                if (QueuedLoads.IsEmpty())
                {
                    bDrainScheduled = false;
                    return;
                }
                Batch = Move(QueuedLoads);
                QueuedLoads.Reset();
            }

            Jobs::ParallelFor((uint32)Batch.Num(), [this, &Batch](uint32 Index)
            {
                PerformLoad(Batch[Index]);
            }, 1);
            Batch.Reset();
        }
    }

    void FAssetManager::PerformLoad(const FGuid& Asset)
    {
        const uint64 Start = PlatformTime::Cycles();
        const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByGUID(Asset);

        CObject* Object = FindObject<CObject>(Asset);
        if (!IsResident(Object) && Data != nullptr)
        {
            if (CPackage* Package = CPackage::LoadPackage(Data->Path))
            {
                Object = Package->LoadObject(Asset);
            }
        }
        RecordRequest(Data.Get(), PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start),
            Object != nullptr ? EAssetLoadOutcome::Loaded : EAssetLoadOutcome::Failed);

        TVector<FWaiter> Waiters;
        {
            FScopeLock Lock(RequestMutex);
            if (auto It = InFlight.Find(Asset); It != nullptr)
            {
                Waiters = Move(*It);
                InFlight.Remove(Asset);
            }
        }

        for (FWaiter& Waiter : Waiters)
        {
            FAssetLoad& State = *Waiter.Load;
            State.Objects[Waiter.Index] = Object;
            if (State.Pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                State.Done.Signal();

                // The game thread takes the last reference, since dropping one can destroy what it holds.
                MainThread::Enqueue([Load = Move(Waiter.Load)]() mutable
                {
                    FAssetHandle Handle;
                    Handle.State = Load;
                    Dispatch(Load, Handle);
                });
            }
        }
    }

    CObject* FAssetManager::LoadSynchronous(const FGuid& Asset)
    {
        if (CObject* Object = FindObject<CObject>(Asset); IsResident(Object))
        {
            return Object;
        }

        {
            FScopeLock Lock(RequestMutex);
            if (InFlight.Contains(Asset))
            {
                // Joining would deadlock a PostLoad that loads what is mid-load, and the import resolves it anyway.
                return FindObject<CObject>(Asset);
            }
            InFlight.FindOrAdd(Asset);
        }
        PerformLoad(Asset);
        return FindObject<CObject>(Asset);
    }

    void FAssetManager::RebuildGroupsIfDirty()
    {
        if (!bGroupsDirty.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();
        GroupMembers.Reset();
        GroupIds.Reset();
        GroupOfClass.Lock()->Reset();

        (void)FAssetRegistry::Get().FindByPredicate([&](const FAssetData& Data)
        {
            const FName Group = GetGroupOfClass(Data.AssetClass);
            if (Group.IsNone())
            {
                return false;
            }
            GroupMembers[Group].Add(Data.AssetGUID);
            if (!GroupIds.TryAdd(SAssetId{ Group, Data.AssetName }, Data.AssetGUID))
            {
                LOG_WARN("FAssetManager: two assets named '{}' in group '{}'; lookups by name find the first.", *Data.AssetName, *Group);
            }
            return false;
        });
    }

    FName FAssetManager::GetGroupOfClass(FName ClassName)
    {
        // Classes repeat across thousands of assets, so each is walked for its group once.
        auto Groups = GroupOfClass.Lock();
        if (const FName* Cached = Groups->Find(ClassName))
        {
            return *Cached;
        }
        FName& Group = Groups->Add(ClassName);
        for (CClass* Class = FindObject<CClass>(ClassName); Class != nullptr; Class = Class->GetSuperClass())
        {
            if (Class->HasMeta("Group"))
            {
                Group = FName(Class->GetMeta("Group"));
                break;
            }
        }
        return Group;
    }

    void FAssetManager::GetGroups(TVector<FName>& Out)
    {
        FScopeLock Lock(GroupMutex);
        RebuildGroupsIfDirty();
        Out.Reserve(Out.Num() + GroupMembers.Num());
        for (const auto& [Group, Members] : GroupMembers)
        {
            Out.Add(Group);
        }
    }

    void FAssetManager::GetGroup(FName Group, TVector<SAssetId>& Out)
    {
        FScopeLock Lock(GroupMutex);
        RebuildGroupsIfDirty();
        auto It = GroupMembers.Find(Group);
        if (It == nullptr)
        {
            return;
        }
        Out.Reserve(Out.Num() + It->Num());
        for (const FGuid& Member : (*It))
        {
            if (const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByGUID(Member))
            {
                Out.Emplace(Group, Data->AssetName);
            }
        }
    }

    FGuid FAssetManager::Resolve(const SAssetId& Id)
    {
        FScopeLock Lock(GroupMutex);
        RebuildGroupsIfDirty();
        auto It = GroupIds.Find(Id);
        return It != nullptr ? (*It) : FGuid();
    }
}
