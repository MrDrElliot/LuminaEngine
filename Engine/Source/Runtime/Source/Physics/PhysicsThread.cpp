#include "RuntimePCH.h"
#include "PhysicsThread.h"

#include "Core/Assertions/Assert.h"
#include "Core/Profiler/Profile.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Memory/Memory.h"
#include "TaskSystem/Scheduler/JobScheduler.h"


namespace Lumina
{
    RUNTIME_API FPhysicsThread* GPhysicsThread = nullptr;

    FPhysicsThread& FPhysicsThread::Get()
    {
        DEBUG_ASSERT(GPhysicsThread != nullptr);
        return *GPhysicsThread;
    }

    FPhysicsThread::FPhysicsThread() = default;

    FPhysicsThread::~FPhysicsThread()
    {
        Stop();
    }

    void FPhysicsThread::Start()
    {
        if (bRunning.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }
        JobCounter = Jobs::AllocCounter(0);
        bRunning.store(true, Atomic::MemoryOrderRelease);
    }

    void FPhysicsThread::Stop()
    {
        if (!bRunning.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }

        Flush();
        bRunning.store(false, Atomic::MemoryOrderRelease);

        if (JobCounter != nullptr)
        {
            Jobs::FreeCounter(JobCounter);
            JobCounter = nullptr;
        }
    }

    void FPhysicsThread::RunJobEntry(void* Arg, uint32 /*WorkerIndex*/)
    {
        FJobCtx* Ctx = static_cast<FJobCtx*>(Arg);
        Ctx->Cmd();
        Ctx->Owner->CommandsCompleted.fetch_add(1, Atomic::MemoryOrderAcqRel);
        Memory::Delete(Ctx);
    }

    void FPhysicsThread::Enqueue(const char* DebugName, FCommand&& Cmd)
    {
        // Not started (boot / shutdown): run inline so callers never deadlock waiting on a dead system.
        if (!bRunning.load(Atomic::MemoryOrderAcquire))
        {
            CommandsEnqueued.fetch_add(1, Atomic::MemoryOrderAcqRel);
            Cmd();
            CommandsCompleted.fetch_add(1, Atomic::MemoryOrderAcqRel);
            return;
        }

        CommandsEnqueued.fetch_add(1, Atomic::MemoryOrderAcqRel);

        FJobCtx* Ctx = Memory::New<FJobCtx>();
        Ctx->Owner = this;
        Ctx->Cmd   = Move(Cmd);

        // High priority: the physics step is on the critical path to next FrameStart's join.
        Jobs::RunJob(&FPhysicsThread::RunJobEntry, Ctx, Jobs::EJobPriority::High, JobCounter, DebugName);
    }

    void FPhysicsThread::EnqueueAndWait(const char* DebugName, FCommand&& Cmd)
    {
        Enqueue(DebugName, Move(Cmd));
        Flush();
    }

    void FPhysicsThread::Flush()
    {
        if (!bRunning.load(Atomic::MemoryOrderAcquire) || JobCounter == nullptr)
        {
            return;
        }
        LUMINA_PROFILE_SCOPE();
        // Assist-waits when called from the main thread; parks when called from a worker fiber.
        Jobs::WaitForCounter(JobCounter, 0);
    }

    void FPhysicsThread::WaitForCounter(uint64 /*Target*/)
    {
        // All physics work shares one counter; draining it satisfies any per-command target.
        Flush();
    }


    void FPhysicsCommandFence::BeginFence()
    {
        TargetCounter = GPhysicsThread != nullptr
            ? GPhysicsThread->EnqueuedCount()
            : 0;
    }

    void FPhysicsCommandFence::Wait()
    {
        if (TargetCounter == 0 || GPhysicsThread == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();
        GPhysicsThread->WaitForCounter(TargetCounter);
    }

    bool FPhysicsCommandFence::IsComplete() const
    {
        if (TargetCounter == 0 || GPhysicsThread == nullptr)
        {
            return true;
        }
        return GPhysicsThread->CompletedCount() >= TargetCounter;
    }
}
