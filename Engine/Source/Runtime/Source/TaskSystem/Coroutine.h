#pragma once

#include "Containers/Function.h"
#include "Containers/Vector.h"
#include "Core/Assertions/Assert.h"
#include "Core/LuminaMacros.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Templates/Optional.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "TaskSystem/Fork/ForkRuntime.h"

#include <coroutine>
#include <type_traits>
#include "Coroutine.generated.h"

namespace Lumina
{
    class CWorld;
    class FCoroutineManager;

    template<typename T = void>
    class TCoroutine;

    REFLECT()
    enum class ECoroutineClock : uint8
    {
        // World time, which dilates and stops while the world is paused.
        World,
        // World time that keeps running while the world is paused.
        Unpaused,
        // Wall clock time, ignoring both dilation and pause.
        Real,
    };

    namespace CoroDetail
    {
        enum class EStatus : uint8
        {
            Running,
            Succeeded,
            Canceled,
        };

        class RUNTIME_API FCoroutineState
        {
        public:
            FCoroutineState() = default;
            virtual ~FCoroutineState() = default;
            LE_NO_COPYMOVE(FCoroutineState);

            void AddRef() { RefCount.fetch_add(1, std::memory_order_relaxed); }
            void Release();

            // Thread safe, and served at the next suspension point that is not inside a guard.
            void Cancel();

            // Cancels past every guard, for an owner or world that is going away.
            void ForceCancel();

            bool IsDone() const { return Status.load(std::memory_order_acquire) != EStatus::Running; }
            bool WasSuccessful() const { return Status.load(std::memory_order_acquire) == EStatus::Succeeded; }
            bool IsCancelRequested() const { return bCancelRequested.load(std::memory_order_acquire); }
            bool ShouldDieAtSuspension() const;
            bool IsOwnerAlive() const { return !bHasOwner || Owner.IsValid(); }

            // False when it already finished, in which case the callback was not stored and the caller runs it.
            bool TryContinueWith(TMoveOnlyFunction<void()>&& Callback);
            void ContinueWith(TMoveOnlyFunction<void()>&& Callback);

            // Resumes the frame, or destroys it when it has a cancellation to serve.
            void Resume();

            // Destroys the suspended frame and completes as canceled.
            void DestroyAsCanceled();

            // Claims the current suspension, so exactly one of a resolver and the scheduler wakes the frame.
            bool ClaimWait(uint32 Serial);
            uint32 BeginWait() { return WaitSerial.fetch_add(1, std::memory_order_acq_rel) + 1; }
            bool IsWaitingOn(uint32 Serial) const { return WaitSerial.load(std::memory_order_acquire) == Serial; }

            void BindWorld(CWorld* InWorld);
            void BindOwner(CObject* InOwner);
            CWorld* GetWorld() const { return World; }

            // The world tore down, so this state stops reaching for its manager.
            void DetachFromWorld() { World = nullptr; }

            void AwaitChildren(TVector<FCoroutineState*>&& InChildren);
            void ClearAwaitedChildren();

            void SetHandle(std::coroutine_handle<> InHandle) { Handle = InHandle; }

            // Called from the final suspension, after the frame has been destroyed.
            void FinishSucceeded();

            int32 GuardCount = 0;
            bool  bDestroyingForCancel = false;

        private:

            void Complete(EStatus Final);

            TAtomic<int32>   RefCount{1};
            TAtomic<EStatus> Status{EStatus::Running};
            TAtomic<bool>    bCancelRequested{false};
            TAtomic<bool>    bForced{false};
            TAtomic<uint32>  WaitSerial{0};

            std::coroutine_handle<>   Handle{};
            CWorld*                   World = nullptr;
            TWeakObjectPtr<CObject>   Owner;
            bool                      bHasOwner = false;

            FMutex                                Lock;
            TVector<TMoveOnlyFunction<void()>>    Continuations;
            TVector<FCoroutineState*>             AwaitedChildren;
        };

        // A counted reference to a state, so a frame, a handle and a pending callback can each keep it alive.
        class FStateRef
        {
        public:
            FStateRef() = default;
            explicit FStateRef(FCoroutineState* In, bool bAddRef = true) : State(In) { if (State && bAddRef) { State->AddRef(); } }
            FStateRef(const FStateRef& Other) : State(Other.State) { if (State) { State->AddRef(); } }
            FStateRef(FStateRef&& Other) noexcept : State(Other.State) { Other.State = nullptr; }
            FStateRef& operator=(FStateRef Other) noexcept { ElementOps::SwapElements(State, Other.State); return *this; }
            ~FStateRef() { if (State) { State->Release(); } }

