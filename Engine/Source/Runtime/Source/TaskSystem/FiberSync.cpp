#include "RuntimePCH.h"
#include "FiberSync.h"

#include "Scheduler/JobScheduler.h"
#include "Core/Threading/Thread.h"

#include <intrin.h>

// All four share one shape: a spinlock guards a FIFO of waiter nodes, and wakeups are direct
// hand-offs -- the releaser updates the shared state for the woken waiter so it never re-contends.
namespace Lumina
{
    namespace FiberSyncDetail
    {
        struct FWaiterNode
        {
            Jobs::FFiberHandle Fiber;            // valid when bIsFiber
            TAtomic<bool>      Signaled{false};  // external (non-fiber) waiters poll this
            bool               bIsFiber = false;
            uint8              Mode     = 0;      // primitive-specific (shared/exclusive for the RW lock)
            FWaiterNode*       Next     = nullptr;
        };

        enum : uint8 { kModeExclusive = 0, kModeShared = 1 };

        FORCEINLINE void Pause() { _mm_pause(); }

        FORCEINLINE void SpinLock(TAtomic<uint32>& Spin)
        {
            uint32 Expected = 0;
            while (!Spin.compare_exchange_weak(Expected, 1u, std::memory_order_acquire, std::memory_order_relaxed))
            {
                Expected = 0;
                Pause();
            }
        }

        FORCEINLINE void SpinUnlock(TAtomic<uint32>& Spin)
        {
            Spin.store(0u, std::memory_order_release);
        }

        // Make a popped (already-unlinked) waiter runnable. Last touch of the node, the woken context
        // may free it (it lives on the waiter's stack) the instant it runs, so never read it after this.
        FORCEINLINE void Wake(FWaiterNode* Node)
        {
            if (Node->bIsFiber)
            {
                Jobs::ResumeFiber(Node->Fiber);
            }
            else
            {
                Node->Signaled.store(true, std::memory_order_release);
            }
        }

        // Deliberately does NOT run queued jobs (unlike the counter assist-wait): a job contending the same
        // lock would enqueue a second waiter on this call stack, and FIFO hand-off would then deadlock.
        FORCEINLINE void WaitExternal(FWaiterNode* Node)
        {
            uint32 IdleSpins = 0;
            while (!Node->Signaled.load(std::memory_order_acquire))
            {
                if (++IdleSpins < 256)
                {
                    Pause();
                }
                else
                {
                    Threading::ThreadYield();
                    IdleSpins = 0;
                }
            }
        }
    }

    using namespace FiberSyncDetail;

    namespace
    {
        struct FMutexPark
        {
            FFiberMutex* Self;
            FWaiterNode* Node;
        };
    }

    bool FFiberMutex::TryLock()
    {
        SpinLock(Spin);
        if (!bLocked)
        {
            bLocked = true;
            SpinUnlock(Spin);
            return true;
        }
        SpinUnlock(Spin);
        return false;
    }

    void FFiberMutex::Lock()
    {
        // Uncontended fast path.
        if (TryLock())
        {
            return;
        }

        FWaiterNode Node;

        if (Jobs::IsWorkerThread())
        {
            FMutexPark Park{ this, &Node };
            Jobs::ParkFiber([](void* Ctx, Jobs::FFiberHandle Handle) -> bool
            {
                FMutexPark*  P = static_cast<FMutexPark*>(Ctx);
                FFiberMutex* M = P->Self;
                SpinLock(M->Spin);
                if (!M->bLocked)
                {
                    // Freed between the fast path and now, take it, don't park.
                    M->bLocked = true;
                    SpinUnlock(M->Spin);
                    return false;
                }
                P->Node->Fiber    = Handle;
                P->Node->bIsFiber = true;
                P->Node->Next     = nullptr;
                if (M->Tail)
                {
                    M->Tail->Next = P->Node;
                }
                else
                {
                    M->Head = P->Node;
                }
                M->Tail = P->Node;
                SpinUnlock(M->Spin);
                return true;
            }, &Park);
            // Resumed: Unlock() handed ownership directly to us. We hold the lock.
            return;
        }

        // External thread: enqueue and assist-wait for the hand-off.
        SpinLock(Spin);
        if (!bLocked)
        {
            bLocked = true;
            SpinUnlock(Spin);
            return;
        }
        Node.bIsFiber = false;
        Node.Next     = nullptr;
        if (Tail) Tail->Next = &Node; else Head = &Node;
        Tail = &Node;
        SpinUnlock(Spin);

        WaitExternal(&Node);
    }

    void FFiberMutex::Unlock()
    {
        SpinLock(Spin);
        if (Head != nullptr)
        {
            // Direct hand-off: keep bLocked set and pass ownership to the next waiter.
            FWaiterNode* Next = Head;
            Head = Next->Next;
            if (Head == nullptr) Tail = nullptr;
            SpinUnlock(Spin);
            Wake(Next);
        }
        else
        {
            bLocked = false;
            SpinUnlock(Spin);
        }
    }

