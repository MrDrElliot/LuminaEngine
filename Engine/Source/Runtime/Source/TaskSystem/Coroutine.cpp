#include "RuntimePCH.h"
#include "Coroutine.h"

#include "ThreadedCallback.h"
#include "Core/Threading/Thread.h"
#include "World/World.h"

namespace Lumina::CoroDetail
{
    void FCoroutineState::Release()
    {
        if (RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            delete this;
        }
    }

    void FCoroutineState::Cancel()
    {
        bCancelRequested.store(true, std::memory_order_release);

        // Copied out under the lock, so a child that finishes meanwhile is still alive when told.
        TVector<FStateRef> Children;
        {
            TScopeLock Guard(Lock);
            for (FCoroutineState* Child : AwaitedChildren)
            {
                Children.Emplace(Child);
            }
        }
        const bool bForce = bForced.load(std::memory_order_acquire);
        for (FStateRef& Child : Children)
        {
            if (bForce)
            {
                Child->ForceCancel();
            }
            else
            {
                Child->Cancel();
            }
        }
    }

    void FCoroutineState::ForceCancel()
    {
        bForced.store(true, std::memory_order_release);
        Cancel();
    }

    bool FCoroutineState::ShouldDieAtSuspension() const
    {
        return bForced.load(std::memory_order_acquire) || (IsCancelRequested() && GuardCount == 0);
    }

    bool FCoroutineState::TryContinueWith(TMoveOnlyFunction<void()>&& Callback)
    {
        TScopeLock Guard(Lock);
        if (IsDone())
        {
            return false;
        }
        Continuations.Add(Move(Callback));
        return true;
    }

    void FCoroutineState::ContinueWith(TMoveOnlyFunction<void()>&& Callback)
    {
        if (!TryContinueWith(Move(Callback)))
        {
            Callback();
        }
    }

    void FCoroutineState::Resume()
    {
        if (ShouldDieAtSuspension())
        {
            DestroyAsCanceled();
            return;
        }
        ASSERT(Handle, "Resumed a coroutine that has already ended.");
        Handle.resume();
    }

    void FCoroutineState::DestroyAsCanceled()
    {
        FStateRef Keep(this);
        std::coroutine_handle<> Frame = Handle;
        if (!Frame)
        {
            return;
        }
        Handle = {};
        bCancelRequested.store(true, std::memory_order_release);
        bDestroyingForCancel = true;
        Frame.destroy();
        Complete(EStatus::Canceled);
    }

    bool FCoroutineState::ClaimWait(uint32 Serial)
    {
        uint32 Expected = Serial;
        return WaitSerial.compare_exchange_strong(Expected, Serial + 1, std::memory_order_acq_rel);
    }

    void FCoroutineState::BindWorld(CWorld* InWorld)
    {
        if (InWorld == nullptr || World != nullptr)
        {
            return;
        }
        World = InWorld;
        World->GetTimers()->GetCoroutineManager().Track(*this);
    }

    void FCoroutineState::BindOwner(CObject* InOwner)
    {
        if (InOwner != nullptr && !bHasOwner)
        {
            Owner     = InOwner;
            bHasOwner = true;
        }
    }

    void FCoroutineState::AwaitChildren(TVector<FCoroutineState*>&& InChildren)
    {
        {
            TScopeLock Guard(Lock);
            AwaitedChildren = Move(InChildren);
        }

        // A cancel that landed before the children were known still has to reach them.
        if (IsCancelRequested())
        {
            Cancel();
        }
    }

    void FCoroutineState::ClearAwaitedChildren()
    {
        TScopeLock Guard(Lock);
        AwaitedChildren.Reset();
    }

    void FCoroutineState::FinishSucceeded()
    {
        Complete(EStatus::Succeeded);
    }

    void FCoroutineState::Complete(EStatus Final)
    {
        TVector<TMoveOnlyFunction<void()>> Run;
        {
            TScopeLock Guard(Lock);
            Status.store(Final, std::memory_order_release);
            Run = Move(Continuations);
            Continuations.Reset();
            AwaitedChildren.Reset();
        }

        if (World != nullptr)
        {
            World->GetTimers()->GetCoroutineManager().Untrack(*this);
            World = nullptr;
        }

        for (TMoveOnlyFunction<void()>& Callback : Run)
        {
            Callback();
        }
    }

    void SuspendLatent(FCoroutineState& State, FLatentWaitSpec&& Spec, uint32 Serial)
    {
        ASSERT(State.GetWorld() != nullptr, "A latent wait needs a world. Pass a CWorld*, or an object with GetWorld(), as the coroutine's first parameter.");
        State.GetWorld()->GetTimers()->GetCoroutineManager().Suspend(State, Move(Spec), Serial);
    }

    namespace
    {
        Jobs::TTask<void> ResumeOnWorker(FResumeTarget Target)
        {
            Target();
            co_return;
        }
    }

    void ScheduleOnTask(FResumeTarget Target, Jobs::EJobPriority Priority)
    {
        Jobs::Spawn(Priority, &ResumeOnWorker, Move(Target));
    }

    void ScheduleOnGameThread(FResumeTarget Target)
    {
        MainThread::Enqueue([Target = Move(Target)]() { Target(); });
    }

    bool IsOnGameThread()
    {
        return Threading::IsMainThread();
    }
}