            FCoroutineState* Get() const { return State; }
            FCoroutineState* operator->() const { return State; }
            explicit operator bool() const { return State != nullptr; }

        private:
            FCoroutineState* State = nullptr;
        };

        template<typename T>
        class TCoroutineState : public FCoroutineState
        {
        public:
            TOptional<T> Result;
        };

        template<>
        class TCoroutineState<void> : public FCoroutineState
        {
        };

        // Reads a world and an owner off a parameter, returning whether it carried either.
        template<typename TFirst>
        bool BindParameter(FCoroutineState& State, TFirst& First)
        {
            using TBare = std::remove_cvref_t<std::remove_pointer_t<std::remove_cvref_t<TFirst>>>;
            if constexpr (std::is_class_v<TBare> && requires { sizeof(TBare); })
            {
                TBare* Pointer = nullptr;
                if constexpr (std::is_pointer_v<std::remove_cvref_t<TFirst>>)
                {
                    Pointer = const_cast<TBare*>(First);
                }
                else
                {
                    Pointer = const_cast<TBare*>(&First);
                }

                constexpr bool bIsWorld   = std::is_same_v<TBare, CWorld>;
                constexpr bool bHasWorld  = requires { { Pointer->GetWorld() } -> std::convertible_to<CWorld*>; };
                constexpr bool bIsObject  = std::is_base_of_v<CObject, TBare>;
                if constexpr (bIsWorld || bHasWorld || bIsObject)
                {
                    if (Pointer == nullptr)
                    {
                        return true;
                    }
                    if constexpr (bIsWorld)
                    {
                        State.BindWorld(Pointer);
                    }
                    else if constexpr (bHasWorld)
                    {
                        State.BindWorld(Pointer->GetWorld());
                    }
                    if constexpr (bIsObject)
                    {
                        State.BindOwner(Pointer);
                    }
                    return true;
                }
            }
            return false;
        }

        // The first parameter that names a world or an object binds, which skips a lambda's closure to reach its real first argument.
        template<typename... TArgs>
        void BindParameters(FCoroutineState& State, TArgs&... Args)
        {
            (BindParameter(State, Args) || ...);
        }

        struct FCoroutinePromiseBase
        {
            FStateRef State;

            FCoroutineState& GetState() const { return *State.Get(); }

            std::suspend_never initial_suspend() noexcept { return {}; }

            struct FFinalAwaiter
            {
                bool await_ready() const noexcept { return false; }

                template<typename P>
                void await_suspend(std::coroutine_handle<P> Handle) noexcept
                {
                    FStateRef Keep = Handle.promise().State;
                    Keep->SetHandle({});
                    Handle.destroy();
                    Keep->FinishSucceeded();
                }

                void await_resume() const noexcept {}
            };

            FFinalAwaiter final_suspend() noexcept { return {}; }

            void unhandled_exception() { ASSERT(false); }
        };

        template<typename TPromise>
        constexpr bool TIsCoroutinePromise = std::is_base_of_v<FCoroutinePromiseBase, TPromise>;

        // Wakes whatever awaited, through its state when it is a TCoroutine so a pending cancel is served.
        struct FResumeTarget
        {
            std::coroutine_handle<> Handle{};
            FStateRef               State;

            template<typename TPromise>
            static FResumeTarget For(std::coroutine_handle<TPromise> InHandle)
            {
                FResumeTarget Target;
                Target.Handle = InHandle;
                if constexpr (TIsCoroutinePromise<TPromise>)
                {
                    Target.State = InHandle.promise().State;
                }
                return Target;
            }

            void operator()() const
            {
                if (State)
                {
                    State->Resume();
                }
                else
                {
                    Handle.resume();
                }
            }
        };

        RUNTIME_API void ScheduleOnTask(FResumeTarget Target, Jobs::EJobPriority Priority);
        RUNTIME_API void ScheduleOnGameThread(FResumeTarget Target);
        RUNTIME_API bool IsOnGameThread();

