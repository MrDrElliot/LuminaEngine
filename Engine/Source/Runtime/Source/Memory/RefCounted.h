#pragma once

#include "Memory.h"
#include "Core/Assertions/Assert.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Threading/Atomic.h"

namespace Lumina
{
	class RUNTIME_API IRefCountedObject
	{
	public:
		virtual ~IRefCountedObject() = default;
		virtual uint32 AddRef() const = 0;
		virtual uint32 Release() = 0;
		virtual uint32 GetRefCount() const = 0;
	};

	/** Atomic intrusive ref counting. */
	class RUNTIME_API IRefCounted
	{
	public:

		IRefCounted() = default;
		virtual ~IRefCounted() = default;
		IRefCounted(const IRefCounted&) = delete;
		IRefCounted& operator = (const IRefCounted&) = delete;

		uint32 AddRef() const
		{
			return RefCount.fetch_add(1, std::memory_order_relaxed);
		}

		uint32 Release() const
		{
			int Value = RefCount.fetch_sub(1, std::memory_order_release);
			if(Value == 1)
			{
				std::atomic_thread_fence(std::memory_order_acquire);
				Memory::Delete(this);
			}

			return Value;
		}

		uint32 GetRefCount() const { return RefCount.load(std::memory_order_relaxed); }

		bool IsValid() const { return RefCount.load(std::memory_order_acquire) > 0; }

	private:

		mutable TAtomic<int> RefCount{0};

	};


	template<typename T>
	concept TRefCounter = requires(T t)
	{
		{ t.Release() } -> std::same_as<uint32>;
		{ t.AddRef() }	-> std::same_as<uint32>;
	};
	
	/** Smart pointer for objects with AddRef/Release. */
	template<typename ReferencedType>
	class TRefCountPtr
	{
		typedef ReferencedType* ReferenceType;
	
	public:

		TRefCountPtr():
			Reference(nullptr)
		{ }

		TRefCountPtr(ReferencedType* InReference, bool bAddRef = true)
		{
			Reference = InReference;
			if(Reference && bAddRef)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(const TRefCountPtr& Copy)
		{
			Reference = Copy.Reference;
			if(Reference)
			{
				Reference->AddRef();
			}
		}

		template<typename CopyReferencedType>
		requires eastl::is_base_of_v<ReferencedType, CopyReferencedType>
		TRefCountPtr(const TRefCountPtr<CopyReferencedType>& Copy)
		{
			Reference = static_cast<ReferencedType*>(Copy.GetReference());
			if (Reference)
			{
				Reference->AddRef();
			}
		}

		TRefCountPtr(TRefCountPtr&& Move) noexcept
		{
			Reference = Move.Reference;
			Move.Reference = nullptr;
		}

		template<typename MoveReferencedType>
		requires eastl::is_base_of_v<ReferencedType, MoveReferencedType>
		TRefCountPtr(TRefCountPtr<MoveReferencedType>&& Move)
		{
			Reference = static_cast<ReferencedType*>(Move.GetReference());
			Move.Reference = nullptr;
		}


		~TRefCountPtr()
		{
			if(Reference != nullptr)
			{
				Reference->Release();
				Reference = nullptr;
			}
		}

		TRefCountPtr& operator=(ReferencedType* InReference)
		{
			if (Reference != InReference)
			{
				// AddRef before Release: handles the new == old case.
				ReferencedType* OldReference = Reference;
				Reference = InReference;
				if (Reference)
				{
					Reference->AddRef();
				}
				if (OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		template<typename... Args>
		static TRefCountPtr Create(Args&&... args)
		{
			return TRefCountPtr(Memory::New<ReferencedType>(Forward<Args>(args)...));
		}
		
		template<typename T>
		requires eastl::is_base_of_v<ReferencedType, T>
		TRefCountPtr<T> As()
		{
			return TRefCountPtr<T>(static_cast<T*>(Reference));
		}

		template<typename T>
		requires eastl::is_base_of_v<ReferencedType, T>
		TRefCountPtr<T> As() const
		{
			return TRefCountPtr<T>(static_cast<T*>(Reference));
		}
	
		FORCEINLINE TRefCountPtr& operator=(const TRefCountPtr& InPtr)
		{
			return *this = InPtr.Reference;
		}

		template<typename CopyReferencedType>
		FORCEINLINE TRefCountPtr& operator=(const TRefCountPtr<CopyReferencedType>& InPtr)
		{
			return *this = InPtr.GetReference();
		}

		TRefCountPtr& operator=(TRefCountPtr&& InPtr) noexcept
		{
			if (this != &InPtr)
			{
				ReferencedType* OldReference = Reference;
				Reference = InPtr.Reference;
				InPtr.Reference = nullptr;
				if(OldReference)
				{
					OldReference->Release();
				}
			}
			return *this;
		}

		template<typename MoveReferencedType>
		TRefCountPtr& operator=(TRefCountPtr<MoveReferencedType>&& InPtr)
		{
			// Different type, so &InPtr != this.
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = nullptr;
			if (OldReference)
			{
				OldReference->Release();
			}
			return *this;
		}

		FORCEINLINE ReferencedType* operator->() const
		{
			return Reference;
		}

		FORCEINLINE operator ReferenceType() const
		{
			return Reference;
		}

		FORCEINLINE ReferencedType** GetInitReference()
		{
			*this = nullptr;
			return &Reference;
		}

		FORCEINLINE ReferencedType* GetReference() const
		{
			return Reference;
		}

		FORCEINLINE friend bool IsValidRef(const TRefCountPtr& InReference)
		{
			return InReference.Reference != nullptr;
		}

		FORCEINLINE bool IsValid() const
		{
			return Reference != nullptr;
		}

		FORCEINLINE void SafeRelease()
		{
			*this = nullptr;
		}

		uint32 GetRefCount() const
		{
			uint32 Result = 0;
			if (Reference)
			{
				Result = Reference->GetRefCount();
				DEBUG_ASSERT(Result > 0);
			}
			return Result;
		}

		FORCEINLINE void Swap(TRefCountPtr& InPtr)
		{
			ReferencedType* OldReference = Reference;
			Reference = InPtr.Reference;
			InPtr.Reference = OldReference;
		}

	private:

		ReferencedType* Reference;

		template <typename OtherType>
		friend class TRefCountPtr;

	public:
		bool operator==(const TRefCountPtr& B) const
		{
			return GetReference() == B.GetReference();
		}

		bool operator==(ReferencedType* B) const
		{
			return GetReference() == B;
		}
	};
}

namespace eastl
{
	template <typename T>
	struct hash<Lumina::TRefCountPtr<T>>
	{
		std::size_t operator()(const Lumina::TRefCountPtr<T>& handle) const noexcept
		{
			return eastl::hash<T*>()(handle.GetReference());
		}
	};
}

template<typename T, typename... TArgs>
requires std::is_constructible_v<T, TArgs...> && (!eastl::is_array_v<T>) && (!eastl::is_abstract_v<T>)
Lumina::TRefCountPtr<T> MakeRefCount(TArgs&&... Args)
{
	return Lumina::TRefCountPtr<T>(Lumina::Memory::New<T>(std::forward<TArgs>(Args)...));
}
