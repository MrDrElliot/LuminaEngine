#pragma once

#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // A managed (C#) listener bound into a script delegate.
    struct FManagedDelegateBinding
    {
        uint64  Id = 0;
        void    (*Thunk)(void* Context, const void* Payload) = nullptr;
        void*   Context = nullptr;
    };

    // Installed by the .NET host; called to free GCHandles when a delegate with live managed bindings is destroyed.
    RUNTIME_API extern void (*GOnScriptDelegateDestroyed)(void* DelegateAddress);

    // Non-templated base of every reflectable script delegate; sits at offset 0 of TScriptDelegate<T>.
    class FScriptDelegateBase
    {
    public:

        using FManagedThunk = void(*)(void* Context, const void* Payload);

        FScriptDelegateBase() = default;
        RUNTIME_API ~FScriptDelegateBase();

        // Copy and move are inert; a copied/moved component starts unbound rather than aliasing another's listeners.
        FScriptDelegateBase(const FScriptDelegateBase&) {}
        FScriptDelegateBase(FScriptDelegateBase&&) noexcept {}
        FScriptDelegateBase& operator=(const FScriptDelegateBase&) { return *this; }
        FScriptDelegateBase& operator=(FScriptDelegateBase&&) noexcept { return *this; }

        // Registers a managed listener; returns an id used to unbind. Game thread only.
        RUNTIME_API uint64 BindManaged(FManagedThunk Thunk, void* Context);

        // Removes a managed listener by id. Reentrancy-safe.
        RUNTIME_API bool UnbindManaged(uint64 Id);

        // Drops every managed listener; the managed side owns the GCHandles.
        RUNTIME_API void ClearManaged();

        bool   HasManagedBindings() const { return !ManagedBindings.empty(); }
        size_t GetManagedBindingCount() const { return ManagedBindings.size(); }

    protected:

        // Every broadcast open on this delegate, so destroying the owner can flag all of them at once.
        struct FBroadcastFrame
        {
            FBroadcastFrame* Previous = nullptr;
            bool             bAlive   = true;
        };

        RUNTIME_API void PushBroadcastFrame(FBroadcastFrame& Frame);
        RUNTIME_API void PopBroadcastFrame(FBroadcastFrame& Frame);

        // Fans out to managed listeners; Payload points at the blittable arg, or nullptr for no payload.
        RUNTIME_API void BroadcastManaged(const void* Payload);

    private:

        RUNTIME_API static uint64 GenerateId();

        TVector<FManagedDelegateBinding> ManagedBindings;
        FBroadcastFrame* ActiveBroadcast = nullptr;
        int32   LockCount = 0;
        bool    bCompactionPending = false;
    };

    // A reflectable multicast event native C++ and C# scripts can bind to, carrying one blittable payload struct.
    template<typename TPayload = void>
    class TScriptDelegate : public FScriptDelegateBase
    {
    public:

        using FNativeDelegate = TMulticastDelegate<void, const TPayload&>;

        TScriptDelegate() = default;

        // Inert like the base, or assignment would replace the native listeners while keeping the managed ones.
        TScriptDelegate(const TScriptDelegate&) {}
        TScriptDelegate(TScriptDelegate&&) noexcept {}
        TScriptDelegate& operator=(const TScriptDelegate&) { return *this; }
        TScriptDelegate& operator=(TScriptDelegate&&) noexcept { return *this; }

        template<typename TFunc>
        FDelegateHandle AddStatic(TFunc&& Func) { return Native.AddStatic(std::forward<TFunc>(Func)); }

        template<typename TObject, typename TMemFunc>
        FDelegateHandle AddMember(TObject* Object, TMemFunc Method) { return Native.AddMember(Object, Method); }

        template<typename TLambda>
        FDelegateHandle AddLambda(TLambda&& Lambda) { return Native.AddLambda(std::forward<TLambda>(Lambda)); }

        bool Remove(FDelegateHandle Handle) { return Native.Remove(Handle); }

        // True if any native or managed listener is attached.
        bool IsBound() const { return Native.IsBound() || HasManagedBindings(); }

        void Broadcast(const TPayload& Payload)
        {
            // A native handler may destroy the owner, which would leave the managed fan-out reading freed memory.
            FBroadcastFrame Frame;
            PushBroadcastFrame(Frame);

            Native.Broadcast(Payload);

            if (!Frame.bAlive)
            {
                return;
            }

            BroadcastManaged(&Payload);

            if (Frame.bAlive)
            {
                PopBroadcastFrame(Frame);
            }
        }

    private:

        FNativeDelegate Native;
    };

    // No-payload specialization.
    template<>
    class TScriptDelegate<void> : public FScriptDelegateBase
    {
    public:

        using FNativeDelegate = TMulticastDelegate<void>;

        TScriptDelegate() = default;

        // Inert like the base, or assignment would replace the native listeners while keeping the managed ones.
        TScriptDelegate(const TScriptDelegate&) {}
        TScriptDelegate(TScriptDelegate&&) noexcept {}
        TScriptDelegate& operator=(const TScriptDelegate&) { return *this; }
        TScriptDelegate& operator=(TScriptDelegate&&) noexcept { return *this; }

        template<typename TFunc>
        FDelegateHandle AddStatic(TFunc&& Func) { return Native.AddStatic(std::forward<TFunc>(Func)); }

        template<typename TObject, typename TMemFunc>
        FDelegateHandle AddMember(TObject* Object, TMemFunc Method) { return Native.AddMember(Object, Method); }

        template<typename TLambda>
        FDelegateHandle AddLambda(TLambda&& Lambda) { return Native.AddLambda(std::forward<TLambda>(Lambda)); }

        bool Remove(FDelegateHandle Handle) { return Native.Remove(Handle); }

        bool IsBound() const { return Native.IsBound() || HasManagedBindings(); }

        void Broadcast()
        {
            // A native handler may destroy the owner, which would leave the managed fan-out reading freed memory.
            FBroadcastFrame Frame;
            PushBroadcastFrame(Frame);

            Native.Broadcast();

            if (!Frame.bAlive)
            {
                return;
            }

            BroadcastManaged(nullptr);

            if (Frame.bAlive)
            {
                PopBroadcastFrame(Frame);
            }
        }

    private:

        FNativeDelegate Native;
    };

    using FScriptDelegate = TScriptDelegate<void>;

    static_assert(sizeof(TScriptDelegate<int>) == sizeof(FScriptDelegate),
        "TScriptDelegate size must be payload-independent (no payload is stored).");
}
