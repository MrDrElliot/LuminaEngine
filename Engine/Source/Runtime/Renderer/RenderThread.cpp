#include "pch.h"
#include "RenderThread.h"

#include "Core/Assertions/Assert.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Core/Profiler/Profile.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"
#include "TaskSystem/Scheduler/JobScheduler.h"

#include <chrono>


namespace Lumina
{
    RUNTIME_API FRenderThread* GRenderThread = nullptr;

    // True while the calling worker is running the render drain (and therefore a render command). The
    // drain is serial and does not yield to the scheduler, so it never migrates off its worker, a
    // thread_local is correct. See FRenderThread::IsInRenderStage().
    static thread_local bool GbInRenderDrain = false;

    bool FRenderThread::IsInRenderStage()
    {
        return GbInRenderDrain;
    }

    FRenderThread& FRenderThread::Get()
    {
        DEBUG_ASSERT(GRenderThread != nullptr);
        return *GRenderThread;
    }

    FRenderThread::FRenderThread()
    {
        PendingCommands.reserve(256);
    }

    FRenderThread::~FRenderThread()
    {
        Stop();
    }

    // These few atomics distinguish the drain failure modes at a glance: armed-but-not-running = the
    // scheduler stranded the drain job; running with a stuck command name = the command is blocked
    // (or its fiber parked -- see the job-system reporter for which counter).
    void FRenderThread::ReportForHangWatchdog()
    {
        FRenderThread* RT = GRenderThread;
        if (RT == nullptr)
        {
            return;
        }
        const char* Command = RT->ActiveCommandName.load(Atomic::MemoryOrderAcquire);
        LOG_ERROR("Render drain: running={} armed={} enqueued={} completed={} command='{}'",
            RT->bDrainRunning.load(Atomic::MemoryOrderAcquire),
            RT->bDrainJobArmed.load(Atomic::MemoryOrderAcquire),
            RT->EnqueuedCount(), RT->CompletedCount(),
            Command != nullptr ? Command : "<none>");
    }

    void FRenderThread::Start()
    {
        if (bRunning.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }
        DrainCounter = Jobs::AllocCounter(0);
        bRunning.store(true, Atomic::MemoryOrderRelease);

        static bool bReporterRegistered = false;
        if (!bReporterRegistered)
        {
            bReporterRegistered = true;
            HangWatchdog::RegisterReporter(&FRenderThread::ReportForHangWatchdog);
        }
    }

    void FRenderThread::Stop()
    {
        if (!bRunning.load(Atomic::MemoryOrderAcquire))
        {
            return;
        }

        Flush();                              // all enqueued commands completed
        Jobs::WaitForCounter(DrainCounter, 0); // the drain job itself has fully exited (no more 'this' use)

        bRunning.store(false, Atomic::MemoryOrderRelease);

        if (DrainCounter != nullptr)
        {
            Jobs::FreeCounter(DrainCounter);
            DrainCounter = nullptr;
        }
    }

    void FRenderThread::ArmDrain()
    {
        // At most one armed drain job in flight. Armed is cleared when the job STARTS (DrainEntry), not
        // when the drain finishes: a running drain re-checks the queue before exiting, so an Enqueue
        // racing a live drain needs no new job -- and a job the scheduler strands can never lock out
        // the WaitForCounter assist, because bDrainRunning stays false.
        bool Expected = false;
        if (bDrainJobArmed.compare_exchange_strong(Expected, true, Atomic::MemoryOrderAcqRel))
        {
            Jobs::RunJob(&FRenderThread::DrainEntry, this, Jobs::EJobPriority::High, DrainCounter, "RenderFrame");
        }
    }

    void FRenderThread::Enqueue(const char* DebugName, FCommand&& Cmd)
    {
        // Not started (boot / shutdown): run inline so the GPU effect is immediate and nobody waits on
        // a dead system.
        if (!bRunning.load(Atomic::MemoryOrderAcquire))
        {
            CommandsEnqueued.fetch_add(1, Atomic::MemoryOrderAcqRel);
            RunCommand(DebugName, Cmd);
            CommandsCompleted.fetch_add(1, Atomic::MemoryOrderAcqRel);
            return;
        }

        {
            std::unique_lock Lock(QueueMutex);
            PendingCommands.push_back({ DebugName, Move(Cmd) });
            CommandsEnqueued.fetch_add(1, Atomic::MemoryOrderAcqRel);
        }
        ArmDrain();
    }

    void FRenderThread::EnqueueAndWait(const char* DebugName, FCommand&& Cmd)
    {
        // Already inside the drain (a render command calling back in): run inline, can't wait on self.
        if (!bRunning.load(Atomic::MemoryOrderAcquire) || IsInRenderStage())
        {
            CommandsEnqueued.fetch_add(1, Atomic::MemoryOrderAcqRel);
            RunCommand(DebugName, Cmd);
            CommandsCompleted.fetch_add(1, Atomic::MemoryOrderAcqRel);
            return;
        }

        FRenderCommandFence Fence;
        Enqueue(DebugName, Move(Cmd));
        Fence.BeginFence();
        Fence.Wait();
    }

