#include "RuntimePCH.h"
#include "TaskSystem.h"

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#if USING(WITH_EDITOR)
#include "Scheduler/JobProfiler.h"
#endif
#include <algorithm>
#include <cstdlib>

namespace Lumina
{
    RUNTIME_API FTaskSystem* GTaskSystem = nullptr;

    uint32 Task::ComputeChunkCount(uint32 Num, uint32 MinRange)
    {
        const uint32 Grain = MinRange == 0 ? 1u : MinRange;
        uint32 NumChunks = (Num + Grain - 1) / Grain;

        uint32 MaxChunks = Jobs::GetNumWorkers() * 4u;
        if (MaxChunks == 0)
        {
            MaxChunks = 1;
        }
        MaxChunks = Math::Min(MaxChunks, Task::kMaxChunks);
        NumChunks = Math::Min(NumChunks, MaxChunks);

        if (NumChunks == 0)
        {
            NumChunks = 1;
        }
        return NumChunks;
    }

    namespace
    {
        constexpr uint32 kMaxChunks = Task::kMaxChunks;

        using FRawThunk = void (*)(void* Ctx, uint32 Start, uint32 End, uint32 Thread);

        // Four times less queue and counter traffic than pre-split chunk jobs, at identical balance.
        struct FCursorFor
        {
            FRawThunk Thunk = nullptr;
            void*     Ctx   = nullptr;
            uint32    Num   = 0;
            uint32    Grain = 1;
            alignas(64) TAtomic<uint32> Cursor{0};
        };

        // Items this thread pulled off the cursor, so the caller can price the loop from work it ran anyway.
        uint32 RunCursorRanges(FCursorFor& C)
        {
            uint32 Ran = 0;
            for (;;)
            {
                const uint32 Start = C.Cursor.fetch_add(C.Grain, std::memory_order_relaxed);
                if (Start >= C.Num)
                {
                    return Ran;
                }
                const uint32 End = C.Num - Start < C.Grain ? C.Num : Start + C.Grain;
                // Re-read per range, since the thunk may wait inside and a resumed fiber can migrate.
                C.Thunk(C.Ctx, Start, End, Jobs::GetWorkerIndex());
                Ran += End - Start;
            }
        }

        void RunCursorJob(void* Arg, uint32 /*Worker*/)
        {
            (void)RunCursorRanges(*static_cast<FCursorFor*>(Arg));
        }

        // A count cannot express the crossover, and the thunk address is one instantiation per call site.
        constexpr uint32 kCostSlots         = 256;
        constexpr uint64 kSerialBudgetNanos = 25'000;   // below this, the fan-out costs more than the work

        struct alignas(64) FCostEntry
        {
            TAtomic<uint64> Key{0};
            TAtomic<uint64> NanosPerItem{0};
        };
        FCostEntry GCostTable[kCostSlots];

        FORCEINLINE FCostEntry& CostSlot(FRawThunk Thunk)
        {
            const uint64 Key  = reinterpret_cast<uint64>(Thunk);
            const uint64 Hash = (Key >> 4) * 0x9E3779B97F4A7C15ull;
            return GCostTable[(Hash >> 56) % kCostSlots];
        }

        // Zero when never measured, or when the slot belongs to someone else.
        FORCEINLINE uint64 EstimatedNanosPerItem(FRawThunk Thunk)
        {
            const FCostEntry& Slot = CostSlot(Thunk);
            return Slot.Key.load(std::memory_order_relaxed) == reinterpret_cast<uint64>(Thunk)
                 ? Slot.NanosPerItem.load(std::memory_order_relaxed) : 0;
        }

