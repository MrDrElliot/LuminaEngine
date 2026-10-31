#include "RuntimePCH.h"
#include "ScriptDelegate.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Atomic.h"
#include "Containers/HashTable.h"
#include "Scripting/ScriptCallback.h"

namespace Lumina
{
    namespace
    {
        size_t GLiveManagedBindings = 0;

        // Only delegates that actually carry a managed listener, so an unbound one costs nothing and no
        // per-delegate bytes are spent on a concern that only matters at reload.
        THashSet<FScriptDelegateBase*>& ManagedBoundDelegates()
        {
            static THashSet<FScriptDelegateBase*> Set;
            return Set;
        }

        // The Destroy hook every managed listener carries. Its address is also what marks a listener as
        // managed, so an id collision can never retire a native callable.
        void FreeManagedListener(void* Context)
        {
            --GLiveManagedBindings;
            Scripting::FreeManagedHandle(Context);
        }

        // The context is the callback's token, and the payload pointer crosses as its value.
        void InvokeManagedListener(void* Context, const void* Payload)
        {
            Scripting::InvokeScriptCallbackRepeating(SScriptCallback{ reinterpret_cast<uint64>(Context) }, reinterpret_cast<uint64>(Payload));
        }
    }

    size_t GetLiveManagedBindingCount()
    {
        return GLiveManagedBindings;
    }

    void ClearAllManagedDelegateBindings()
    {
        // Copied, because clearing the last managed listener unregisters the delegate from the set.
        TVector<FScriptDelegateBase*> Bound;
        Bound.reserve(ManagedBoundDelegates().size());
        for (FScriptDelegateBase* Delegate : ManagedBoundDelegates())
        {
            Bound.push_back(Delegate);
        }

        for (FScriptDelegateBase* Delegate : Bound)
        {
            Delegate->ClearManaged();
        }
        ManagedBoundDelegates().clear();
    }

    FScriptDelegateBase::~FScriptDelegateBase()
    {
        FBroadcastFrame* const Outermost = OutermostBroadcast();
        EndOpenBroadcasts();
        ManagedBoundDelegates().erase(this);

        // A handler destroying its own delegate is still running, so the broadcast that called it frees the callables.
        for (FListener& Listener : Listeners)
        {
            if (Listener.Destroy == nullptr)
            {
                continue;
            }
            if (Outermost != nullptr)
            {
                Outermost->PendingDestroy.push_back(Listener);
            }
            else
            {
                Listener.Destroy(Listener.Context);
            }
        }
        Listeners.clear();
        LiveCount = 0;
    }

    FScriptDelegateBase::FScriptDelegateBase(FScriptDelegateBase&& Other) noexcept
    {
        FBroadcastFrame* const Outermost = Other.OutermostBroadcast();
        Other.EndOpenBroadcasts();

        // Retired slots stay behind, since only an open broadcast needed them kept in place.
        for (const FListener& Listener : Other.Listeners)
        {
            if (Listener.Thunk != nullptr)
            {
                Listeners.push_back(Listener);
            }
            else if (Listener.Destroy != nullptr && Outermost != nullptr)
            {
                Outermost->PendingDestroy.push_back(Listener);
            }
        }
        LiveCount = (uint16)Listeners.size();

        Other.Listeners.clear();
        Other.LiveCount = 0;

        if (ManagedBoundDelegates().erase(&Other) != 0)
        {
            ManagedBoundDelegates().insert(this);
        }
    }

    // A handler is free to destroy or relocate the owner mid-broadcast, so every open frame stops touching this.
    void FScriptDelegateBase::EndOpenBroadcasts()
    {
        for (FBroadcastFrame* Frame = ActiveBroadcast; Frame != nullptr; Frame = Frame->Previous)
        {
            Frame->bAlive = false;
        }
        ActiveBroadcast    = nullptr;
        LockCount          = 0;
        bCompactionPending = false;
    }

