#include "RuntimePCH.h"
#include "ScriptDelegate.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Atomic.h"

namespace Lumina
{
    void (*GOnScriptDelegateDestroyed)(void* DelegateAddress) = nullptr;

    FScriptDelegateBase::~FScriptDelegateBase()
    {
        // A handler is free to destroy the owner mid-broadcast, so every open frame stops touching this.
        for (FBroadcastFrame* Frame = ActiveBroadcast; Frame != nullptr; Frame = Frame->Previous)
        {
            Frame->bAlive = false;
        }
        ActiveBroadcast = nullptr;

        // Live bindings at destruction ask the managed registry to free the matching GCHandles.
        if (!ManagedBindings.empty() && GOnScriptDelegateDestroyed != nullptr)
        {
            GOnScriptDelegateDestroyed(this);
        }
    }

    void FScriptDelegateBase::PushBroadcastFrame(FBroadcastFrame& Frame)
    {
        Frame.Previous  = ActiveBroadcast;
        Frame.bAlive    = true;
        ActiveBroadcast = &Frame;
    }

    void FScriptDelegateBase::PopBroadcastFrame(FBroadcastFrame& Frame)
    {
        ActiveBroadcast = Frame.Previous;
    }

    uint64 FScriptDelegateBase::BindManaged(FManagedThunk Thunk, void* Context)
    {
        if (Thunk == nullptr)
        {
            return 0;
        }

        const uint64 Id = GenerateId();
        ManagedBindings.push_back(FManagedDelegateBinding{ Id, Thunk, Context });
        return Id;
    }

    bool FScriptDelegateBase::UnbindManaged(uint64 Id)
    {
        if (Id == 0)
        {
            return false;
        }

        for (SIZE_T Index = 0; Index < ManagedBindings.size(); ++Index)
        {
            if (ManagedBindings[Index].Id == Id)
            {
                if (LockCount > 0)
                {
                    ManagedBindings[Index].Id = 0;
                    ManagedBindings[Index].Thunk = nullptr;
                    ManagedBindings[Index].Context = nullptr;
                    bCompactionPending = true;
                }
                else
                {
                    ManagedBindings.erase(ManagedBindings.begin() + Index);
                }
                return true;
            }
        }
        return false;
    }

    void FScriptDelegateBase::ClearManaged()
    {
        if (LockCount > 0)
        {
            for (FManagedDelegateBinding& Binding : ManagedBindings)
            {
                Binding.Id = 0;
                Binding.Thunk = nullptr;
                Binding.Context = nullptr;
            }
            bCompactionPending = true;
        }
        else
        {
            ManagedBindings.clear();
        }
    }

    void FScriptDelegateBase::BroadcastManaged(const void* Payload)
    {
        if (ManagedBindings.empty())
        {
            return;
        }

        // A handler is free to destroy whatever owns this delegate, so nothing below touches a dead this.
        FBroadcastFrame Frame;
        PushBroadcastFrame(Frame);

        ++LockCount;

        const SIZE_T Count = ManagedBindings.size();
        for (SIZE_T Index = 0; Index < Count && Frame.bAlive; ++Index)
        {
            // Copy out before invoking; a handler may reallocate the vector mid-broadcast.
            const FManagedThunk Thunk = ManagedBindings[Index].Thunk;
            void* const Context = ManagedBindings[Index].Context;
            if (Thunk != nullptr)
            {
                Thunk(Context, Payload);
            }
        }

        if (!Frame.bAlive)
        {
            return;
        }

        PopBroadcastFrame(Frame);

        ASSERT(LockCount > 0);
        if (--LockCount == 0 && bCompactionPending)
        {
            bCompactionPending = false;

            SIZE_T Write = 0;
            for (SIZE_T Read = 0; Read < ManagedBindings.size(); ++Read)
            {
                if (ManagedBindings[Read].Thunk != nullptr)
                {
                    if (Write != Read)
                    {
                        ManagedBindings[Write] = ManagedBindings[Read];
                    }
                    ++Write;
                }
            }
            ManagedBindings.resize(Write);
        }
    }

    uint64 FScriptDelegateBase::GenerateId()
    {
        static TAtomic<uint64> NextId{1};
        return NextId.fetch_add(1, Atomic::MemoryOrderRelaxed);
    }
}