        // Every latent wait, scheduled on the state's world manager and served within one tick of a cancel.
        struct FLatentWaitSpec
        {
            enum class EKind : uint8 { Ticks, Time, Until, External };

            EKind                      Kind = EKind::Ticks;
            ECoroutineClock            Clock = ECoroutineClock::World;
            int64                      Ticks = 0;
            double                     Seconds = 0.0;
            bool                       bAbsolute = false;
            TMoveOnlyFunction<bool()>  Predicate;
        };

        RUNTIME_API void SuspendLatent(FCoroutineState& State, FLatentWaitSpec&& Spec, uint32 Serial);
    }

    template<typename T>
    struct TCoroutinePromise : CoroDetail::FCoroutinePromiseBase
    {
        template<typename... TArgs>
        explicit TCoroutinePromise(TArgs&... Args)
        {
            State = CoroDetail::FStateRef(new CoroDetail::TCoroutineState<T>(), false);
            CoroDetail::BindParameters(GetState(), Args...);
        }

        TCoroutine<T> get_return_object() noexcept;

        template<typename U = T>
        void return_value(U&& Value)
        {
            static_cast<CoroDetail::TCoroutineState<T>&>(GetState()).Result = Forward<U>(Value);
        }
    };

    template<>
    struct TCoroutinePromise<void> : CoroDetail::FCoroutinePromiseBase
    {
        template<typename... TArgs>
        explicit TCoroutinePromise(TArgs&... Args)
        {
            State = CoroDetail::FStateRef(new CoroDetail::TCoroutineState<void>(), false);
            CoroDetail::BindParameters(GetState(), Args...);
        }

        TCoroutine<void> get_return_object() noexcept;

        void return_void() noexcept {}
    };

    namespace CoroDetail
    {
        template<typename T>
        struct TChildAwaiter
        {
            FStateRef Child;

            bool await_ready() const noexcept { return !Child || Child->IsDone(); }

            template<typename TPromise>
            bool await_suspend(std::coroutine_handle<TPromise> Caller)
            {
                FResumeTarget Target = FResumeTarget::For(Caller);
                if (Target.State)
                {
                    Target.State->AwaitChildren({ Child.Get() });
                }
                FStateRef Parent = Target.State;
                const bool bSuspended = Child->TryContinueWith([Target]()
                {
                    if (Target.State)
                    {
                        Target.State->ClearAwaitedChildren();
                    }
                    Target();
                });
                if (!bSuspended && Parent)
                {
                    Parent->ClearAwaitedChildren();
                }
                return bSuspended;
            }

            T await_resume() const
            {
                if constexpr (!std::is_void_v<T>)
                {
                    auto& Typed = static_cast<TCoroutineState<T>&>(*Child.Get());
                    return Typed.Result.IsSet() ? Typed.Result.GetValue() : T{};
                }
            }
        };
    }

    // A game coroutine. It starts at once, stops at its next co_await when canceled, and dies with the object or world it was called on.
    template<typename T>
    class TCoroutine
    {
    public:
        using promise_type = TCoroutinePromise<T>;

        TCoroutine() = default;
        explicit TCoroutine(CoroDetail::FStateRef InState) : State(Move(InState)) {}

        template<typename U> requires (std::is_void_v<T> && !std::is_void_v<U>)
        TCoroutine(const TCoroutine<U>& Other) : State(Other.GetStateRef()) {}

        bool IsValid() const { return static_cast<bool>(State); }
        bool IsDone() const { return !State || State->IsDone(); }
        bool WasSuccessful() const { return State && State->WasSuccessful(); }

        void Cancel() const
        {
            if (State)
            {
                State->Cancel();
            }
        }

        // Runs once it ends for any reason, at once when it already has.
        void ContinueWith(TMoveOnlyFunction<void()> Callback) const
        {
            if (State)
            {
                State->ContinueWith(Move(Callback));
            }
            else
            {
                Callback();
            }
        }

        // Runs only while Object is still alive when the coroutine ends.
        template<typename TObject, typename TFunc>
        void ContinueWithWeak(TObject* Object, TFunc&& Callback) const
        {
            ContinueWith([Weak = TWeakObjectPtr<TObject>(Object), Callback = Forward<TFunc>(Callback)]() mutable
            {
                if (TObject* Alive = Weak.Get())
                {
                    if constexpr (std::is_invocable_v<TFunc, TObject*>) { Callback(Alive); }
                    else                                                { Callback(); }
                }
            });
        }

