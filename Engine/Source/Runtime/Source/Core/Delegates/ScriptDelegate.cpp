#include "RuntimePCH.h"
#include "ScriptDelegate.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Atomic.h"
#include "Containers/HashTable.h"

namespace Lumina
{
    void (*GFreeManagedDelegateContext)(void* Context) = nullptr;

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
            if (GFreeManagedDelegateContext != nullptr)
            {
                GFreeManagedDelegateContext(Context);
            }
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
        // A handler is free to destroy the owner mid-broadcast, so every open frame stops touching this.
        for (FBroadcastFrame* Frame = ActiveBroadcast; Frame != nullptr; Frame = Frame->Previous)
        {
            Frame->bAlive = false;
        }
        ActiveBroadcast = nullptr;

        ManagedBoundDelegates().erase(this);

        // A managed listener's Destroy is what releases its GCHandle, so this frees both kinds.
        for (FListener& Listener : Listeners)
        {
            if (Listener.Destroy != nullptr)
            {
                Listener.Destroy(Listener.Context);
            }
        }
        Listeners.clear();
        LiveCount = 0;
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

    // Frees a native listener's callable and blanks the slot; the slot itself goes on the next compaction.
    void FScriptDelegateBase::RetireAt(size_t Index)
    {
        FListener& Listener = Listeners[Index];
        if (Listener.Thunk == nullptr)
        {
            return;
        }

        if (Listener.Destroy != nullptr)
        {
            Listener.Destroy(Listener.Context);
        }

        Listener.Id      = 0;
        Listener.Thunk   = nullptr;
        Listener.Context = nullptr;
        Listener.Destroy = nullptr;
        --LiveCount;

        if (LockCount > 0)
        {
            bCompactionPending = true;
        }
        else
        {
            Listeners.erase(Listeners.begin() + Index);
        }
    }

    void FScriptDelegateBase::Compact()
    {
        bCompactionPending = false;

        size_t Write = 0;
        for (size_t Read = 0; Read < Listeners.size(); ++Read)
        {
            if (Listeners[Read].Thunk != nullptr)
            {
                if (Write != Read)
                {
                    Listeners[Write] = Listeners[Read];
                }
                ++Write;
            }
        }
        Listeners.resize(Write);
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