    FScriptDelegateBase::FBroadcastFrame* FScriptDelegateBase::OutermostBroadcast() const
    {
        FBroadcastFrame* Frame = ActiveBroadcast;
        while (Frame != nullptr && Frame->Previous != nullptr)
        {
            Frame = Frame->Previous;
        }
        return Frame;
    }

    bool FScriptDelegateBase::UnbindManagedAnywhere(const void* LastKnownAddress, uint64 Id)
    {
        if (Id == 0)
        {
            return false;
        }

        // Only a delegate in the set is known to be alive, so the address is never dereferenced on trust.
        THashSet<FScriptDelegateBase*>& Bound = ManagedBoundDelegates();
        FScriptDelegateBase* Hint = static_cast<FScriptDelegateBase*>(const_cast<void*>(LastKnownAddress));
        if (Bound.contains(Hint) && Hint->UnbindManaged(Id))
        {
            return true;
        }

        for (FScriptDelegateBase* Delegate : Bound)
        {
            // Returns at once, since a successful unbind can erase the delegate from the set being walked.
            if (Delegate != Hint && Delegate->UnbindManaged(Id))
            {
                return true;
            }
        }
        return false;
    }

    uint64 FScriptDelegateBase::AddListener(FThunk Thunk, void* Context, void (*Destroy)(void*))
    {
        if (Thunk == nullptr)
        {
            if (Destroy != nullptr)
            {
                Destroy(Context);
            }
            return 0;
        }

        const uint64 Id = GenerateId();
        Listeners.push_back(FListener{ Id, Thunk, Context, Destroy });
        ++LiveCount;
        return Id;
    }

    bool FScriptDelegateBase::RemoveListener(uint64 Id)
    {
        if (Id == 0)
        {
            return false;
        }

        for (size_t Index = 0; Index < Listeners.size(); ++Index)
        {
            if (Listeners[Index].Id == Id)
            {
                RetireAt(Index);
                return true;
            }
        }
        return false;
    }

    // During a broadcast the callable may be the one running, so it is kept until the compaction after the broadcast.
    void FScriptDelegateBase::RetireAt(size_t Index)
    {
        FListener& Listener = Listeners[Index];
        if (Listener.Thunk == nullptr)
        {
            return;
        }

        Listener.Id    = 0;
        Listener.Thunk = nullptr;
        --LiveCount;

        if (LockCount > 0)
        {
            bCompactionPending = true;
            return;
        }

        const FListener Retired = Listener;
        Listeners.erase(Listeners.begin() + Index);
        if (Retired.Destroy != nullptr)
        {
            Retired.Destroy(Retired.Context);
        }
    }

    void FScriptDelegateBase::Compact()
    {
        bCompactionPending = false;

        TVector<FListener> Retired;
        size_t Write = 0;
        for (size_t Read = 0; Read < Listeners.size(); ++Read)
        {
            if (Listeners[Read].Thunk == nullptr)
            {
                if (Listeners[Read].Destroy != nullptr)
                {
                    Retired.push_back(Listeners[Read]);
                }
                continue;
            }
            if (Write != Read)
            {
                Listeners[Write] = Listeners[Read];
            }
            ++Write;
        }
        Listeners.resize(Write);

        // Last, since freeing a callable can run a destructor that destroys this delegate's owner.
        for (const FListener& Listener : Retired)
        {
            Listener.Destroy(Listener.Context);
        }
    }

    uint64 FScriptDelegateBase::BindManaged(SScriptCallback Callback)
    {
        return BindManaged(&InvokeManagedListener, reinterpret_cast<void*>(Callback.Token));
    }

    uint64 FScriptDelegateBase::BindManaged(FThunk Thunk, void* Context)
    {
        ++GLiveManagedBindings;
        const uint64 Id = AddListener(Thunk, Context, &FreeManagedListener);
        if (Id == 0)
        {
            // AddListener already ran Destroy, which decremented; nothing else to undo.
            return 0;
        }

        ManagedBoundDelegates().insert(this);
        return Id;
    }