        // Racy by design, since a stale estimate costs one mis-sized dispatch and never correctness.
        void RecordItemCost(FRawThunk Thunk, uint32 ItemsRun, double Seconds)
        {
            if (ItemsRun == 0 || Seconds <= 0.0)
            {
                return;
            }
            const uint64 Sample = static_cast<uint64>((Seconds * 1'000'000'000.0) / ItemsRun);
            FCostEntry&  Slot   = CostSlot(Thunk);
            const uint64 Key    = reinterpret_cast<uint64>(Thunk);

            if (Slot.Key.load(std::memory_order_relaxed) != Key)
            {
                Slot.Key.store(Key, std::memory_order_relaxed);
                Slot.NanosPerItem.store(Sample, std::memory_order_relaxed);
                return;
            }
            const uint64 Prior = Slot.NanosPerItem.load(std::memory_order_relaxed);
            Slot.NanosPerItem.store(Prior == 0 ? Sample : (Prior * 3 + Sample) / 4, std::memory_order_relaxed);
        }


        // Owns the user function and chunk storage, and self-destructs once its counter drains.
        struct FAsyncContext
        {
            TaskSetFunction           Function;
            TWeakPtr<FTaskCompletion> Handle;
            Jobs::FCounter*           Counter   = nullptr;

            struct FChunk
            {
                FAsyncContext* Ctx;
                uint32         Start;
                uint32         End;
            };
            FChunk* Chunks    = nullptr;
            uint32  NumChunks = 0;

            static void RunChunk(void* Arg, uint32 Worker)
            {
                FChunk* C = static_cast<FChunk*>(Arg);
                C->Ctx->Function(C->Start, C->End, Worker);
            }

            static void OnComplete(void* Raw, uint32 /*Worker*/)
            {
                FAsyncContext* Self = static_cast<FAsyncContext*>(Raw);
                if (TSharedPtr<FTaskCompletion> H = Self->Handle.lock())
                {
                    H->bCompleted.exchange(true, std::memory_order_release);
                    std::atomic_notify_all(&H->bCompleted);
                }
                Jobs::FreeCounter(Self->Counter);
                void* ChunksMem = Self->Chunks;
                Memory::Free(ChunksMem);
                Memory::Delete(Self);
            }

            void Launch(uint32 Num, uint32 MinRange, ETaskPriority Priority)
            {
                NumChunks = Task::ComputeChunkCount(Num, MinRange);
                Chunks    = static_cast<FChunk*>(Memory::Malloc(sizeof(FChunk) * NumChunks, alignof(FChunk)));
                Counter   = Jobs::AllocCounter(0);
                Jobs::SetCounterCompletion(Counter, &FAsyncContext::OnComplete, this);

                Jobs::FJobDecl Decls[kMaxChunks];
                const uint32 Base = Num / NumChunks;
                const uint32 Rem  = Num % NumChunks;
                uint32 Start = 0;
                for (uint32 c = 0; c < NumChunks; ++c)
                {
                    const uint32 Len = Base + (c < Rem ? 1u : 0u);
                    Chunks[c] = FChunk{ this, Start, Start + Len };
                    // AsyncTask runs arbitrary user work, while a ParallelFor chunk is compute and stays native.
                    Decls[c]  = Jobs::FJobDecl{ &FAsyncContext::RunChunk, &Chunks[c], "Task::Async", /*bMayPark*/ true };
                    Start += Len;
                }

                Jobs::RunJobs(Decls, NumChunks, ToJobPriority(Priority), Counter);
            }
        };
    }

    namespace Task
    {
        void Initialize()
        {
            GTaskSystem = Memory::New<FTaskSystem>();

            const uint32 Hardware = Threading::GetNumThreads();

            Jobs::FConfig Config;
            // Crossing the processor count turns an empty fan-out from nanoseconds into microseconds.
            Config.NumWorkerThreads   = Hardware > 4 ? Hardware - (Hardware / 4) - 1 : 1;
            if (const char* WorkersEnv = std::getenv("LUMINA_JOB_WORKERS"))
            {
                const int N = std::atoi(WorkersEnv);
                if (N > 0) Config.NumWorkerThreads = (uint32)N;
            }
            Config.NumExternalThreads = 8;

            Jobs::Initialize(Config);
            GTaskSystem->RegisterExternalThread(); // the main thread gets a stable slot
        }

        void Shutdown()
        {
            Jobs::WaitForAll();
#if USING(WITH_EDITOR)
            FJobProfiler::Get().Shutdown();
#endif
            GTaskSystem->UnregisterExternalThread();
            Jobs::Shutdown();
            Memory::Delete(GTaskSystem);
            GTaskSystem = nullptr;
        }
    }

    void FTaskSystem::ParallelForImpl(uint32 Num, uint32 MinRange, ETaskPriority Priority, FParallelThunk Thunk, void* Ctx)
    {
        const uint32 Grain = Task::ComputeCursorGrain(Num, MinRange);

        // A loop finishing inside the dispatch overhead runs on the caller and submits nothing.
        const uint64 NanosPerItem = EstimatedNanosPerItem(Thunk);
        const bool   bTooSmall    = NanosPerItem != 0 && NanosPerItem * Num <= kSerialBudgetNanos;

        if (Num <= Grain || bTooSmall)
        {
            const double Start = PlatformTime::Seconds();
            Thunk(Ctx, 0, Num, Jobs::GetWorkerIndex());
            RecordItemCost(Thunk, Num, PlatformTime::Seconds() - Start);
            return;
        }

        FCursorFor C;
        C.Thunk = Thunk;
        C.Ctx   = Ctx;
        C.Num   = Num;
        C.Grain = Grain;

        // One job per worker at most, minus the grab the participating caller takes itself.
        const uint32 Grabs = (Num + Grain - 1) / Grain;
        uint32 K = Grabs - 1;
        K = Math::Min(K, Math::Min(Jobs::GetNumWorkers(), Task::kMaxChunks));

        const Jobs::FJobDecl Decl{ &RunCursorJob, &C, "Task::ParallelFor" };

        Jobs::FCounter* Counter = Jobs::AllocCounter(0);
        Jobs::RunJobs(Decl, K, ToJobPriority(Priority), Counter);
        // The work happens either way, so timing the caller's own slices costs two clock reads.
        const double Start = PlatformTime::Seconds();
        const uint32 Ran   = RunCursorRanges(C);
        RecordItemCost(Thunk, Ran, PlatformTime::Seconds() - Start);

        Jobs::WaitForCounter(Counter, 0);
        Jobs::FreeCounter(Counter);
    }

    FTaskHandle FTaskSystem::ScheduleLambda(uint32 Num, uint32 MinRange, TaskSetFunction&& Function, ETaskPriority Priority)
    {
        if (Num == 0)
        {
            LOG_WARN("Task Size of [0] passed to task system.");
            return nullptr;
        }

        FTaskHandle    Handle = MakeShared<FTaskCompletion>();
        FAsyncContext* Ctx    = Memory::New<FAsyncContext>();
        Ctx->Function = Move(Function);
        Ctx->Handle   = Handle;
        Ctx->Launch(Num, MinRange, Priority);

        return Handle;
    }

    void FTaskSystem::WaitForAll()
    {
        Jobs::WaitForAll();
    }

    FTaskHandle Task::AsyncTask(uint32 Num, uint32 MinRange, TaskSetFunction&& Function, ETaskPriority Priority)
    {
        return GTaskSystem->ScheduleLambda(Num, MinRange, Move(Function), Priority);
    }
}
