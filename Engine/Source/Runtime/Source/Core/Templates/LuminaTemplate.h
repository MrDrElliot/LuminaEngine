#pragma once
#include <type_traits>
#include "Core/Utils/NonCopyable.h"
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
	requires(std::atomic<AtomicType>::is_always_lock_free)
	struct TGuardAtomicValue : private INonCopyable
	{
	    NODISCARD TGuardAtomicValue(std::atomic<AtomicType>& InAtomic, AtomicType NewValue)
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
	    std::atomic<AtomicType>& AtomicRef;
	    AtomicType OriginalValue;
	};

	template<typename T>
	requires(std::is_integral_v<T> && std::atomic<T>::is_always_lock_free)
	struct TAtomicScopeGuard : private INonCopyable
	{
	    std::atomic<T>& Ref;
	    T Delta;
	
	    explicit TAtomicScopeGuard(std::atomic<T>& InRef, T InDelta)
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
    requires(std::is_lvalue_reference_v<T> && !std::is_const_v<T>)
    FORCEINLINE constexpr std::remove_reference<T>::type&& Move(T&& x) noexcept
    {
        return static_cast<std::remove_reference<T>::type&&>(std::forward<T>(x));
    }

    
    template <typename T>
    FORCEINLINE constexpr T&& Forward(std::remove_reference_t<T>& x) noexcept
    {
        return static_cast<T&&>(x);
    }


    template <typename T>
    requires(!std::is_lvalue_reference_v<T>)
    FORCEINLINE constexpr T&& Forward(std::remove_reference_t<T>&& x) noexcept
    {
        return static_cast<T&&>(x);
    }
}