    int32 FScriptDelegateBase::GetManagedListeners(uint64* OutIds, uint64* OutTokens, int32 Capacity) const
    {
        int32 Count = 0;
        for (const FListener& Listener : Listeners)
        {
            // Only a token-backed listener can be resolved by the managed side; a test thunk's context is not a GCHandle.
            if (Listener.Thunk != &InvokeManagedListener)
            {
                continue;
            }
            if (Count < Capacity)
            {
                OutIds[Count]    = Listener.Id;
                OutTokens[Count] = reinterpret_cast<uint64>(Listener.Context);
            }
            ++Count;
        }
        return Count;
    }

    bool FScriptDelegateBase::UnbindManaged(uint64 Id)
    {
        if (Id == 0)
        {
            return false;
        }

        for (size_t Index = 0; Index < Listeners.size(); ++Index)
        {
            // Managed only, so an id collision can never free a native callable.
            if (Listeners[Index].Id == Id && Listeners[Index].Destroy == &FreeManagedListener)
            {
                RetireAt(Index);
                if (!HasManagedBindings())
                {
                    ManagedBoundDelegates().erase(this);
                }
                return true;
            }
        }
        return false;
    }

    void FScriptDelegateBase::ClearManaged()
    {
        for (size_t Index = Listeners.size(); Index > 0; --Index)
        {
            if (Listeners[Index - 1].Destroy == &FreeManagedListener)
            {
                RetireAt(Index - 1);
            }
        }
        ManagedBoundDelegates().erase(this);
    }

    void FScriptDelegateBase::RemoveAll()
    {
        for (size_t Index = Listeners.size(); Index > 0; --Index)
        {
            RetireAt(Index - 1);
        }
    }

    bool FScriptDelegateBase::HasManagedBindings() const
    {
        for (const FListener& Listener : Listeners)
        {
            if (Listener.Thunk != nullptr && Listener.Destroy == &FreeManagedListener)
            {
                return true;
            }
        }
        return false;
    }

    size_t FScriptDelegateBase::GetBindingCount() const
    {
        return LiveCount;
    }

    size_t FScriptDelegateBase::GetManagedBindingCount() const
    {
        size_t Count = 0;
        for (const FListener& Listener : Listeners)
        {
            if (Listener.Thunk != nullptr && Listener.Destroy == &FreeManagedListener)
            {
                ++Count;
            }
        }
        return Count;
    }

    void FScriptDelegateBase::BroadcastAll(const void* Payload)
    {
        if (LiveCount == 0)
        {
            return;
        }

        // A handler is free to destroy whatever owns this delegate, so nothing below touches a dead this.
        FBroadcastFrame Frame;
        Frame.Previous  = ActiveBroadcast;
        Frame.bAlive    = true;
        ActiveBroadcast = &Frame;

        ++LockCount;

        // Natives run before managed, so one destroying the owner still stops the managed fan-out.
        for (int32 Phase = 0; Phase < 2 && Frame.bAlive; ++Phase)
        {
            const bool bNativePhase = (Phase == 0);

            const size_t Count = Listeners.size();
            for (size_t Index = 0; Index < Count && Frame.bAlive; ++Index)
            {
                // Copy out before invoking; a handler may reallocate the vector mid-broadcast.
                const FThunk Thunk   = Listeners[Index].Thunk;
                void* const  Context = Listeners[Index].Context;
                const bool   bNative = Listeners[Index].Destroy != &FreeManagedListener;

                if (Thunk != nullptr && bNative == bNativePhase)
                {
                    Thunk(Context, Payload);
                }
            }
        }

        if (!Frame.bAlive)
        {
            for (const FListener& Listener : Frame.PendingDestroy)
            {
                Listener.Destroy(Listener.Context);
            }
            return;
        }

        ActiveBroadcast = Frame.Previous;

        ASSERT(LockCount > 0);
        if (--LockCount == 0 && bCompactionPending)
        {
            Compact();
        }
    }

    uint64 FScriptDelegateBase::GenerateId()
    {
        static TAtomic<uint64> NextId{1};
        return NextId.fetch_add(1, Atomic::MemoryOrderRelaxed);
    }
}