    namespace
    {
        struct FSemPark
        {
            FFiberSemaphore* Self;
            FWaiterNode*     Node;
        };
    }

    bool FFiberSemaphore::TryAcquire()
    {
        SpinLock(Spin);
        if (Count > 0)
        {
            --Count;
            SpinUnlock(Spin);
            return true;
        }
        SpinUnlock(Spin);
        return false;
    }

    void FFiberSemaphore::Acquire()
    {
        FWaiterNode Node;

        if (Jobs::IsWorkerThread())
        {
            FSemPark Park{ this, &Node };
            Jobs::ParkFiber([](void* Ctx, Jobs::FFiberHandle Handle) -> bool
            {
                FSemPark*        P = static_cast<FSemPark*>(Ctx);
                FFiberSemaphore* S = P->Self;
                SpinLock(S->Spin);
                if (S->Count > 0)
                {
                    --S->Count;
                    SpinUnlock(S->Spin);
                    return false;
                }
                P->Node->Fiber    = Handle;
                P->Node->bIsFiber = true;
                P->Node->Next     = nullptr;
                if (S->Tail) S->Tail->Next = P->Node; else S->Head = P->Node;
                S->Tail = P->Node;
                SpinUnlock(S->Spin);
                return true;
            }, &Park);
            return; // Release() handed us a permit.
        }

        SpinLock(Spin);
        if (Count > 0)
        {
            --Count;
            SpinUnlock(Spin);
            return;
        }
        Node.bIsFiber = false;
        Node.Next     = nullptr;
        if (Tail) Tail->Next = &Node; else Head = &Node;
        Tail = &Node;
        SpinUnlock(Spin);

        WaitExternal(&Node);
    }

    void FFiberSemaphore::Release(int32 N)
    {
        while (N > 0)
        {
            SpinLock(Spin);
            if (Head != nullptr)
            {
                // Hand a permit straight to the next waiter (don't bump Count).
                FWaiterNode* Next = Head;
                Head = Next->Next;
                if (Head == nullptr) Tail = nullptr;
                SpinUnlock(Spin);
                Wake(Next);
            }
            else
            {
                ++Count;
                SpinUnlock(Spin);
            }
            --N;
        }
    }

    namespace
    {
        struct FRWPark
        {
            FFiberSharedMutex* Self;
            FWaiterNode*       Node;
            uint8              Mode;
        };
    }

    // Grant as many front waiters as the current state allows, following FIFO order. Called holding Spin.
    void FFiberSharedMutex::WakeFrontLocked()
    {
        for (;;)
        {
            FWaiterNode* Front = Head;
            if (Front == nullptr || bWriter)
            {
                return;
            }

            if (Front->Mode == kModeExclusive)
            {
                if (Readers != 0)
                {
                    return; // writer must wait for readers to drain
                }
                bWriter = true;
                Head = Front->Next;
                if (Head == nullptr) Tail = nullptr;
                Wake(Front);
                return; // exclusive grant is mutually exclusive, stop
            }

            // Shared front: grant it (and keep going to batch consecutive readers).
            ++Readers;
            Head = Front->Next;
            if (Head == nullptr) Tail = nullptr;
            Wake(Front);
        }
    }

    bool FFiberSharedMutex::TryLock()
    {
        SpinLock(Spin);
        if (!bWriter && Readers == 0 && Head == nullptr)
        {
            bWriter = true;
            SpinUnlock(Spin);
            return true;
        }
        SpinUnlock(Spin);
        return false;
    }

    bool FFiberSharedMutex::TryLockShared()
    {
        SpinLock(Spin);
        if (!bWriter && Head == nullptr)
        {
            ++Readers;
            SpinUnlock(Spin);
            return true;
        }
        SpinUnlock(Spin);
        return false;
    }

    void FFiberSharedMutex::Lock()
    {
        FWaiterNode Node;

        if (Jobs::IsWorkerThread())
        {
            FRWPark Park{ this, &Node, kModeExclusive };
            Jobs::ParkFiber([](void* Ctx, Jobs::FFiberHandle Handle) -> bool
            {
                FRWPark*           P = static_cast<FRWPark*>(Ctx);
                FFiberSharedMutex* M = P->Self;
                SpinLock(M->Spin);
                if (!M->bWriter && M->Readers == 0 && M->Head == nullptr)
                {
                    M->bWriter = true;
                    SpinUnlock(M->Spin);
                    return false;
                }
                P->Node->Fiber    = Handle;
                P->Node->bIsFiber = true;
                P->Node->Mode     = kModeExclusive;
                P->Node->Next     = nullptr;
                if (M->Tail) M->Tail->Next = P->Node; else M->Head = P->Node;
                M->Tail = P->Node;
                SpinUnlock(M->Spin);
                return true;
            }, &Park);
            return;
        }

        SpinLock(Spin);
        if (!bWriter && Readers == 0 && Head == nullptr)
        {
            bWriter = true;
            SpinUnlock(Spin);
            return;
        }
        Node.bIsFiber = false;
        Node.Mode     = kModeExclusive;
        Node.Next     = nullptr;
        if (Tail) Tail->Next = &Node; else Head = &Node;
        Tail = &Node;
        SpinUnlock(Spin);

        WaitExternal(&Node);
    }

