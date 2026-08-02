#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Core/Assertions/Assert.h"
#include "Core/Threading/Atomic.h"

namespace Lumina
{
    // Handle for tracking delegates
    struct FDelegateHandle
    {
        uint64 ID = 0;
        
        bool IsValid() const { return ID != 0; }
        void Reset() { ID = 0; }
        
        bool operator==(const FDelegateHandle& Other) const { return ID == Other.ID; }
        bool operator!=(const FDelegateHandle& Other) const { return ID != Other.ID; }
    };

    template<typename R, typename ... TArgs>
    class TBaseDelegate
    {
    public:
        using FFunc = TFunction<R(TArgs...)>;

        TBaseDelegate() = default;
        
        template<typename TFunc>
        requires(eastl::is_invocable_r_v<R, TFunc, TArgs...>)
        static TBaseDelegate CreateStatic(TFunc&& Func)
        {
            TBaseDelegate Delegate;
            Delegate.Func = eastl::forward<TFunc>(Func);
            return Delegate;
        }

        template<typename TObject, typename TMemFunc>
        static TBaseDelegate CreateMember(TObject* Object, TMemFunc Method)
        {
            TBaseDelegate Delegate;
            Delegate.Func = [Object, Method](TArgs... args) -> R
            {
                return (Object->*Method)(eastl::forward<TArgs>(args)...);
            };
            return Delegate;
        }

        template<typename TLambda>
        requires(eastl::is_invocable_r_v<R, TLambda, TArgs...>)
        static TBaseDelegate CreateLambda(TLambda&& Lambda)
        {
            TBaseDelegate Delegate;
            Delegate.Func = eastl::forward<TLambda>(Lambda);
            return Delegate;
        }

        bool IsBound() const { return static_cast<bool>(Func); }

        template<typename... TCallArgs>
        R Execute(TCallArgs&&... Args) const
        {
            ASSERT(Func);

            if constexpr (eastl::is_void_v<R>)
            {
                Func(eastl::forward<TCallArgs>(Args)...);
                return;
            }
            else
            {
                return Func(eastl::forward<TCallArgs>(Args)...);
            }
        }

        // Calls the bound function if any. Only valid for void-returning delegates.
        template<typename... TCallArgs>
        bool ExecuteIfBound(TCallArgs&&... Args) const requires(eastl::is_void_v<R>)
        {
            if (!Func)
            {
                return false;
            }
            Func(eastl::forward<TCallArgs>(Args)...);
            return true;
        }

        void Unbind() { Func = nullptr; }

    private:
        FFunc Func;
    };
    
    template<typename R, typename... Args>
    class TMulticastDelegate
    {
    public:
        using FBase = TBaseDelegate<R, Args...>;

        TMulticastDelegate() noexcept = default;

        template<typename TFunc>
        NODISCARD FDelegateHandle AddStatic(TFunc&& Func)
        {
            return Add(FBase::CreateStatic(eastl::forward<TFunc>(Func)));
        }

        template<typename TObject, typename TMemFunc>
        NODISCARD FDelegateHandle AddMember(TObject* Object, TMemFunc Method)
        {
            return Add(FBase::CreateMember(Object, Method));
        }

        template<typename TLambda>
        NODISCARD FDelegateHandle AddLambda(TLambda&& Lambda)
        {
            return Add(FBase::CreateLambda(eastl::forward<TLambda>(Lambda)));
        }

        bool Remove(FDelegateHandle Handle)
        {
            if (!Handle.IsValid())
            {
                return false;
            }

            for (SIZE_T Index = 0; Index < InvocationList.size(); ++Index)
            {
                if (InvocationList[Index].Handle == Handle)
                {
                    if (LockCount > 0)
                    {
                        DeferRemove(InvocationList[Index]);
                    }
                    else
                    {
                        InvocationList.erase(InvocationList.begin() + Index);
                    }
                    return true;
                }
            }
            return false;
        }

        void RemoveAll()
        {
            Clear();
        }

        void Clear()
        {
            if (LockCount > 0)
            {
                for (FDelegateEntry& Entry : InvocationList)
                {
                    DeferRemove(Entry);
                }
            }
            else
            {
                InvocationList.clear();
                InvocationList.shrink_to_fit();
            }
        }

        template<typename... CallArgs>
        void Broadcast(CallArgs&&... args)
        {
            ++LockCount;
            
            const SIZE_T Count = InvocationList.size();
            for (SIZE_T Index = 0; Index < Count; ++Index)
            {
                if (InvocationList[Index].Delegate.IsBound())
                {
                    FBase Delegate = InvocationList[Index].Delegate;
                    Delegate.Execute(args...);
                }
            }

            Unlock();
        }

        template<typename... CallArgs>
        void BroadcastAndClear(CallArgs&&... args)
        {
            Broadcast(eastl::forward<CallArgs>(args)...);
            Clear();
        }

        bool IsBound() const
        {
            for (const FDelegateEntry& Entry : InvocationList)
            {
                if (Entry.Delegate.IsBound())
                {
                    return true;
                }
            }
            return false;
        }

        size_t GetCount() const { return InvocationList.size(); }

    private:
        struct FDelegateEntry
        {
            FDelegateHandle Handle;
            FBase Delegate;
        };

        FDelegateHandle Add(FBase&& Delegate)
        {
            const FDelegateHandle Handle = GenerateHandle();
            InvocationList.push_back({Handle, eastl::move(Delegate)});
            return Handle;
        }

        // Marks an entry dead during an active broadcast; compacted away on unwind.
        void DeferRemove(FDelegateEntry& Entry)
        {
            Entry.Handle.Reset();
            Entry.Delegate.Unbind();
            bCompactionPending = true;
        }

        void Unlock()
        {
            ASSERT(LockCount > 0);
            if (--LockCount == 0 && bCompactionPending)
            {
                bCompactionPending = false;

                SIZE_T Write = 0;
                for (SIZE_T Read = 0; Read < InvocationList.size(); ++Read)
                {
                    if (InvocationList[Read].Delegate.IsBound())
                    {
                        if (Write != Read)
                        {
                            InvocationList[Write] = eastl::move(InvocationList[Read]);
                        }
                        ++Write;
                    }
                }
                InvocationList.resize(Write);
            }
        }

        static FDelegateHandle GenerateHandle()
        {
            static TAtomic<uint64> NextID{1};
            FDelegateHandle Handle;
            Handle.ID = NextID.fetch_add(1, Atomic::MemoryOrderRelaxed);
            return Handle;
        }

        TVector<FDelegateEntry> InvocationList;
        int32                   LockCount = 0;
        bool                    bCompactionPending = false;
    };
}

#define DECLARE_MULTICAST_DELEGATE(DelegateName, ...) \
struct DelegateName \
: public Lumina::TMulticastDelegate<void __VA_OPT__(,) __VA_ARGS__> {}

#define DECLARE_MULTICAST_DELEGATE_R(DelegateName, ...) \
struct DelegateName : public Lumina::TMulticastDelegate<__VA_ARGS__> {}