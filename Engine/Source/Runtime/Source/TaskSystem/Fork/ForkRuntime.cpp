#include "ForkRuntime.h"

#include "ForkDeque.h"
#include "ForkStack.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Memory/Memory.h"

#include <bit>
#include <cstdio>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace Lumina::Jobs
{
    namespace ForkRuntimeInternal
    {
        constexpr uint32 kNoSlot          = ~0u;
        constexpr uint32 kSpinsBeforeIdle = 128;
        constexpr uint32 kIdleTimeoutMs   = 50;
        constexpr uint32 kWaitTimeoutMs   = 1;

        using FDeque = TWorkDeque<FFrame>;

        FORCEINLINE void CpuPause()
        {
#if defined(_MSC_VER)
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }

        FORCEINLINE const volatile uint32* AsWord(const std::atomic<uint32>& Word)
        {
            return reinterpret_cast<const volatile uint32*>(&Word);
        }

        FORCEINLINE void WaitOnWord(const std::atomic<uint32>& Word, uint32 Expected, uint32 TimeoutMs)
        {
            Threading::WaitOnAddress32(AsWord(Word), Expected, TimeoutMs);
        }

        FORCEINLINE void WakeOne(const std::atomic<uint32>& Word) { Threading::WakeOneOnAddress32(AsWord(Word)); }
        FORCEINLINE void WakeAll(const std::atomic<uint32>& Word) { Threading::WakeAllOnAddress32(AsWord(Word)); }

        class FSpinLock
        {
        public:

            FORCEINLINE void Lock()
            {
                while (Flag.exchange(true, std::memory_order_acquire))
                {
                    while (Flag.load(std::memory_order_relaxed))
                    {
                        CpuPause();
                    }
                }
            }

            FORCEINLINE void Unlock() { Flag.store(false, std::memory_order_release); }

        private:

            std::atomic<bool> Flag{false};
        };

        struct FScopedSpin
        {
            explicit FScopedSpin(FSpinLock& InLock) : Lock(InLock) { Lock.Lock(); }
            ~FScopedSpin() { Lock.Unlock(); }
            FSpinLock& Lock;
        };

        struct alignas(64) FSlot
        {
            FDeque Bands[kNumJobPriorities];
        };

        struct FInjectionQueue
        {
            FSpinLock Lock;
            FFrame*   Head = nullptr;
            FFrame*   Tail = nullptr;
            std::atomic<uint32> Count{0};
        };

        struct FContext
        {
            FStack*  Stack      = nullptr;
            FStack*  AllocStack = nullptr;
            FStack*  Cached     = nullptr;
            FSlot*   Slot       = nullptr;
            uint32   SlotIndex  = kNoSlot;
            uint32   Generation = 0;
            uint32   TaskDepth  = 0;
            bool     bWorker    = false;
            bool     bOwnsExternalSlot = false;

            ~FContext();
        };

        struct FRuntime
        {
            uint32 NumWorkers  = 0;
            uint32 NumExternal = 0;
            uint32 NumSlots    = 0;

            FSlot* Slots = nullptr;
            FThread* Threads[kMaxThreadSlots] = {};
            uint16 StealOrder[kMaxThreadSlots][kMaxThreadSlots] = {};

            FInjectionQueue Injected[kNumJobPriorities];
            FRuntimeHooks   Hooks;

            alignas(64) std::atomic<uint32> LiveRoots{0};
            alignas(64) std::atomic<uint32> WorkSignal{0};
            alignas(64) std::atomic<uint32> Sleepers{0};
            alignas(64) std::atomic<uint32> Searching{0};
            alignas(64) std::atomic<uint64> ExternalSlotsFree{0};
            std::atomic<bool> bStopping{false};

            FSpinLock StackLock;
            FStack*   FreeStacks = nullptr;
        };

        FRuntime* GRuntime = nullptr;
        bool GHasHeavyFence = false;
        uint32 GGeneration = 0;
        thread_local FContext GContext;
        thread_local uint32   GIsolationDepth = 0;

        FStack* AcquireStack(FContext& Context)
        {
            if (FStack* Cached = Context.Cached)
            {
                Context.Cached = nullptr;
                return Cached;
            }
            {
                FScopedSpin Guard(GRuntime->StackLock);
                if (FStack* Free = GRuntime->FreeStacks)
                {
                    GRuntime->FreeStacks = Free->NextFree;
                    return Free;
                }
            }
            return Memory::New<FStack>();
        }

        void ReleaseStack(FContext& Context, FStack* Stack)
        {
            if (Context.Cached == nullptr)
            {
                Context.Cached = Stack;
                return;
            }
            FScopedSpin Guard(GRuntime->StackLock);
            Stack->NextFree = GRuntime->FreeStacks;
            GRuntime->FreeStacks = Stack;
        }

        FORCEINLINE void AdoptStack(FContext& Context, FStack* Stack)
        {
            if (Context.Stack != Stack)
            {
                if (Context.Stack != nullptr)
                {
                    ReleaseStack(Context, Context.Stack);
                }
                Context.Stack = Stack;
            }
            Context.AllocStack = Stack;
        }

        // Stacks from a stopped runtime are deleted; live ones go back to the pool.
        void ReturnContext(FContext& Context)
        {
            const bool bLive = GRuntime != nullptr && Context.Generation == GGeneration;
            FStack* Stacks[] = { Context.Stack, Context.AllocStack != Context.Stack ? Context.AllocStack : nullptr, Context.Cached };
            for (FStack* Stack : Stacks)
            {
                if (Stack == nullptr)
                {
                    continue;
                }
                if (bLive)
                {
                    FScopedSpin Guard(GRuntime->StackLock);
                    Stack->NextFree = GRuntime->FreeStacks;
                    GRuntime->FreeStacks = Stack;
                }
                else
                {
                    Memory::Delete(Stack);
                }
            }
            if (bLive && Context.bOwnsExternalSlot)
            {
                GRuntime->ExternalSlotsFree.fetch_or(1ull << (Context.SlotIndex - GRuntime->NumWorkers), std::memory_order_release);
            }
            Context.Stack      = nullptr;
            Context.AllocStack = nullptr;
            Context.Cached     = nullptr;
            Context.bOwnsExternalSlot = false;
        }

        // Only live stacks are returned here: thread exit can run after the allocator is gone.
        FContext::~FContext()
        {
            if (GRuntime != nullptr && Generation == GGeneration)
            {
                ReturnContext(*this);
            }
        }

        FORCEINLINE void WakeOneWorker()
        {
            GRuntime->WorkSignal.fetch_add(1, std::memory_order_relaxed);
            WakeOne(GRuntime->WorkSignal);
        }

        FORCEINLINE void NotifyWorkAvailable()
        {
            if (!GHasHeavyFence)
            {
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            else
            {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            if (GRuntime->Sleepers.load(std::memory_order_relaxed) != 0 && GRuntime->Searching.load(std::memory_order_relaxed) == 0)
            {
                WakeOneWorker();
            }
        }

        bool ClaimExternalSlot(FContext& Context)
        {
            uint64 Free = GRuntime->ExternalSlotsFree.load(std::memory_order_acquire);
            while (Free != 0)
            {
                const uint32 Bit = static_cast<uint32>(std::countr_zero(Free));
                if (GRuntime->ExternalSlotsFree.compare_exchange_weak(Free, Free & ~(1ull << Bit), std::memory_order_acq_rel))
                {
                    Context.SlotIndex = GRuntime->NumWorkers + Bit;
                    Context.Slot = &GRuntime->Slots[Context.SlotIndex];
                    Context.bOwnsExternalSlot = true;
                    return true;
                }
            }
            return false;
        }

        FContext& EnsureContext()
        {
            FContext& Context = GContext;
            if (Context.Generation != GGeneration)
            {
                ReturnContext(Context);
                Context = FContext();
                Context.Generation = GGeneration;
            }
            if (Context.Slot == nullptr && !ClaimExternalSlot(Context))
            {
                LOG_CRITICAL("Jobs: all {} external thread slots are taken; raise FRuntimeConfig::NumExternalThreads.", GRuntime->NumExternal);
                PANIC("Out of job thread slots");
            }
            if (Context.Stack == nullptr)
            {
                Context.Stack = AcquireStack(Context);
                Context.AllocStack = Context.Stack;
            }
            return Context;
        }

        void Inject(FFrame& Root)
        {
            FInjectionQueue& Queue = GRuntime->Injected[static_cast<uint32>(Root.Band)];
            Root.NextInjected = nullptr;
            {
                FScopedSpin Guard(Queue.Lock);
                if (Queue.Tail != nullptr)
                {
                    Queue.Tail->NextInjected = &Root;
                }
                else
                {
                    Queue.Head = &Root;
                }
                Queue.Tail = &Root;
                Queue.Count.fetch_add(1, std::memory_order_relaxed);
            }
            NotifyWorkAvailable();
        }

        FFrame* PopInjected(uint32 Band)
        {
            FInjectionQueue& Queue = GRuntime->Injected[Band];
            if (Queue.Count.load(std::memory_order_relaxed) == 0)
            {
                return nullptr;
            }
            FScopedSpin Guard(Queue.Lock);
            FFrame* Head = Queue.Head;
            if (Head != nullptr)
            {
                Queue.Head = Head->NextInjected;
                if (Queue.Head == nullptr)
                {
                    Queue.Tail = nullptr;
                }
                Queue.Count.fetch_sub(1, std::memory_order_relaxed);
            }
            return Head;
        }

        FFrame* StealFrom(const FContext& Context, uint32 Band)
        {
            const uint32 Self = Context.SlotIndex;
            if (Context.bWorker)
            {
                const uint16* Order = GRuntime->StealOrder[Self];
                for (uint32 Index = 0; Index + 1 < GRuntime->NumSlots; ++Index)
                {
                    FDeque& Victim = GRuntime->Slots[Order[Index]].Bands[Band];
                    if (!Victim.LooksEmpty())
                    {
                        if (FFrame* Frame = Victim.Steal())
                        {
                            return Frame;
                        }
                    }
                }
                return nullptr;
            }
            for (uint32 Index = 0; Index < GRuntime->NumSlots; ++Index)
            {
                const uint32 VictimIndex = (Self + 1 + Index) % GRuntime->NumSlots;
                if (VictimIndex == Self)
                {
                    continue;
                }
                FDeque& Victim = GRuntime->Slots[VictimIndex].Bands[Band];
                if (!Victim.LooksEmpty())
                {
                    if (FFrame* Frame = Victim.Steal())
                    {
                        return Frame;
                    }
                }
            }
            return nullptr;
        }

        struct FFound
        {
            FFrame* Frame     = nullptr;
            bool    bInjected = false;

            explicit operator bool() const { return Frame != nullptr; }
        };

        // A chain that ended on a parked child can leave its parents' continuations here; they are taken as steals.
        FFound FindWork(const FContext& Context, uint32 BandLimit)
        {
            const bool bIsolated = GIsolationDepth != 0;
            for (uint32 Band = 0; Band < BandLimit; ++Band)
            {
                if (FFrame* Own = Context.Slot->Bands[Band].Pop())
                {
                    return { Own, false };
                }
                if (bIsolated)
                {
                    continue;
                }
                if (FFrame* Root = PopInjected(Band))
                {
                    return { Root, true };
                }
                if (FFrame* Stolen = StealFrom(Context, Band))
                {
                    return { Stolen, false };
                }
            }
            return {};
        }

        bool AnyWorkVisible()
        {
            for (uint32 Band = 0; Band < kNumJobPriorities; ++Band)
            {
                if (GRuntime->Injected[Band].Count.load(std::memory_order_relaxed) != 0)
                {
                    return true;
                }
                for (uint32 Slot = 0; Slot < GRuntime->NumSlots; ++Slot)
                {
                    if (!GRuntime->Slots[Slot].Bands[Band].LooksEmpty())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // An injected frame brings its own stack unless stolen children still share it; a stolen continuation runs on ours and counts the steal.
        FORCEINLINE void Run(FContext& Context, const FFound& Found)
        {
            if (Found.bInjected)
            {
                if (Found.Frame->Steals == 0)
                {
                    AdoptStack(Context, Found.Frame->Stack);
                }
            }
            else
            {
                ++Found.Frame->Steals;
            }
            const FRuntimeHooks& Hooks = GRuntime->Hooks;
            if (Hooks.SegmentBegin != nullptr)
            {
                Hooks.SegmentBegin(Context.SlotIndex, Found.bInjected);
            }
            ++Context.TaskDepth;
            Found.Frame->Handle.resume();
            --Context.TaskDepth;
            if (Hooks.SegmentEnd != nullptr)
            {
                Hooks.SegmentEnd(Context.SlotIndex);
            }
        }

        // A child that parked on an event leaves its parent above the floor; it must run before the floor drops.
        void DrainAboveFloor(FContext& Context)
        {
            bool bRan = true;
            while (bRan)
            {
                bRan = false;
                for (uint32 Band = 0; Band < kNumJobPriorities; ++Band)
                {
                    while (FFrame* Frame = Context.Slot->Bands[Band].Pop())
                    {
                        Run(Context, FFound{ Frame, false });
                        bRan = true;
                    }
                }
            }
        }

        void WorkerMain(uint32 Index)
        {
            FContext& Context = GContext;
            Context.Generation = GGeneration;
            Context.SlotIndex = Index;
            Context.Slot      = &GRuntime->Slots[Index];
            Context.bWorker   = true;
            Context.Stack     = AcquireStack(Context);
            Context.AllocStack = Context.Stack;

            GRuntime->Searching.fetch_add(1, std::memory_order_relaxed);
            uint32 Spins = 0;
            while (!GRuntime->bStopping.load(std::memory_order_acquire))
            {
                if (const FFound Found = FindWork(Context, kNumJobPriorities))
                {
                    if (GRuntime->Searching.fetch_sub(1, std::memory_order_acq_rel) == 1 && GRuntime->Sleepers.load(std::memory_order_relaxed) != 0)
                    {
                        WakeOneWorker();
                    }
                    Run(Context, Found);
                    GRuntime->Searching.fetch_add(1, std::memory_order_relaxed);
                    Spins = 0;
                    continue;
                }

                if (++Spins < kSpinsBeforeIdle)
                {
                    CpuPause();
                    continue;
                }
                Spins = 0;

                GRuntime->Searching.fetch_sub(1, std::memory_order_seq_cst);
                GRuntime->Sleepers.fetch_add(1, std::memory_order_seq_cst);
                const uint32 Seen = GRuntime->WorkSignal.load(std::memory_order_seq_cst);
                if (GHasHeavyFence)
                {
                    Threading::ProcessWideMemoryBarrier();
                }
                if (!AnyWorkVisible() && !GRuntime->bStopping.load(std::memory_order_acquire))
                {
                    if (GRuntime->Hooks.IdleBegin != nullptr)
                    {
                        GRuntime->Hooks.IdleBegin(Index);
                    }
                    WaitOnWord(GRuntime->WorkSignal, Seen, kIdleTimeoutMs);
                    if (GRuntime->Hooks.IdleEnd != nullptr)
                    {
                        GRuntime->Hooks.IdleEnd(Index);
                    }
                }
                GRuntime->Sleepers.fetch_sub(1, std::memory_order_relaxed);
                GRuntime->Searching.fetch_add(1, std::memory_order_relaxed);
            }
            GRuntime->Searching.fetch_sub(1, std::memory_order_relaxed);
        }

        void BuildDefaultStealOrder()
        {
            for (uint32 Self = 0; Self < GRuntime->NumSlots; ++Self)
            {
                uint32 Fill = 0;
                for (uint32 Offset = 1; Offset < GRuntime->NumSlots; ++Offset)
                {
                    GRuntime->StealOrder[Self][Fill++] = static_cast<uint16>((Self + Offset) % GRuntime->NumSlots);
                }
            }
        }
    }

    using namespace ForkRuntimeInternal;

    namespace ForkDetail
    {
        void* AllocateFrame(std::size_t Size)
        {
            return GContext.AllocStack->Allocate(Size);
        }

        void FreeFrame(void* Memory, std::size_t Size)
        {
            GContext.Stack->Free(Memory, Size);
        }

        void BindCall(FFrame& Parent, FFrame& Child)
        {
            Child.Parent = &Parent;
            Child.Stack  = GContext.Stack;
            Child.Kind   = EFrameKind::Call;
            Child.Band   = Parent.Band;
        }

        void BindFork(FFrame& Parent, FFrame& Child)
        {
            FContext& Context = GContext;
            Child.Parent = &Parent;
            Child.Stack  = Context.Stack;
            Child.Kind   = EFrameKind::Fork;
            Child.Band   = Parent.Band;
            Context.Slot->Bands[static_cast<uint32>(Parent.Band)].Push(&Parent);
            NotifyWorkAvailable();
        }

        std::coroutine_handle<> Finish(FFrame& Frame)
        {
            FContext& Context = GContext;
            FFrame* Parent = Frame.Parent;

            switch (Frame.Kind)
            {
            case EFrameKind::Call:
                Frame.Handle.destroy();
                return Parent->Handle;

            case EFrameKind::Fork:
            {
                const uint32 Band = static_cast<uint32>(Frame.Band);
                Frame.Handle.destroy();
                if (FFrame* Waiting = Context.Slot->Bands[Band].Pop())
                {
                    return Waiting->Handle;
                }

                // Read before the decrement, after which the parent may already be running elsewhere.
                FStack* const ParentStack = Parent->Stack;
                if (Parent->Joins.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    AdoptStack(Context, ParentStack);
                    return Parent->Handle;
                }
                if (Context.Stack == ParentStack)
                {
                    Context.Stack = AcquireStack(Context);
                    Context.AllocStack = Context.Stack;
                }
                return std::noop_coroutine();
            }

            case EFrameKind::Root:
            {
                const FRootLink Link = Frame.Root;
                Frame.Handle.destroy();
                if (Link.Done != nullptr)
                {
                    Link.Done->store(1, std::memory_order_release);
                    WakeAll(*Link.Done);
                }
                if (Link.OnDone != nullptr)
                {
                    Link.OnDone(Link.OnDoneCtx);
                }
                if (Link.Done == nullptr && GRuntime->LiveRoots.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    WakeAll(GRuntime->LiveRoots);
                }
                return std::noop_coroutine();
            }
            }
            return std::noop_coroutine();
        }

        bool SuspendAtJoin(FFrame& Frame)
        {
            FContext& Context = GContext;
            if (Context.Stack == Frame.Stack)
            {
                Context.Stack = AcquireStack(Context);
                Context.AllocStack = Context.Stack;
            }
            const uint32 Outstanding = FFrame::kJoinInit - Frame.Steals;
            if (Frame.Joins.fetch_sub(Outstanding, std::memory_order_acq_rel) == Outstanding)
            {
                AdoptStack(Context, Frame.Stack);
                return false;
            }
            return true;
        }

        FStack* BeginRoot()
        {
            FContext& Context = EnsureContext();
            FStack* Stack = AcquireStack(Context);
            Context.AllocStack = Stack;
            return Stack;
        }

        void EndRoot(FFrame& Root, FStack* Stack, EJobPriority Band)
        {
            ASSERT(static_cast<uint32>(Band) < kNumJobPriorities);
            FContext& Context = GContext;
            Context.AllocStack = Context.Stack;
            Root.Stack = Stack;
            Root.Kind  = EFrameKind::Root;
            Root.Band  = Band;
        }

        // Floors keep this wait off the outer task's continuations, and the swapped stack keeps the outer frames intact.
        struct FNestedWait
        {
            explicit FNestedWait(FContext& InContext) : Context(InContext)
            {
                for (uint32 Band = 0; Band < kNumJobPriorities; ++Band)
                {
                    SavedFloors[Band] = Context.Slot->Bands[Band].RaiseFloor();
                }
                SavedStack = Context.Stack;
                Context.Stack = AcquireStack(Context);
                Context.AllocStack = Context.Stack;
            }

            ~FNestedWait()
            {
                DrainAboveFloor(Context);
                ReleaseStack(Context, Context.Stack);
                Context.Stack = SavedStack;
                Context.AllocStack = SavedStack;
                for (uint32 Band = 0; Band < kNumJobPriorities; ++Band)
                {
                    Context.Slot->Bands[Band].RestoreFloor(SavedFloors[Band]);
                }
            }

            FContext& Context;
            FStack*   SavedStack = nullptr;
            int64     SavedFloors[kNumJobPriorities] = {};
        };

        void HelpLoop(FContext& Context, FWaitDone IsDone, const void* DoneContext, const std::atomic<uint32>& SleepWord, uint32 BandLimit)
        {
            uint32 Spins = 0;
            while (!IsDone(DoneContext))
            {
                if (const FFound Found = FindWork(Context, BandLimit))
                {
                    Run(Context, Found);
                    Spins = 0;
                    continue;
                }
                if (++Spins < kSpinsBeforeIdle)
                {
                    CpuPause();
                    continue;
                }
                Spins = 0;
                const uint32 Seen = SleepWord.load(std::memory_order_acquire);
                if (!IsDone(DoneContext))
                {
                    WaitOnWord(SleepWord, Seen, kWaitTimeoutMs);
                }
            }
        }

        void RunRootAndWait(FFrame& Root)
        {
            FContext& Context = EnsureContext();
            FNestedWait Nested(Context);

            // A background root still helps its own band, or its caller would sit idle while workers do all of it.
            const uint32 OwnBand   = static_cast<uint32>(Root.Band) + 1;
            const uint32 BandLimit = OwnBand > static_cast<uint32>(EJobPriority::Background) ? OwnBand : static_cast<uint32>(EJobPriority::Background);

            std::atomic<uint32> Done{0};
            Root.Root.Done = &Done;
            AdoptStack(Context, Root.Stack);
            ++Context.TaskDepth;
            Root.Handle.resume();
            --Context.TaskDepth;

            const auto IsDone = [](const void* Word) { return static_cast<const std::atomic<uint32>*>(Word)->load(std::memory_order_acquire) != 0; };
            HelpLoop(Context, IsDone, &Done, Done, BandLimit);
        }

        void HelpUntil(FWaitDone IsDone, const void* DoneContext, const std::atomic<uint32>& SleepWord, uint32 BandLimit)
        {
            if (IsDone(DoneContext))
            {
                return;
            }
            FContext& Context = EnsureContext();
            FNestedWait Nested(Context);
            HelpLoop(Context, IsDone, DoneContext, SleepWord, BandLimit);
        }

        void SpawnRoot(FFrame& Root)
        {
            GRuntime->LiveRoots.fetch_add(1, std::memory_order_acq_rel);
            Inject(Root);
        }
    }

    namespace ForkDetail
    {
        void ParkForEvent(FFrame& Frame)
        {
            FContext& Context = GContext;
            if (Context.Stack == Frame.Stack)
            {
                Context.Stack = AcquireStack(Context);
                Context.AllocStack = Context.Stack;
            }
        }

        void ReclaimParked(FFrame& Frame)
        {
            if (Frame.Steals == 0)
            {
                AdoptStack(GContext, Frame.Stack);
            }
        }

        void ResumeParked(FFrame& Frame)
        {
            Inject(Frame);
        }
    }

    bool FTaskEvent::Enqueue(FFrame& Frame)
    {
        ForkDetail::ParkForEvent(Frame);
        FFrame* Head = Waiters.load(std::memory_order_acquire);
        do
        {
            if (Head == kSignaled)
            {
                ForkDetail::ReclaimParked(Frame);
                return false;
            }
            Frame.NextInjected = Head;
        }
        while (!Waiters.compare_exchange_weak(Head, &Frame, std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }

    // The word is written before waiters are released, so nothing this call does can land after a waiter's Reset.
    void FTaskEvent::Signal()
    {
        Word.store(1, std::memory_order_release);
        FFrame* Head = Waiters.exchange(kSignaled, std::memory_order_acq_rel);
        WakeAll(Word);
        while (Head != nullptr && Head != kSignaled)
        {
            FFrame* Next = Head->NextInjected;
            ForkDetail::ResumeParked(*Head);
            Head = Next;
        }
    }

    void FTaskEvent::Wait() const
    {
        const auto IsDone = [](const void* Event) { return static_cast<const FTaskEvent*>(Event)->IsSignaled(); };
        ForkDetail::HelpUntil(IsDone, this, Word, static_cast<uint32>(EJobPriority::Background));
    }

    void FTaskEvent::Reset()
    {
        Word.store(0, std::memory_order_relaxed);
        Waiters.store(nullptr, std::memory_order_release);
    }

    void StartRuntime(const FRuntimeConfig& Config)
    {
        GRuntime = Memory::New<FRuntime>();
        ++GGeneration;
        const uint32 Hardware = Threading::GetNumThreads();
        GRuntime->NumWorkers  = Config.NumWorkers != FRuntimeConfig::kAutoWorkers ? Config.NumWorkers : (Hardware > 1 ? Hardware - 1 : 1);
        GRuntime->NumExternal = Config.NumExternalThreads != 0 ? Config.NumExternalThreads : 1;
        GRuntime->NumSlots    = GRuntime->NumWorkers + GRuntime->NumExternal;
        GRuntime->ExternalSlotsFree.store(GRuntime->NumExternal >= 64 ? ~0ull : ((1ull << GRuntime->NumExternal) - 1), std::memory_order_relaxed);

        GRuntime->Slots = static_cast<FSlot*>(Memory::Malloc(sizeof(FSlot) * GRuntime->NumSlots, alignof(FSlot)));
        for (uint32 Index = 0; Index < GRuntime->NumSlots; ++Index)
        {
            new (&GRuntime->Slots[Index]) FSlot();
        }

        GRuntime->Hooks = Config.Hooks;
        if (Config.BuildStealOrder != nullptr)
        {
            Config.BuildStealOrder(GRuntime->NumWorkers, GRuntime->NumSlots, GRuntime->StealOrder);
        }
        else
        {
            BuildDefaultStealOrder();
        }
        GHasHeavyFence = Threading::ProcessWideMemoryBarrier();

        for (uint32 Index = 0; Index < GRuntime->NumWorkers; ++Index)
        {
            char Name[32];
            std::snprintf(Name, sizeof(Name), "Lumina Worker %u", Index);
            GRuntime->Threads[Index] = Memory::New<FThread>(FThreadOptions{ Name, Threading::ThreadGroup_Worker }, &WorkerMain, Index);
        }
    }

    void StopRuntime()
    {
        if (GRuntime == nullptr)
        {
            return;
        }
        GRuntime->bStopping.store(true, std::memory_order_release);
        GRuntime->WorkSignal.fetch_add(1, std::memory_order_relaxed);
        WakeAll(GRuntime->WorkSignal);
        for (uint32 Index = 0; Index < GRuntime->NumWorkers; ++Index)
        {
            if (GRuntime->Threads[Index]->joinable())
            {
                GRuntime->Threads[Index]->join();
            }
            Memory::Delete(GRuntime->Threads[Index]);
        }
        for (uint32 Index = 0; Index < GRuntime->NumSlots; ++Index)
        {
            GRuntime->Slots[Index].~FSlot();
        }
        void* SlotMemory = GRuntime->Slots;
        Memory::Free(SlotMemory);
        ReturnContext(GContext);
        while (FStack* Stack = GRuntime->FreeStacks)
        {
            GRuntime->FreeStacks = Stack->NextFree;
            Memory::Delete(Stack);
        }
        Memory::Delete(GRuntime);
        GRuntime = nullptr;
    }

    bool IsRuntimeRunning() { return GRuntime != nullptr; }
    uint32 GetNumWorkers() { return GRuntime != nullptr ? GRuntime->NumWorkers : 0; }
    uint32 GetNumThreadSlots() { return GRuntime != nullptr ? GRuntime->NumSlots : 0; }
    uint32 GetThreadSlot() { return EnsureContext().SlotIndex; }
    bool IsWorkerThread() { return GContext.bWorker; }

    void ReleaseThreadSlot()
    {
        FContext& Context = GContext;
        ReturnContext(Context);
        Context = FContext();
    }

    FTaskIsolation::FTaskIsolation() { ++GIsolationDepth; }
    FTaskIsolation::~FTaskIsolation() { --GIsolationDepth; }

    void WaitForSpawned()
    {
        if (GContext.TaskDepth != 0)
        {
            LOG_ERROR("Jobs::WaitForSpawned called from inside a task would wait on itself; ignored.");
            return;
        }
        const auto IsDone = [](const void*) { return GRuntime->LiveRoots.load(std::memory_order_acquire) == 0; };
        ForkDetail::HelpUntil(IsDone, nullptr, GRuntime->LiveRoots, kNumJobPriorities);
    }

    void GetRuntimeStats(FRuntimeStats& Out)
    {
        Out = FRuntimeStats();
        if (GRuntime == nullptr)
        {
            return;
        }
        for (uint32 Band = 0; Band < kNumJobPriorities; ++Band)
        {
            Out.QueuedRoots[Band] = GRuntime->Injected[Band].Count.load(std::memory_order_relaxed);
            for (uint32 Slot = 0; Slot < GRuntime->NumSlots; ++Slot)
            {
                Out.QueuedForks[Band] += static_cast<uint32>(GRuntime->Slots[Slot].Bands[Band].Size());
            }
        }
        Out.LiveRoots = GRuntime->LiveRoots.load(std::memory_order_relaxed);
        Out.Sleepers  = GRuntime->Sleepers.load(std::memory_order_relaxed);
    }
}
