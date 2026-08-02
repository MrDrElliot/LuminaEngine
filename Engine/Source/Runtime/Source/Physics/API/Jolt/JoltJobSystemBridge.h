#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

namespace Lumina::Physics
{
    // Bridges Jolt's JobSystem onto the engine fiber scheduler (Lumina::Jobs). Each Jolt job is enqueued as
    // a detached (counter-free) engine job, so physics work runs on the shared worker pool instead of a
    // separate Jolt thread pool, removing oversubscription and routing physics through the same Tracy
    // timeline as everything else. JobSystemWithBarrier supplies the barrier/dependency machinery; its
    // Wait() still executes runnable jobs on the waiting (physics) thread, and Jolt's Job::Execute() is
    // CAS-guarded execute-once, so a worker and the waiter racing the same job is safe.
    class FJoltJobSystemBridge final : public JPH::JobSystemWithBarrier
    {
    public:

        // InMaxConcurrency caps how finely Jolt splits work (it batches into ~GetMaxConcurrency chunks);
        // lower values mean fewer, larger jobs and thus less per-job scheduling overhead.
        FJoltJobSystemBridge(JPH::uint InMaxJobs, JPH::uint InMaxBarriers, int InMaxConcurrency);
        virtual ~FJoltJobSystemBridge() override = default;

        virtual int       GetMaxConcurrency() const override { return MaxConcurrency; }
        virtual JobHandle CreateJob(const char* InName, JPH::ColorArg InColor, const JobFunction& InJobFunction, JPH::uint32 InNumDependencies = 0) override;

    protected:

        virtual void QueueJob(Job* InJob) override;
        virtual void QueueJobs(Job** InJobs, JPH::uint InNumJobs) override;
        virtual void FreeJob(Job* InJob) override;

    private:

        // Engine-job entry point: run the Jolt job once, then drop the reference taken when queuing.
        static void RunJoltJob(void* Arg, JPH::uint32 WorkerIndex);

        using FAvailableJobs = JPH::FixedSizeFreeList<Job>;
        FAvailableJobs JobPool;
        int            MaxConcurrency = 1;
    };
}
