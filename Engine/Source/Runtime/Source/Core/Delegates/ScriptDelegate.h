#pragma once

#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Installed by the .NET host; called to free GCHandles when a delegate with live managed bindings is destroyed.
    RUNTIME_API extern void (*GOnScriptDelegateDestroyed)(void* DelegateAddress);

    // Non-templated base of every reflectable script delegate; sits at offset 0 of TScriptDelegate<T>.
    class FScriptDelegateBase
    {
    public:

        using FThunk = void(*)(void* Context, const void* Payload);

        // The name the .NET bridge binds against.
        using FManagedThunk = FThunk;

        FScriptDelegateBase() = default;
        RUNTIME_API ~FScriptDelegateBase();

        // Copy and move are inert; a copied/moved component starts unbound rather than aliasing another's listeners.
        FScriptDelegateBase(const FScriptDelegateBase&) {}
        FScriptDelegateBase(FScriptDelegateBase&&) noexcept {}
        FScriptDelegateBase& operator=(const FScriptDelegateBase&) { return *this; }
        FScriptDelegateBase& operator=(FScriptDelegateBase&&) noexcept { return *this; }

        // Registers a managed listener; returns an id used to unbind. Game thread only.
        RUNTIME_API uint64 BindManaged(FThunk Thunk, void* Context);

        // Removes a managed listener by id. Reentrancy-safe.
        RUNTIME_API bool UnbindManaged(uint64 Id);

        // Drops every managed listener; the managed side owns the GCHandles.
        RUNTIME_API void ClearManaged();

        // Drops every listener, native and managed alike.
        RUNTIME_API void RemoveAll();

        RUNTIME_API bool   HasManagedBindings() const;
        RUNTIME_API size_t GetManagedBindingCount() const;

        NODISCARD bool IsBound() const { return LiveCount != 0; }

    protected:

        // One listener, native or managed. A native bind owns Context and supplies Destroy; a managed one does not.
        struct FListener
        {
            uint64 Id       = 0;
            FThunk Thunk    = nullptr;
            void*  Context  = nullptr;
            void (*Destroy)(void*) = nullptr;
        };

        // Every broadcast open on this delegate, so destroying the owner can flag all of them at once.
        struct FBroadcastFrame
        {
            FBroadcastFrame* Previous = nullptr;
            bool             bAlive   = true;
        };

        RUNTIME_API uint64 AddListener(FThunk Thunk, void* Context, void (*Destroy)(void*));
        RUNTIME_API bool   RemoveListener(uint64 Id);

        // Fans out to every listener; Payload points at the blittable arg, or nullptr for no payload.
        RUNTIME_API void BroadcastAll(const void* Payload);

        RUNTIME_API static uint64 GenerateId();

    private:

        void RetireAt(size_t Index);
        void Compact();

        TVector<FListener> Listeners;
        FBroadcastFrame*   ActiveBroadcast = nullptr;
        uint16             LiveCount = 0;
        uint16             LockCount = 0;
        bool               bCompactionPending = false;
    };

    // A reflectable multicast event native C++ and C# scripts can bind to, carrying one blittable payload struct.
    template<typename TPayload = void>
    class TScriptDelegate : public FScriptDelegateBase
    {
    public:

        TScriptDelegate() = default;

        // Inert like the base, or assignment would replace the native listeners while keeping the managed ones.
        TScriptDelegate(const TScriptDelegate&) {}
        TScriptDelegate(TScriptDelegate&&) noexcept {}
        TScriptDelegate& operator=(const TScriptDelegate&) { return *this; }
        TScriptDelegate& operator=(TScriptDelegate&&) noexcept { return *this; }

        template<typename TFunc>
        FDelegateHandle AddStatic(TFunc&& Func) { return AddCallable(std::forward<TFunc>(Func)); }

        template<typename TLambda>
        FDelegateHandle AddLambda(TLambda&& Lambda) { return AddCallable(std::forward<TLambda>(Lambda)); }

        template<typename TObject, typename TMemFunc>
        FDelegateHandle AddMember(TObject* Object, TMemFunc Method)
        {
            return AddCallable([Object, Method](const TPayload& Payload) { (Object->*Method)(Payload); });
        }

        bool Remove(FDelegateHandle Handle) { return RemoveListener(Handle.ID); }

        void Broadcast(const TPayload& Payload) { BroadcastAll(&Payload); }

    private:

        template<typename TCallable>
        FDelegateHandle AddCallable(TCallable&& Callable)
        {
            using FOwned = std::decay_t<TCallable>;

            FOwned* Owned = new FOwned(std::forward<TCallable>(Callable));
            const uint64 Id = AddListener(
                [](void* Context, const void* Payload)
                {
                    (*static_cast<FOwned*>(Context))(*static_cast<const TPayload*>(Payload));
                },
                Owned,
                [](void* Context) { delete static_cast<FOwned*>(Context); });

            return FDelegateHandle{ Id };
        }
    };

    // No-payload specialization.
    template<>
    class TScriptDelegate<void> : public FScriptDelegateBase
    {
    public:

        TScriptDelegate() = default;

        TScriptDelegate(const TScriptDelegate&) {}
        TScriptDelegate(TScriptDelegate&&) noexcept {}
        TScriptDelegate& operator=(const TScriptDelegate&) { return *this; }
        TScriptDelegate& operator=(TScriptDelegate&&) noexcept { return *this; }

        template<typename TFunc>
        FDelegateHandle AddStatic(TFunc&& Func) { return AddCallable(std::forward<TFunc>(Func)); }

        template<typename TLambda>
        FDelegateHandle AddLambda(TLambda&& Lambda) { return AddCallable(std::forward<TLambda>(Lambda)); }

        template<typename TObject, typename TMemFunc>
        FDelegateHandle AddMember(TObject* Object, TMemFunc Method)
        {
            return AddCallable([Object, Method]() { (Object->*Method)(); });
        }

        bool Remove(FDelegateHandle Handle) { return RemoveListener(Handle.ID); }

        void Broadcast() { BroadcastAll(nullptr); }

    private:

        template<typename TCallable>
        FDelegateHandle AddCallable(TCallable&& Callable)
        {
            using FOwned = std::decay_t<TCallable>;

            FOwned* Owned = new FOwned(std::forward<TCallable>(Callable));
            const uint64 Id = AddListener(
                [](void* Context, const void*) { (*static_cast<FOwned*>(Context))(); },
                Owned,
                [](void* Context) { delete static_cast<FOwned*>(Context); });

            return FDelegateHandle{ Id };
        }
    };

    using FScriptDelegate = TScriptDelegate<void>;

    static_assert(sizeof(TScriptDelegate<int>) == sizeof(FScriptDelegate),
        "TScriptDelegate size must be payload-independent (no payload is stored).");
}