        // The returned value, or a default one when it was canceled. Only valid once it is done.
        template<typename U = T> requires (!std::is_void_v<U>)
        const U& GetResult() const
        {
            ASSERT(IsDone());
            auto& Typed = static_cast<CoroDetail::TCoroutineState<U>&>(*State.Get());
            if (!Typed.Result.IsSet())
            {
                Typed.Result = U{};
            }
            return Typed.Result.GetValue();
        }

        CoroDetail::TChildAwaiter<T> operator co_await() const noexcept { return CoroDetail::TChildAwaiter<T>{ State }; }

        const CoroDetail::FStateRef& GetStateRef() const { return State; }

    private:
        CoroDetail::FStateRef State;
    };

    template<typename T>
    TCoroutine<T> TCoroutinePromise<T>::get_return_object() noexcept
    {
        GetState().SetHandle(std::coroutine_handle<TCoroutinePromise<T>>::from_promise(*this));
        return TCoroutine<T>(State);
    }

    inline TCoroutine<void> TCoroutinePromise<void>::get_return_object() noexcept
    {
        GetState().SetHandle(std::coroutine_handle<TCoroutinePromise<void>>::from_promise(*this));
        return TCoroutine<void>(State);
    }

    namespace Coro
    {
        namespace Detail
        {
            // Suspends into the world's coroutine manager, which serves a cancel within one tick.
            struct FLatentAwaiter
            {
                CoroDetail::FLatentWaitSpec Spec;

                bool await_ready()
                {
                    switch (Spec.Kind)
                    {
                    case CoroDetail::FLatentWaitSpec::EKind::Ticks: return Spec.Ticks <= 0;
                    case CoroDetail::FLatentWaitSpec::EKind::Time:  return !Spec.bAbsolute && Spec.Seconds <= 0.0;
                    case CoroDetail::FLatentWaitSpec::EKind::Until: return Spec.Predicate();
                    default:                                        return false;
                    }
                }

                template<typename TPromise>
                void await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    static_assert(CoroDetail::TIsCoroutinePromise<TPromise>, "Latent waits run on a world and need a TCoroutine.");
                    CoroDetail::FCoroutineState& State = Caller.promise().GetState();
                    CoroDetail::SuspendLatent(State, Move(Spec), State.BeginWait());
                }

                void await_resume() const noexcept {}
            };

            // Lets an awaiter reach the running coroutine's state without suspending.
            template<typename TFunc>
            struct TStateProbe
            {
                TFunc Func;
                TOptional<std::invoke_result_t<TFunc, CoroDetail::FCoroutineState&>> Value;

                bool await_ready() const noexcept { return false; }

                template<typename TPromise>
                bool await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    static_assert(CoroDetail::TIsCoroutinePromise<TPromise>, "Only a TCoroutine has a cancellation state.");
                    Value.Emplace(Func(Caller.promise().GetState()));
                    return false;
                }

                auto await_resume() { return Move(Value.GetValue()); }
            };

            template<typename TStart>
            struct TChainAwaiter
            {
                TStart Start;

                bool await_ready() const noexcept { return false; }

                template<typename TPromise>
                void await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    CoroDetail::FResumeTarget Target = CoroDetail::FResumeTarget::For(Caller);

                    // Moved out first, since a synchronous resume can end the await and free this awaiter mid-call.
                    TStart LocalStart = Move(Start);
                    if constexpr (CoroDetail::TIsCoroutinePromise<TPromise>)
                    {
                        CoroDetail::FCoroutineState& State = Caller.promise().GetState();
                        const uint32 Serial = State.BeginWait();
                        if (State.GetWorld() != nullptr)
                        {
                            CoroDetail::FLatentWaitSpec Spec;
                            Spec.Kind = CoroDetail::FLatentWaitSpec::EKind::External;
                            CoroDetail::SuspendLatent(State, Move(Spec), Serial);
                        }
                        LocalStart([Target, Serial]()
                        {
                            if (Target.State->ClaimWait(Serial))
                            {
                                Target();
                            }
                        });
                    }
                    else
                    {
                        LocalStart([Target]() { Target(); });
                    }
                }

