#include "RuntimePCH.h"
#include "TaskSystem.h"

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
        MaxChunks = std::min(MaxChunks, Task::kMaxChunks);
        NumChunks = std::min(NumChunks, MaxChunks);

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

        struct FParChunk
        {
            FRawThunk Thunk;
            void*     Ctx;
            uint32    Start;
            uint32    End;
        };

        void RunParChunk(void* Arg, uint32 Worker)
        {
            FParChunk* C = static_cast<FParChunk*>(Arg);
            C->Thunk(C->Ctx, C->Start, C->End, Worker);
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
                    Decls[c]  = Jobs::FJobDecl{ &FAsyncContext::RunChunk, &Chunks[c], "Task::Async" };
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
        const uint32 NumChunks = Task::ComputeChunkCount(Num, MinRange);

        if (NumChunks == 1)
        {
            Thunk(Ctx, 0, Num, Jobs::GetWorkerIndex());
            return;
        }

        FParChunk      Chunks[kMaxChunks];
        Jobs::FJobDecl Decls[kMaxChunks];

        const uint32 Base = Num / NumChunks;
        const uint32 Rem  = Num % NumChunks;
        uint32 Start = 0;
        for (uint32 c = 0; c < NumChunks; ++c)
        {
            const uint32 Len = Base + (c < Rem ? 1u : 0u);
            Chunks[c] = FParChunk{ Thunk, Ctx, Start, Start + Len };
            Decls[c]  = Jobs::FJobDecl{ &RunParChunk, &Chunks[c], "Task::ParallelFor" };
            Start += Len;
        }

        Jobs::FCounter* Counter = Jobs::AllocCounter(0);
        Jobs::RunJobs(Decls, NumChunks, ToJobPriority(Priority), Counter);
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