    void FRenderThread::Flush()
    {
        if (!bRunning.load(Atomic::MemoryOrderAcquire) || IsInRenderStage())
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        WaitForCounter(CommandsEnqueued.load(Atomic::MemoryOrderAcquire));
    }

    void FRenderThread::WaitForCounter(uint64 Target)
    {
        if (!bRunning.load(Atomic::MemoryOrderAcquire) || IsInRenderStage())
        {
            return;
        }
        
        for (;;)
        {
            if (CommandsCompleted.load(Atomic::MemoryOrderAcquire) >= Target)
            {
                return;
            }

            bool Expected = false;
            if (bDrainRunning.compare_exchange_strong(Expected, true, Atomic::MemoryOrderAcqRel))
            {
                DrainLoop();
                continue;
            }

            std::unique_lock Lock(IdleMutex);
            IdleCV.wait_for(Lock, std::chrono::milliseconds(2), [this, Target]()
            {
                return CommandsCompleted.load(Atomic::MemoryOrderAcquire) >= Target;
            });
        }
    }

    void FRenderThread::DrainEntry(void* Arg, uint32 /*WorkerIndex*/)
    {
        FRenderThread* Self = static_cast<FRenderThread*>(Arg);

        // The armed job has started; a fresh Enqueue may arm a new one from here on.
        Self->bDrainJobArmed.store(false, Atomic::MemoryOrderRelease);

        // Become the drainer -- unless an assisting waiter already is. The loser just exits; the live
        // drainer re-checks the queue before exiting, so nothing is stranded.
        bool Expected = false;
        if (Self->bDrainRunning.compare_exchange_strong(Expected, true, Atomic::MemoryOrderAcqRel))
        {
            Self->DrainLoop();
        }
    }

    // Callers must own bDrainRunning (won the CAS); DrainLoop releases it on exit.
    void FRenderThread::DrainLoop()
    {
        GbInRenderDrain = true;
        // Tripwire: render commands must never fiber-park (the drain is serial; a park strands every
        // Flush waiter, and the fiber can resume on another thread, breaking GbInRenderDrain).
        Jobs::SetThreadNoParkGuard("render drain");

        TVector<FQueuedCommand> Batch;
        for (;;)
        {
            {
                std::unique_lock Lock(QueueMutex);
                Batch.swap(PendingCommands);
            }

            if (Batch.empty())
            {
                // Tentatively done. Re-check under the run flag so a command racing in isn't stranded.
                bDrainRunning.store(false, Atomic::MemoryOrderRelease);

                bool HasMore;
                {
                    std::unique_lock Lock(QueueMutex);
                    HasMore = !PendingCommands.empty();
                }
                if (HasMore)
                {
                    bool Expected = false;
                    if (bDrainRunning.compare_exchange_strong(Expected, true, Atomic::MemoryOrderAcqRel))
                    {
                        continue; // re-armed self
                    }
                }
                break; // queue empty, or another drainer took over
            }

            for (FQueuedCommand& Q : Batch)
            {
                RunCommand(Q.DebugName, Q.Cmd);
                CommandsCompleted.fetch_add(1, Atomic::MemoryOrderAcqRel);
            }
            Batch.clear();

            {
                std::unique_lock Lock(IdleMutex);
                IdleCV.notify_all();
            }
        }

        Jobs::SetThreadNoParkGuard(nullptr);
        GbInRenderDrain = false;
    }

    void FRenderThread::RunCommand(const char* DebugName, FCommand& Cmd)
    {
        LUMINA_PROFILE_SCOPE_COLORED(tracy::Color::SteelBlue);
        LUMINA_PROFILE_TAG(DebugName);
        ActiveCommandName.store(DebugName, Atomic::MemoryOrderRelease);
        Cmd();
        ActiveCommandName.store(nullptr, Atomic::MemoryOrderRelease);
    }


    void FRenderCommandFence::BeginFence()
    {
        TargetCounter = GRenderThread != nullptr
            ? GRenderThread->EnqueuedCount()
            : 0;
    }

    void FRenderCommandFence::Wait()
    {
        if (TargetCounter == 0 || GRenderThread == nullptr)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();
        GRenderThread->WaitForCounter(TargetCounter);
    }

    bool FRenderCommandFence::IsComplete() const
    {
        if (TargetCounter == 0 || GRenderThread == nullptr)
        {
            return true;
        }
        return GRenderThread->CompletedCount() >= TargetCounter;
    }
}
