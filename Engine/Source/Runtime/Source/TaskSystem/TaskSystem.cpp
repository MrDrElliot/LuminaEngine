#include "RuntimePCH.h"
#include "TaskSystem.h"

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
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

        // W worker jobs pulling ranges off one atomic cursor: 4x less queue/counter/fiber traffic than W*4 pre-split chunk jobs, at identical balance.
        struct FCursorFor
        {
            FRawThunk Thunk = nullptr;
            void*     Ctx   = nullptr;
            uint32    Num   = 0;
            uint32    Grain = 1;
            alignas(64) TAtomic<uint32> Cursor{0};
        };

        void RunCursorRanges(FCursorFor& C)
        {
            for (;;)
            {
                const uint32 Start = C.Cursor.fetch_add(C.Grain, std::memory_order_relaxed);
                if (Start >= C.Num)
                {
                    return;
                }
                const uint32 End = C.Num - Start < C.Grain ? C.Num : Start + C.Grain;
                // Re-read the slot per range: the thunk may wait inside, and a resumed fiber can migrate.
                C.Thunk(C.Ctx, Start, End, Jobs::GetWorkerIndex());
            }
        }

        void RunCursorJob(void* Arg, uint32 /*Worker*/)
        {
            RunCursorRanges(*static_cast<FCursorFor*>(Arg));
        }


        // Fire-and-forget task backing Task::AsyncTask. Owns the user function + chunk storage and
        // self-destructs once its counter drains.
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
                    // Park-capable: AsyncTask runs arbitrary user work, including asset loads that block on a
                    // fiber-aware lock. A ParallelFor chunk is compute and stays native.
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
            Config.NumWorkerThreads   = Hardware > 3 ? Hardware - 2 : 1; // leave headroom for main + render
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

        if (Num <= Grain)
        {
            Thunk(Ctx, 0, Num, Jobs::GetWorkerIndex());
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

        Jobs::FJobDecl Decls[kMaxChunks];
        for (uint32 i = 0; i < K; ++i)
        {
            Decls[i] = Jobs::FJobDecl{ &RunCursorJob, &C, "Task::ParallelFor" };
        }

        Jobs::FCounter* Counter = Jobs::AllocCounter(0);
        Jobs::RunJobs(Decls, K, ToJobPriority(Priority), Counter);
        // The caller works the cursor too instead of parking for the whole fan-out.
        RunCursorRanges(C);
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
