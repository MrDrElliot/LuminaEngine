#pragma once

#include <cstddef>

#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/LuminaMacros.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Installed by the .NET host; releases the GCHandle behind one managed listener. A managed bind hands
    // its handle to the delegate, so retiring the listener is what frees it, here or in the destructor.
    RUNTIME_API extern void (*GFreeManagedDelegateContext)(void* Context);

    /** Managed bindings alive process-wide. */
    RUNTIME_API size_t GetLiveManagedBindingCount();

    /** Drops every managed listener on every delegate, releasing the GCHandles behind them. A handler lives
     *  in the script load context, so it cannot outlive a reload; this is what lets that context unload. */
    RUNTIME_API void ClearAllManagedDelegateBindings();

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

        // Native and managed together, which the details panel subtracts to show the two separately.
        RUNTIME_API size_t GetBindingCount() const;

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

    /** One broadcast's arguments, laid out as a plain C struct in declaration order. */
    template<typename... TArgs>
    struct TArgPack;

    template<>
    struct TArgPack<> {};

    template<typename A0>
    struct TArgPack<A0>
    {
        A0 V0;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
    };

    template<typename A0, typename A1>
    struct TArgPack<A0, A1>
    {
        A0 V0; A1 V1;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
        static constexpr size_t Off1() { return offsetof(TArgPack, V1); }
    };

    template<typename A0, typename A1, typename A2>
    struct TArgPack<A0, A1, A2>
    {
        A0 V0; A1 V1; A2 V2;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
        static constexpr size_t Off1() { return offsetof(TArgPack, V1); }
        static constexpr size_t Off2() { return offsetof(TArgPack, V2); }
    };

    template<typename A0, typename A1, typename A2, typename A3>
    struct TArgPack<A0, A1, A2, A3>
    {
        A0 V0; A1 V1; A2 V2; A3 V3;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
        static constexpr size_t Off1() { return offsetof(TArgPack, V1); }
        static constexpr size_t Off2() { return offsetof(TArgPack, V2); }
        static constexpr size_t Off3() { return offsetof(TArgPack, V3); }
    };

    template<typename A0, typename A1, typename A2, typename A3, typename A4>
    struct TArgPack<A0, A1, A2, A3, A4>
    {
        A0 V0; A1 V1; A2 V2; A3 V3; A4 V4;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
        static constexpr size_t Off1() { return offsetof(TArgPack, V1); }
        static constexpr size_t Off2() { return offsetof(TArgPack, V2); }
        static constexpr size_t Off3() { return offsetof(TArgPack, V3); }
        static constexpr size_t Off4() { return offsetof(TArgPack, V4); }
    };

    template<typename A0, typename A1, typename A2, typename A3, typename A4, typename A5>
    struct TArgPack<A0, A1, A2, A3, A4, A5>
    {
        A0 V0; A1 V1; A2 V2; A3 V3; A4 V4; A5 V5;
        static constexpr size_t Off0() { return offsetof(TArgPack, V0); }
        static constexpr size_t Off1() { return offsetof(TArgPack, V1); }
        static constexpr size_t Off2() { return offsetof(TArgPack, V2); }
        static constexpr size_t Off3() { return offsetof(TArgPack, V3); }
        static constexpr size_t Off4() { return offsetof(TArgPack, V4); }
        static constexpr size_t Off5() { return offsetof(TArgPack, V5); }
    };

    namespace Delegates
    {
        template<typename TCallable, typename TPack, typename... TArgs>
        struct TPackInvoker;

        template<typename TCallable, typename TPack>
        struct TPackInvoker<TCallable, TPack>
        {
            static void Invoke(TCallable& Callable, const TPack&) { Callable(); }
        };

        template<typename TCallable, typename TPack, typename A0>
        struct TPackInvoker<TCallable, TPack, A0>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0); }
        };

        template<typename TCallable, typename TPack, typename A0, typename A1>
        struct TPackInvoker<TCallable, TPack, A0, A1>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0, Pack.V1); }
        };

        template<typename TCallable, typename TPack, typename A0, typename A1, typename A2>
        struct TPackInvoker<TCallable, TPack, A0, A1, A2>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0, Pack.V1, Pack.V2); }
        };

        template<typename TCallable, typename TPack, typename A0, typename A1, typename A2, typename A3>
        struct TPackInvoker<TCallable, TPack, A0, A1, A2, A3>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0, Pack.V1, Pack.V2, Pack.V3); }
        };

        template<typename TCallable, typename TPack, typename A0, typename A1, typename A2, typename A3, typename A4>
        struct TPackInvoker<TCallable, TPack, A0, A1, A2, A3, A4>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0, Pack.V1, Pack.V2, Pack.V3, Pack.V4); }
        };

        template<typename TCallable, typename TPack, typename A0, typename A1, typename A2, typename A3, typename A4, typename A5>
        struct TPackInvoker<TCallable, TPack, A0, A1, A2, A3, A4, A5>
        {
            static void Invoke(TCallable& Callable, const TPack& Pack) { Callable(Pack.V0, Pack.V1, Pack.V2, Pack.V3, Pack.V4, Pack.V5); }
        };
    }

    // A reflectable multicast event native C++ and C# scripts can bind to, carrying reflected arguments.
    template<typename... TArgs>
    class TScriptDelegate : public FScriptDelegateBase
    {
    public:

        using FPack = TArgPack<TArgs...>;

        static_assert(sizeof...(TArgs) <= 6, "A script delegate carries at most six arguments.");

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
            return AddCallable([Object, Method](const TArgs&... Args) { (Object->*Method)(Args...); });
        }

        bool Remove(FDelegateHandle Handle) { return RemoveListener(Handle.ID); }

        void Broadcast(const TArgs&... Args)
        {
            if (!IsBound())
            {
                return;
            }

            const FPack Pack{ Args... };
            BroadcastAll(&Pack);
        }

        /** Broadcasts then drops every listener, for a one-shot event such as engine startup. */
        void BroadcastAndClear(const TArgs&... Args)
        {
            Broadcast(Args...);
            RemoveAll();
        }

    private:

        template<typename TCallable>
        FDelegateHandle AddCallable(TCallable&& Callable)
        {
            using FOwned = std::decay_t<TCallable>;

            FOwned* Owned = new FOwned(std::forward<TCallable>(Callable));
            const uint64 Id = AddListener(
                [](void* Context, const void* Payload)
                {
                    Delegates::TPackInvoker<FOwned, FPack, TArgs...>::Invoke(
                        *static_cast<FOwned*>(Context), *static_cast<const FPack*>(Payload));
                },
                Owned,
                [](void* Context) { delete static_cast<FOwned*>(Context); });

            return FDelegateHandle{ Id };
        }
    };

    using FScriptDelegate = TScriptDelegate<>;

    static_assert(sizeof(TScriptDelegate<int>) == sizeof(FScriptDelegate),
        "TScriptDelegate size must be payload-independent (no payload is stored).");
}