                void await_resume() const noexcept {}
            };

            struct FMoveToTaskAwaiter
            {
                Jobs::EJobPriority Priority;

                bool await_ready() const noexcept { return false; }

                template<typename TPromise>
                void await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    CoroDetail::ScheduleOnTask(CoroDetail::FResumeTarget::For(Caller), Priority);
                }

                void await_resume() const noexcept {}
            };

            struct FMoveToGameThreadAwaiter
            {
                bool await_ready() const noexcept { return CoroDetail::IsOnGameThread(); }

                template<typename TPromise>
                void await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    CoroDetail::ScheduleOnGameThread(CoroDetail::FResumeTarget::For(Caller));
                }

                void await_resume() const noexcept {}
            };

            struct FFinishNowAwaiter
            {
                bool await_ready() const noexcept { return false; }

                template<typename TPromise>
                bool await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    static_assert(CoroDetail::TIsCoroutinePromise<TPromise>, "Only a TCoroutine can be canceled.");
                    CoroDetail::FCoroutineState& State = Caller.promise().GetState();
                    if (!State.ShouldDieAtSuspension())
                    {
                        return false;
                    }
                    State.DestroyAsCanceled();
                    return true;
                }

                void await_resume() const noexcept {}
            };

            struct FSelfCancelAwaiter
            {
                bool await_ready() const noexcept { return false; }

                template<typename TPromise>
                void await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    static_assert(CoroDetail::TIsCoroutinePromise<TPromise>, "Only a TCoroutine can be canceled.");
                    CoroDetail::FCoroutineState& State = Caller.promise().GetState();
                    State.ForceCancel();
                    State.DestroyAsCanceled();
                }

                void await_resume() const noexcept {}
            };
        }

        inline auto Ticks(int64 Count)
        {
            CoroDetail::FLatentWaitSpec Spec;
            Spec.Kind  = CoroDetail::FLatentWaitSpec::EKind::Ticks;
            Spec.Ticks = Count;
            return Detail::FLatentAwaiter{ Move(Spec) };
        }

        inline auto NextTick() { return Ticks(1); }

        // Resumes once Seconds have passed on Clock, measured from the co_await.
        inline auto Seconds(double Seconds, ECoroutineClock Clock = ECoroutineClock::World)
        {
            CoroDetail::FLatentWaitSpec Spec;
            Spec.Kind    = CoroDetail::FLatentWaitSpec::EKind::Time;
            Spec.Clock   = Clock;
            Spec.Seconds = Seconds;
            return Detail::FLatentAwaiter{ Move(Spec) };
        }

        inline auto UnpausedSeconds(double InSeconds) { return Seconds(InSeconds, ECoroutineClock::Unpaused); }
        inline auto RealSeconds(double InSeconds) { return Seconds(InSeconds, ECoroutineClock::Real); }

        // Resumes once Clock reads at least Time, which a manager reports through FCoroutineManager::GetTime.
        inline auto UntilTime(double Time, ECoroutineClock Clock = ECoroutineClock::World)
        {
            CoroDetail::FLatentWaitSpec Spec;
            Spec.Kind      = CoroDetail::FLatentWaitSpec::EKind::Time;
            Spec.Clock     = Clock;
            Spec.Seconds   = Time;
            Spec.bAbsolute = true;
            return Detail::FLatentAwaiter{ Move(Spec) };
        }

        // Polled once a tick, resuming on the first true.
        inline auto Until(TMoveOnlyFunction<bool()> Predicate)
        {
            CoroDetail::FLatentWaitSpec Spec;
            Spec.Kind      = CoroDetail::FLatentWaitSpec::EKind::Until;
            Spec.Predicate = Move(Predicate);
            return Detail::FLatentAwaiter{ Move(Spec) };
        }

        // Hands Start a resume callback for any callback API, such as an asset load or a delegate, and wakes once it is called.
        template<typename TStart>
        auto Chain(TStart&& Start) { return Detail::TChainAwaiter<std::decay_t<TStart>>{ Forward<TStart>(Start) }; }

        // Waits for anything with OnReady, such as an FAssetHandle.
        template<typename THandle>
        auto WaitForReady(const THandle& Handle)
        {
            return Chain([&Handle](auto Resume)
            {
                Handle.OnReady([Resume = Move(Resume)](const THandle&) { Resume(); });
            });
        }

        inline auto MoveToTask(Jobs::EJobPriority Priority = Jobs::EJobPriority::Normal) { return Detail::FMoveToTaskAwaiter{ Priority }; }

        inline auto MoveToGameThread() { return Detail::FMoveToGameThreadAwaiter{}; }

        namespace Detail
        {
            enum class EJoin : uint8 { All, Any, Race };

            struct FJoinAwaiter
            {
                // Counted by the awaiter and each child continuation, since a continuation can outlive the awaiting frame.
                struct FShared
                {
                    TAtomic<int32>            Remaining{0};
                    TAtomic<int32>            Winner{-1};
                    TAtomic<int32>            Refs{0};
                    CoroDetail::FResumeTarget Target;

                    void Drop()
                    {
                        if (Refs.fetch_sub(1) == 1)
                        {
                            delete this;
                        }
                    }
                };

                TVector<TCoroutine<>> Children;
                EJoin                 Mode;
                FShared*              Shared = nullptr;

                FJoinAwaiter(TVector<TCoroutine<>>&& InChildren, EJoin InMode) : Children(Move(InChildren)), Mode(InMode) {}
                FJoinAwaiter(FJoinAwaiter&& Other) noexcept : Children(Move(Other.Children)), Mode(Other.Mode), Shared(Other.Shared) { Other.Shared = nullptr; }
                FJoinAwaiter(const FJoinAwaiter&) = delete;
                FJoinAwaiter& operator=(const FJoinAwaiter&) = delete;
                FJoinAwaiter& operator=(FJoinAwaiter&&) = delete;
                ~FJoinAwaiter() { if (Shared) { Shared->Drop(); } }

                bool await_ready() const noexcept
                {
                    if (Children.IsEmpty())
                    {
                        return true;
                    }
                    for (const TCoroutine<>& Child : Children)
                    {
                        const bool bDone = Child.IsDone();
                        if (Mode == EJoin::All && !bDone) { return false; }
                        if (Mode != EJoin::All && bDone)  { return true; }
                    }
                    return Mode == EJoin::All;
                }

                template<typename TPromise>
                bool await_suspend(std::coroutine_handle<TPromise> Caller)
                {
                    Shared = new FShared();
                    Shared->Target = CoroDetail::FResumeTarget::For(Caller);
                    Shared->Refs.store((int32)Children.Num() + 1);

                    // One count per child it waits on plus one held here, so no child wakes the caller mid-registration.
                    const int32 Needed = Mode == EJoin::All ? (int32)Children.Num() : 1;
                    Shared->Remaining.store(Needed + 1);
                    if (Shared->Target.State)
                    {
                        TVector<CoroDetail::FCoroutineState*> States;
                        for (const TCoroutine<>& Child : Children) { States.Add(Child.GetStateRef().Get()); }
                        Shared->Target.State->AwaitChildren(Move(States));
                    }

                    FShared* Local = Shared;
                    for (int32 Index = 0; Index < (int32)Children.Num(); ++Index)
                    {
                        Children[Index].ContinueWith([Local, Index]()
                        {
                            int32 Expected = -1;
                            Local->Winner.compare_exchange_strong(Expected, Index);
                            if (Local->Remaining.fetch_sub(1) == 1)
                            {
                                if (Local->Target.State) { Local->Target.State->ClearAwaitedChildren(); }
                                CoroDetail::FResumeTarget Target = Local->Target;
                                Local->Drop();
                                Target();
                                return;
                            }
                            Local->Drop();
                        });
                    }
                    if (Local->Remaining.fetch_sub(1) == 1)
                    {
                        if (Local->Target.State) { Local->Target.State->ClearAwaitedChildren(); }
                        return false;
                    }
                    return true;
                }

                int32 await_resume()
                {
                    int32 Winner = Shared != nullptr ? Shared->Winner.load() : -1;
                    for (int32 Index = 0; Winner < 0 && Index < Children.Num(); ++Index)
                    {
                        if (Children[Index].IsDone()) { Winner = Index; }
                    }
                    if (Mode == EJoin::Race)
                    {
                        for (const TCoroutine<>& Child : Children) { Child.Cancel(); }
                    }
                    return Winner < 0 ? 0 : Winner;
                }
            };

            template<typename... Ts>
            TVector<TCoroutine<>> Collect(const TCoroutine<Ts>&... Coroutines)
            {
                TVector<TCoroutine<>> Out;
                Out.Reserve(sizeof...(Ts));
                (Out.EmplaceGetRef(Coroutines), ...);
                return Out;
            }
        }

        // Resumes once every coroutine has ended.
        inline auto WhenAll(TVector<TCoroutine<>> Coroutines) { return Detail::FJoinAwaiter(Move(Coroutines), Detail::EJoin::All); }

        // Resumes once the first ends, with its index, and leaves the rest running.
        inline auto WhenAny(TVector<TCoroutine<>> Coroutines) { return Detail::FJoinAwaiter(Move(Coroutines), Detail::EJoin::Any); }

        // Resumes once the first ends, with its index, and cancels the rest.
        inline auto Race(TVector<TCoroutine<>> Coroutines) { return Detail::FJoinAwaiter(Move(Coroutines), Detail::EJoin::Race); }

        template<typename... Ts> requires (sizeof...(Ts) > 0)
        auto WhenAll(const TCoroutine<Ts>&... Coroutines) { return WhenAll(Detail::Collect(Coroutines...)); }

        template<typename... Ts> requires (sizeof...(Ts) > 0)
        auto WhenAny(const TCoroutine<Ts>&... Coroutines) { return WhenAny(Detail::Collect(Coroutines...)); }

        template<typename... Ts> requires (sizeof...(Ts) > 0)
        auto Race(const TCoroutine<Ts>&... Coroutines) { return Race(Detail::Collect(Coroutines...)); }

        // True when a cancel is pending, guards included, without serving it.
        inline auto IsCanceled()
        {
            return Detail::TStateProbe{ [](CoroDetail::FCoroutineState& State) { return State.IsCancelRequested(); } };
        }

        // Defers cancellation while it lives, so cleanup that has to await can finish.
        class FCancellationGuard
        {
        public:
            explicit FCancellationGuard(CoroDetail::FCoroutineState* InState) : State(InState) { if (State) { ++State->GuardCount; } }
            FCancellationGuard(FCancellationGuard&& Other) noexcept : State(Other.State) { Other.State = nullptr; }
            FCancellationGuard(const FCancellationGuard&) = delete;
            FCancellationGuard& operator=(const FCancellationGuard&) = delete;
            FCancellationGuard& operator=(FCancellationGuard&&) = delete;
            ~FCancellationGuard() { if (State) { --State->GuardCount; } }

        private:
            CoroDetail::FCoroutineState* State;
        };

        inline auto CancellationGuard()
        {
            return Detail::TStateProbe{ [](CoroDetail::FCoroutineState& State) { return FCancellationGuard(&State); } };
        }

        // Runs Callback only when the coroutine is being torn down by a cancel.
        template<typename TFunc>
        class TOnCanceled
        {
        public:
            TOnCanceled(CoroDetail::FCoroutineState* InState, TFunc&& InFunc) : State(InState), Func(Move(InFunc)) {}
            TOnCanceled(TOnCanceled&& Other) noexcept : State(Other.State), Func(Move(Other.Func)) { Other.State = nullptr; }
            TOnCanceled(const TOnCanceled&) = delete;
            TOnCanceled& operator=(const TOnCanceled&) = delete;
            TOnCanceled& operator=(TOnCanceled&&) = delete;
            ~TOnCanceled() { if (State && State->bDestroyingForCancel) { Func(); } }

        private:
            CoroDetail::FCoroutineState* State;
            TFunc                        Func;
        };

        template<typename TFunc>
        auto OnCanceled(TFunc&& Func)
        {
            return Detail::TStateProbe{ [Func = Forward<TFunc>(Func)](CoroDetail::FCoroutineState& State) mutable
            {
                return TOnCanceled<std::decay_t<TFunc>>(&State, Move(Func));
            } };
        }

        // A cancellation point for a loop that never suspends, cheap when nothing is pending.
        inline auto FinishNowIfCanceled() { return Detail::FFinishNowAwaiter{}; }

        // Cancels the running coroutine and ends it here.
        inline auto SelfCancel() { return Detail::FSelfCancelAwaiter{}; }
    }
}
