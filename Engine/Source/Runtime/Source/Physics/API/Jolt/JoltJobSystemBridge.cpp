#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "JoltJobSystemBridge.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

#include <thread>

namespace Lumina::Physics
{
    FJoltJobSystemBridge::FJoltJobSystemBridge(JPH::uint InMaxJobs, JPH::uint InMaxBarriers, int InMaxConcurrency)
        : JobSystemWithBarrier(InMaxBarriers)
        , MaxConcurrency(InMaxConcurrency < 1 ? 1 : InMaxConcurrency)
    {
        JobPool.Init(InMaxJobs, InMaxJobs); // max objects + page size (single page, like JobSystemThreadPool)
    }

    void FJoltJobSystemBridge::RunJoltJob(void* Arg, JPH::uint32 /*WorkerIndex*/)
    {
        LUMINA_PROFILE_SCOPE();
        Job* TheJob = static_cast<Job*>(Arg);
        TheJob->Execute();   // execute-once: a no-op if the waiting thread already ran it
        TheJob->Release();   // drop the reference taken in QueueJob/QueueJobs
    }

    auto FJoltJobSystemBridge::CreateJob(const char* InName, JPH::ColorArg InColor, const JobFunction& InJobFunction, JPH::uint32 InNumDependencies) -> JobHandle
    {
        JPH::uint32 Index;
        for (;;)
        {
            Index = JobPool.ConstructObject(InName, InColor, this, InJobFunction, InNumDependencies);
            if (Index != FAvailableJobs::cInvalidObjectIndex)
            {
                break;
            }
            JPH_ASSERT(false, "No jobs available!");
            PlatformTime::SleepMicroseconds(100);
        }

        Job* TheJob = &JobPool.Get(Index);

        // Take a handle reference before (possibly) queuing, the job may complete immediately once queued.
        JobHandle Handle(TheJob);

        if (InNumDependencies == 0)
        {
            QueueJob(TheJob);
        }

        return Handle;
    }

    void FJoltJobSystemBridge::QueueJob(Job* InJob)
    {
        // The scheduler queue holds a reference until RunJoltJob releases it.
        InJob->AddRef();

        Jobs::FJobDecl Decl{ &RunJoltJob, InJob, "JoltJob" };
        Jobs::RunJobs(&Decl, 1, Jobs::EJobPriority::Normal, nullptr);
    }

    void FJoltJobSystemBridge::QueueJobs(Job** InJobs, JPH::uint InNumJobs)
    {
        // Bulk-enqueue in batches so one RunJobs call amortizes the queue bookkeeping over many jobs.
        constexpr JPH::uint kBatch = 256;
        Jobs::FJobDecl Decls[kBatch];

        for (JPH::uint Base = 0; Base < InNumJobs; Base += kBatch)
        {
            const JPH::uint N = (InNumJobs - Base) < kBatch ? (InNumJobs - Base) : kBatch;
            for (JPH::uint i = 0; i < N; ++i)
            {
                InJobs[Base + i]->AddRef();
                Decls[i] = Jobs::FJobDecl{ &RunJoltJob, InJobs[Base + i], "JoltJob" };
            }
            Jobs::RunJobs(Decls, N, Jobs::EJobPriority::Normal, nullptr);
        }
    }

    void FJoltJobSystemBridge::FreeJob(Job* InJob)
    {
        JobPool.DestructObject(InJob);
    }
}
