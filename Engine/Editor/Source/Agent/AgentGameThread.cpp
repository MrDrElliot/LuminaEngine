#include "EditorPCH.h"
#include "Agent/AgentGameThread.h"

#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/ThreadedCallback.h"

namespace Lumina::Agent
{
    namespace
    {
        constexpr int32 GDefaultTimeoutMilliseconds = 5000;

        // Long enough not to spin a transport thread, short enough that a quick tool still feels immediate.
        constexpr uint64 GPollMilliseconds = 1;

        enum class EPhase : uint8
        {
            Pending,
            Running,
            Finished,
            Abandoned,
        };

        struct FGateState
        {
            TAtomic<EPhase> Phase { EPhase::Pending };
        };
    }

    int32 FGameThreadGate::GetDefaultTimeoutMilliseconds()
    {
        return GDefaultTimeoutMilliseconds;
    }

    EGameThreadResult FGameThreadGate::Run(TMoveOnlyFunction<void()>&& Work, int32 TimeoutMilliseconds)
    {
        if (!Work)
        {
            return EGameThreadResult::Ran;
        }

        // Hopping to the thread we are already on would wait for a queue only this thread can drain.
        if (Threading::IsMainThread())
        {
            Work();
            return EGameThreadResult::Ran;
        }

        TSharedPtr<FGateState> State = MakeShared<FGateState>();

        MainThread::Enqueue([State, Work = Move(Work)]() mutable
        {
            EPhase Expected = EPhase::Pending;
            if (!State->Phase.compare_exchange_strong(Expected, EPhase::Running, std::memory_order_acq_rel))
            {
                return;
            }

            Work();
            State->Phase.store(EPhase::Finished, std::memory_order_release);
        });

        const double Deadline = PlatformTime::Seconds() + (static_cast<double>(TimeoutMilliseconds) / 1000.0);

        for (;;)
        {
            const EPhase Phase = State->Phase.load(std::memory_order_acquire);

            if (Phase == EPhase::Finished)
            {
                return EGameThreadResult::Ran;
            }

            // Losing this exchange means the work just started, so waiting on beats abandoning live work.
            if (Phase == EPhase::Pending && PlatformTime::Seconds() >= Deadline)
            {
                EPhase Expected = EPhase::Pending;
                if (State->Phase.compare_exchange_strong(Expected, EPhase::Abandoned, std::memory_order_acq_rel))
                {
                    return EGameThreadResult::TimedOut;
                }
            }

            Threading::Sleep(GPollMilliseconds);
        }
    }
}
