#pragma once
#include <type_traits>
#include "Core/Utils/NonCopyable.h"
#include "EASTL/internal/atomic/atomic.h"
#include "Platform/Platform.h"


namespace Lumina
{
	/** Forces a value to return to its original value when this goes out of scope */
    template <typename RefType, typename AssignedType = RefType>
	struct TGuardValue : private INonCopyable
	{
		NODISCARD TGuardValue(RefType& ReferenceValue, const AssignedType& NewValue)
		: RefValue(ReferenceValue), OriginalValue(ReferenceValue)
		{
			RefValue = NewValue;
		}
    	
		~TGuardValue()
		{
			RefValue = OriginalValue;
		}
    	
		FORCEINLINE const AssignedType& GetOriginalValue() const
		{
			return OriginalValue;
		}
	
	private:
		RefType& RefValue;
		AssignedType OriginalValue;
	};

	/** Forces an atomic value to return to its original value when this goes out of scope */
	template <typename AtomicType>
	requires(eastl::atomic<AtomicType>::is_always_lock_free)
	struct TGuardAtomicValue : private INonCopyable
	{
	    NODISCARD TGuardAtomicValue(eastl::atomic<AtomicType>& InAtomic, AtomicType NewValue)
	        : AtomicRef(InAtomic)
	        , OriginalValue(InAtomic.load(std::memory_order_relaxed))
	    {
	        AtomicRef.store(NewValue, std::memory_order_relaxed);
	    }
	
	    ~TGuardAtomicValue()
	    {
	        AtomicRef.store(OriginalValue, std::memory_order_relaxed);
	    }
	
	    FORCEINLINE const AtomicType& GetOriginalValue() const
	    {
	        return OriginalValue;
	    }
	
	private:
	    eastl::atomic<AtomicType>& AtomicRef;
	    AtomicType OriginalValue;
	};

	template<typename T>
	requires(eastl::is_integral_v<T> && eastl::atomic<T>::is_always_lock_free)
	struct TAtomicScopeGuard : private INonCopyable
	{
	    eastl::atomic<T>& Ref;
	    T Delta;
	
	    explicit TAtomicScopeGuard(eastl::atomic<T>& InRef, T InDelta)
	        : Ref(InRef), Delta(InDelta)
	    {
	        Ref.fetch_add(Delta, std::memory_order_relaxed);
	    }
	
	    ~TAtomicScopeGuard()
	    {
	        Ref.fetch_sub(Delta, std::memory_order_acq_rel);
	    }
	};
	
    

    template <typename T>
    FORCEINLINE T ImplicitConv(std::type_identity_t<T> Obj)
    {
        return Obj;
    }

    template <typename T>
    requires(eastl::is_lvalue_reference_v<T> && !eastl::is_const_v<T>)
    FORCEINLINE constexpr eastl::remove_reference<T>::type&& Move(T&& x) noexcept
    {
        return static_cast<eastl::remove_reference<T>::type&&>(eastl::forward<T>(x));
    }

    
    template <typename T>
    FORCEINLINE constexpr T&& Forward(eastl::remove_reference_t<T>& x) noexcept
    {
        return static_cast<T&&>(x);
    }


    template <typename T>
    requires(!eastl::is_lvalue_reference_v<T>)
    FORCEINLINE constexpr T&& Forward(eastl::remove_reference_t<T>&& x) noexcept
    {
        return static_cast<T&&>(x);
    }
}