    void FFiberSharedMutex::LockShared()
    {
        FWaiterNode Node;

        if (Jobs::IsWorkerThread())
        {
            FRWPark Park{ this, &Node, kModeShared };
            Jobs::ParkFiber([](void* Ctx, Jobs::FFiberHandle Handle) -> bool
            {
                FRWPark*           P = static_cast<FRWPark*>(Ctx);
                FFiberSharedMutex* M = P->Self;
                SpinLock(M->Spin);
                if (!M->bWriter && M->Head == nullptr)
                {
                    ++M->Readers;
                    SpinUnlock(M->Spin);
                    return false;
                }
                P->Node->Fiber    = Handle;
                P->Node->bIsFiber = true;
                P->Node->Mode     = kModeShared;
                P->Node->Next     = nullptr;
                if (M->Tail) M->Tail->Next = P->Node; else M->Head = P->Node;
                M->Tail = P->Node;
                SpinUnlock(M->Spin);
                return true;
            }, &Park);
            return;
        }

        SpinLock(Spin);
        if (!bWriter && Head == nullptr)
        {
            ++Readers;
            SpinUnlock(Spin);
            return;
        }
        Node.bIsFiber = false;
        Node.Mode     = kModeShared;
        Node.Next     = nullptr;
        if (Tail) Tail->Next = &Node; else Head = &Node;
        Tail = &Node;
        SpinUnlock(Spin);

        WaitExternal(&Node);
    }

    void FFiberSharedMutex::Unlock()
    {
        SpinLock(Spin);
        bWriter = false;
        WakeFrontLocked();
        SpinUnlock(Spin);
    }

    void FFiberSharedMutex::UnlockShared()
    {
        SpinLock(Spin);
        --Readers;
        if (Readers == 0)
        {
            WakeFrontLocked();
        }
        SpinUnlock(Spin);
    }

    namespace
    {
        struct FCVPark
        {
            FFiberConditionVariable* Self;
            FFiberMutex*             Mutex;
            FWaiterNode*             Node;
        };
    }

    void FFiberConditionVariable::Wait(FFiberMutex& Mutex)
    {
        FWaiterNode Node;

        if (Jobs::IsWorkerThread())
        {
            FCVPark Park{ this, &Mutex, &Node };
            Jobs::ParkFiber([](void* Ctx, Jobs::FFiberHandle Handle) -> bool
            {
                FCVPark*                 P  = static_cast<FCVPark*>(Ctx);
                FFiberConditionVariable* CV = P->Self;
                // Enqueue under the CV lock BEFORE releasing the user mutex: a notifier (which holds the
                // mutex while changing the predicate) can't slip a wakeup past us.
                SpinLock(CV->Spin);
                P->Node->Fiber    = Handle;
                P->Node->bIsFiber = true;
                P->Node->Next     = nullptr;
                if (CV->Tail) CV->Tail->Next = P->Node; else CV->Head = P->Node;
                CV->Tail = P->Node;
                SpinUnlock(CV->Spin);
                P->Mutex->Unlock();
                return true; // always parks
            }, &Park);
            // Notified, re-acquire the mutex before returning.
            Mutex.Lock();
            return;
        }

        // External thread.
        SpinLock(Spin);
        Node.bIsFiber = false;
        Node.Next     = nullptr;
        if (Tail) Tail->Next = &Node; else Head = &Node;
        Tail = &Node;
        SpinUnlock(Spin);
        Mutex.Unlock();

        WaitExternal(&Node);
        Mutex.Lock();
    }

    void FFiberConditionVariable::NotifyOne()
    {
        SpinLock(Spin);
        FWaiterNode* Next = Head;
        if (Next != nullptr)
        {
            Head = Next->Next;
            if (Head == nullptr) Tail = nullptr;
        }
        SpinUnlock(Spin);
        if (Next != nullptr)
        {
            Wake(Next);
        }
    }

    void FFiberConditionVariable::NotifyAll()
    {
        SpinLock(Spin);
        FWaiterNode* List = Head;
        Head = nullptr;
        Tail = nullptr;
        SpinUnlock(Spin);

        while (List != nullptr)
        {
            FWaiterNode* Next = List->Next; // read before waking, Wake may free the node
            Wake(List);
            List = Next;
        }
    }
}
