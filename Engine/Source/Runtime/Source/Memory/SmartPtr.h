#pragma once

#include <type_traits>
#include <utility>

#include "Containers/ContainerTraits.h"
#include "Core/Threading/Atomic.h"
#include "Memory/Memory.h"

namespace Lumina
{
    template <typename T> class TSharedPtr;
    template <typename T> class TWeakPtr;
    template <typename T> class TSharedFromThis;

    /** Pairs with Memory::New so both ends of the ownership go through the Runtime-exported allocator. */
    template <typename T>
    struct smart_ptr_deleter
    {
        constexpr smart_ptr_deleter() noexcept = default;

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        smart_ptr_deleter(const smart_ptr_deleter<U>&) noexcept {}

        void operator()(T* Ptr) const
        {
            Memory::Delete(Ptr);
        }
    };

    namespace SharedPtrDetail
    {
        enum class EControlAction : uint8
        {
            DestroyValue,
            FreeBlock,
        };

        /** Two counts and one thunk, so the block carries no vtable and a release is a direct call. */
        struct FControlBlock
        {
            using FManageFn = void (*)(FControlBlock*, EControlAction);

            FManageFn       Manage;
            TAtomic<uint32> Strong{ 1 };

            // All strong references share one weak count, so the block outlives the value for weak readers.
            TAtomic<uint32> Weak{ 1 };

            explicit FControlBlock(FManageFn InManage) noexcept : Manage(InManage) {}

            FORCEINLINE void AddStrong() noexcept
            {
                Strong.fetch_add(1, std::memory_order_relaxed);
            }

            NODISCARD bool TryAddStrong() noexcept
            {
                uint32 Current = Strong.load(std::memory_order_relaxed);
                while (Current != 0)
                {
                    if (Strong.compare_exchange_weak(Current, Current + 1,
                                                     std::memory_order_acq_rel, std::memory_order_relaxed))
                    {
                        return true;
                    }
                }

                return false;
            }

            FORCEINLINE void AddWeak() noexcept
            {
                Weak.fetch_add(1, std::memory_order_relaxed);
            }

            void ReleaseStrong() noexcept
            {
                if (Strong.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    Manage(this, EControlAction::DestroyValue);
                    ReleaseWeak();
                }
            }

            void ReleaseWeak() noexcept
            {
                if (Weak.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    Manage(this, EControlAction::FreeBlock);
                }
            }

            NODISCARD uint32 GetStrongCount() const noexcept
            {
                return Strong.load(std::memory_order_relaxed);
            }
        };

        /** MakeShared's block: the value lives inside it, so the pair costs one allocation. */
        template <typename T>
        struct TInlineBlock : FControlBlock
        {
            union { T Value; };

            template <typename... TArgs>
            explicit TInlineBlock(TArgs&&... Args)
                : FControlBlock(&ManageBlock)
            {
                ::new (static_cast<void*>(&Value)) T(std::forward<TArgs>(Args)...);
            }

            // The union member is destroyed by DestroyValue, never by this.
            ~TInlineBlock() {}

            static void ManageBlock(FControlBlock* Base, EControlAction Action)
            {
                TInlineBlock* Self = static_cast<TInlineBlock*>(Base);
                if (Action == EControlAction::DestroyValue)
                {
                    Self->Value.~T();
                    return;
                }

                void* Raw = Self;
                Self->~TInlineBlock();
                Memory::Free(Raw);
            }
        };

        /** A block for a pointer someone else allocated, so it has to remember how to free it. */
        template <typename T, typename TDeleter>
        struct TPointerBlock : FControlBlock
        {
            T*                               Value;
            LUMINA_NO_UNIQUE_ADDRESS TDeleter Deleter;

            TPointerBlock(T* InValue, TDeleter InDeleter)
                : FControlBlock(&ManageBlock), Value(InValue), Deleter(std::move(InDeleter))
            {
            }

            static void ManageBlock(FControlBlock* Base, EControlAction Action)
            {
                TPointerBlock* Self = static_cast<TPointerBlock*>(Base);
                if (Action == EControlAction::DestroyValue)
                {
                    Self->Deleter(Self->Value);
                    return;
                }

                void* Raw = Self;
                Self->~TPointerBlock();
                Memory::Free(Raw);
            }
        };

        template <typename T>
        void BindSharedFromThis(FControlBlock* Block, T* Value, const TSharedFromThis<T>* Base) noexcept;

        FORCEINLINE void BindSharedFromThis(FControlBlock*, const void*, const void*) noexcept {}

        // A striped spin table, so an atomic load or store of a shared pointer needs no extra word in it.
        RUNTIME_API void LockAtomicSlot(const void* Address) noexcept;
        RUNTIME_API void UnlockAtomicSlot(const void* Address) noexcept;

        struct FAtomicSlotGuard
        {
            const void* Address;

            explicit FAtomicSlotGuard(const void* InAddress) noexcept : Address(InAddress)
            {
                LockAtomicSlot(Address);
            }

            ~FAtomicSlotGuard() { UnlockAtomicSlot(Address); }

            FAtomicSlotGuard(const FAtomicSlotGuard&) = delete;
            FAtomicSlotGuard& operator=(const FAtomicSlotGuard&) = delete;
        };
    }

    /** Strong owning pointer, atomically counted; its block routes through Memory::Malloc so it is tracked. */
    template <typename T>
    class TSharedPtr
    {
    public:

        using ElementType = T;

        constexpr TSharedPtr() noexcept = default;
        constexpr TSharedPtr(std::nullptr_t) noexcept {}

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        explicit TSharedPtr(U* InValue)
        {
            AdoptRaw(InValue, smart_ptr_deleter<U>{});
        }

        template <typename U, typename TDeleter>
        requires (std::is_convertible_v<U*, T*> && std::is_invocable_v<TDeleter&, U*>)
        TSharedPtr(U* InValue, TDeleter InDeleter)
        {
            AdoptRaw(InValue, std::move(InDeleter));
        }

        /** Aliasing: shares Owner's count while pointing at something inside it. */
        template <typename U>
        TSharedPtr(const TSharedPtr<U>& Owner, T* InValue) noexcept : Value(InValue), Block(Owner.Block)
        {
            if (Block != nullptr)
            {
                Block->AddStrong();
            }
        }

        TSharedPtr(const TSharedPtr& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            if (Block != nullptr)
            {
                Block->AddStrong();
            }
        }

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TSharedPtr(const TSharedPtr<U>& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            if (Block != nullptr)
            {
                Block->AddStrong();
            }
        }

        TSharedPtr(TSharedPtr&& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            Other.Value = nullptr;
            Other.Block = nullptr;
        }

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TSharedPtr(TSharedPtr<U>&& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            Other.Value = nullptr;
            Other.Block = nullptr;
        }

        ~TSharedPtr()
        {
            if (Block != nullptr)
            {
                Block->ReleaseStrong();
            }
        }

        TSharedPtr& operator=(const TSharedPtr& Other) noexcept
        {
            TSharedPtr(Other).Swap(*this);
            return *this;
        }

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TSharedPtr& operator=(const TSharedPtr<U>& Other) noexcept
        {
            TSharedPtr(Other).Swap(*this);
            return *this;
        }

        TSharedPtr& operator=(TSharedPtr&& Other) noexcept
        {
            TSharedPtr(std::move(Other)).Swap(*this);
            return *this;
        }

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TSharedPtr& operator=(TSharedPtr<U>&& Other) noexcept
        {
            TSharedPtr(std::move(Other)).Swap(*this);
            return *this;
        }

        TSharedPtr& operator=(std::nullptr_t) noexcept
        {
            Reset();
            return *this;
        }

        NODISCARD FORCEINLINE T* Get() const noexcept { return Value; }
        NODISCARD FORCEINLINE T& operator*() const noexcept { return *Value; }
        NODISCARD FORCEINLINE T* operator->() const noexcept { return Value; }
        NODISCARD FORCEINLINE explicit operator bool() const noexcept { return Value != nullptr; }
        NODISCARD FORCEINLINE bool IsValid() const noexcept { return Value != nullptr; }

        NODISCARD uint32 GetRefCount() const noexcept { return Block != nullptr ? Block->GetStrongCount() : 0u; }
        NODISCARD bool IsUnique() const noexcept { return GetRefCount() == 1u; }

        NODISCARD FORCEINLINE T* get() const noexcept { return Value; }

        void Reset() noexcept
        {
            TSharedPtr().Swap(*this);
        }

        void Swap(TSharedPtr& Other) noexcept
        {
            T* const TempValue = Value;
            SharedPtrDetail::FControlBlock* const TempBlock = Block;
            Value = Other.Value;
            Block = Other.Block;
            Other.Value = TempValue;
            Other.Block = TempBlock;
        }

        FORCEINLINE void reset() noexcept { Reset(); }

        NODISCARD friend bool operator==(const TSharedPtr& Left, const TSharedPtr& Right) noexcept
        {
            return Left.Value == Right.Value;
        }

        NODISCARD friend bool operator==(const TSharedPtr& Left, std::nullptr_t) noexcept
        {
            return Left.Value == nullptr;
        }

        NODISCARD friend auto operator<=>(const TSharedPtr& Left, const TSharedPtr& Right) noexcept
        {
            return Left.Value <=> Right.Value;
        }

    private:

        template <typename U> friend class TSharedPtr;
        template <typename U> friend class TWeakPtr;

        template <typename U, typename... TArgs>
        friend TSharedPtr<U> MakeShared(TArgs&&... Args);

        TSharedPtr(T* InValue, SharedPtrDetail::FControlBlock* InBlock) noexcept
            : Value(InValue), Block(InBlock)
        {
        }

        template <typename U, typename TDeleter>
        void AdoptRaw(U* InValue, TDeleter InDeleter)
        {
            if (InValue == nullptr)
            {
                return;
            }

            using FBlock = SharedPtrDetail::TPointerBlock<U, TDeleter>;
            void* Storage = Memory::Malloc(sizeof(FBlock), alignof(FBlock));

            Value = InValue;
            Block = ::new (Storage) FBlock(InValue, std::move(InDeleter));

            SharedPtrDetail::BindSharedFromThis(Block, InValue, InValue);
        }

        T*                              Value = nullptr;
        SharedPtrDetail::FControlBlock* Block = nullptr;
    };

    /** A non-owning observer. Pin it for a strong pointer, which comes back empty once the value is gone. */
    template <typename T>
    class TWeakPtr
    {
    public:

        constexpr TWeakPtr() noexcept = default;
        constexpr TWeakPtr(std::nullptr_t) noexcept {}

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TWeakPtr(const TSharedPtr<U>& Strong) noexcept : Value(Strong.Value), Block(Strong.Block)
        {
            if (Block != nullptr)
            {
                Block->AddWeak();
            }
        }

        TWeakPtr(const TWeakPtr& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            if (Block != nullptr)
            {
                Block->AddWeak();
            }
        }

        TWeakPtr(TWeakPtr&& Other) noexcept : Value(Other.Value), Block(Other.Block)
        {
            Other.Value = nullptr;
            Other.Block = nullptr;
        }

        ~TWeakPtr()
        {
            if (Block != nullptr)
            {
                Block->ReleaseWeak();
            }
        }

        TWeakPtr& operator=(const TWeakPtr& Other) noexcept
        {
            TWeakPtr(Other).Swap(*this);
            return *this;
        }

        TWeakPtr& operator=(TWeakPtr&& Other) noexcept
        {
            TWeakPtr(std::move(Other)).Swap(*this);
            return *this;
        }

        template <typename U>
        requires std::is_convertible_v<U*, T*>
        TWeakPtr& operator=(const TSharedPtr<U>& Strong) noexcept
        {
            TWeakPtr(Strong).Swap(*this);
            return *this;
        }

        TWeakPtr& operator=(std::nullptr_t) noexcept
        {
            Reset();
            return *this;
        }

        /** A strong pointer while the value is alive, an empty one once it is not. */
        NODISCARD TSharedPtr<T> Pin() const noexcept
        {
            if (Block != nullptr && Block->TryAddStrong())
            {
                return TSharedPtr<T>(Value, Block);
            }

            return TSharedPtr<T>();
        }

        NODISCARD FORCEINLINE TSharedPtr<T> lock() const noexcept { return Pin(); }

        NODISCARD bool IsExpired() const noexcept { return Block == nullptr || Block->GetStrongCount() == 0; }
        NODISCARD FORCEINLINE bool expired() const noexcept { return IsExpired(); }

        void Reset() noexcept { TWeakPtr().Swap(*this); }

        void Swap(TWeakPtr& Other) noexcept
        {
            T* const TempValue = Value;
            SharedPtrDetail::FControlBlock* const TempBlock = Block;
            Value = Other.Value;
            Block = Other.Block;
            Other.Value = TempValue;
            Other.Block = TempBlock;
        }

    private:

        template <typename U> friend class TSharedFromThis;

        T*                              Value = nullptr;
        SharedPtrDetail::FControlBlock* Block = nullptr;
    };

    /** Lets a value hand out a strong pointer to itself, but only once a TSharedPtr already owns it. */
    template <typename T>
    class TSharedFromThis
    {
    public:

        NODISCARD TSharedPtr<T> AsShared() const noexcept { return WeakThis.Pin(); }
        NODISCARD FORCEINLINE TSharedPtr<T> shared_from_this() const noexcept { return AsShared(); }
        NODISCARD TWeakPtr<T> AsWeak() const noexcept { return WeakThis; }

    protected:

        constexpr TSharedFromThis() noexcept = default;
        TSharedFromThis(const TSharedFromThis&) noexcept {}
        TSharedFromThis& operator=(const TSharedFromThis&) noexcept { return *this; }
        ~TSharedFromThis() = default;

    private:

        template <typename U>
        friend void SharedPtrDetail::BindSharedFromThis(SharedPtrDetail::FControlBlock*, U*,
                                                        const TSharedFromThis<U>*) noexcept;

        void Bind(T* Value, SharedPtrDetail::FControlBlock* Block) const noexcept
        {
            WeakThis.Value = Value;
            WeakThis.Block = Block;
            Block->AddWeak();
        }

        mutable TWeakPtr<T> WeakThis;
    };

    namespace SharedPtrDetail
    {
        template <typename T>
        void BindSharedFromThis(FControlBlock* Block, T* Value, const TSharedFromThis<T>* Base) noexcept
        {
            Base->Bind(Value, Block);
        }
    }

    /** One allocation for the value and its counts, which is why it beats adopting a raw pointer. */
    template <typename T, typename... TArgs>
    NODISCARD TSharedPtr<T> MakeShared(TArgs&&... Args)
    {
        using FBlock = SharedPtrDetail::TInlineBlock<T>;

        void* Storage = Memory::Malloc(sizeof(FBlock), alignof(FBlock));
        FBlock* Block = ::new (Storage) FBlock(std::forward<TArgs>(Args)...);

        T* Value = &Block->Value;
        SharedPtrDetail::BindSharedFromThis(Block, Value, Value);

        return TSharedPtr<T>(Value, Block);
    }

    template <typename T, typename U>
    NODISCARD TSharedPtr<T> StaticCastSharedPtr(const TSharedPtr<U>& Other) noexcept
    {
        return TSharedPtr<T>(Other, static_cast<T*>(Other.Get()));
    }

    /** Publishes and consumes a shared pointer across threads; the pointer itself carries no lock. */
    template <typename T>
    NODISCARD TSharedPtr<T> AtomicLoad(const TSharedPtr<T>* Target) noexcept
    {
        const SharedPtrDetail::FAtomicSlotGuard Guard(Target);
        return *Target;
    }

    template <typename T>
    void AtomicStore(TSharedPtr<T>* Target, TSharedPtr<T> Value) noexcept
    {
        {
            const SharedPtrDetail::FAtomicSlotGuard Guard(Target);
            Target->Swap(Value);
        }

        // Value now holds the old pointer, released here rather than under the lock.
    }

    template <typename T>
    NODISCARD TSharedPtr<T> AtomicExchange(TSharedPtr<T>* Target, TSharedPtr<T> Value) noexcept
    {
        {
            const SharedPtrDetail::FAtomicSlotGuard Guard(Target);
            Target->Swap(Value);
        }

        return Value;
    }

    /** Single owner, freed through smart_ptr_deleter unless you name another one. */
    template <typename T, typename TDeleter = smart_ptr_deleter<T>>
    class TUniquePtr
    {
    public:

        using ElementType = T;
        using DeleterType = TDeleter;

        constexpr TUniquePtr() noexcept = default;
        constexpr TUniquePtr(std::nullptr_t) noexcept {}

        explicit TUniquePtr(T* InValue) noexcept : Value(InValue) {}
        TUniquePtr(T* InValue, TDeleter InDeleter) noexcept : Value(InValue), Deleter(std::move(InDeleter)) {}

        TUniquePtr(const TUniquePtr&) = delete;
        TUniquePtr& operator=(const TUniquePtr&) = delete;

        TUniquePtr(TUniquePtr&& Other) noexcept
            : Value(Other.Value), Deleter(std::move(Other.Deleter))
        {
            Other.Value = nullptr;
        }

        template <typename U, typename UDeleter>
        requires (std::is_convertible_v<U*, T*> && std::is_constructible_v<TDeleter, UDeleter &&>)
        TUniquePtr(TUniquePtr<U, UDeleter>&& Other) noexcept
            : Value(Other.Release()), Deleter(std::move(Other.GetDeleter()))
        {
        }

        ~TUniquePtr()
        {
            if (Value != nullptr)
            {
                Deleter(Value);
            }
        }

        TUniquePtr& operator=(TUniquePtr&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset(Other.Release());
                Deleter = std::move(Other.Deleter);
            }

            return *this;
        }

        template <typename U, typename UDeleter>
        requires (std::is_convertible_v<U*, T*> && std::is_assignable_v<TDeleter&, UDeleter &&>)
        TUniquePtr& operator=(TUniquePtr<U, UDeleter>&& Other) noexcept
        {
            Reset(Other.Release());
            Deleter = std::move(Other.GetDeleter());
            return *this;
        }

        TUniquePtr& operator=(std::nullptr_t) noexcept
        {
            Reset();
            return *this;
        }

        NODISCARD FORCEINLINE T* Get() const noexcept { return Value; }
        NODISCARD FORCEINLINE T& operator*() const noexcept { return *Value; }
        NODISCARD FORCEINLINE T* operator->() const noexcept { return Value; }
        NODISCARD FORCEINLINE explicit operator bool() const noexcept { return Value != nullptr; }
        NODISCARD FORCEINLINE bool IsValid() const noexcept { return Value != nullptr; }

        NODISCARD FORCEINLINE TDeleter& GetDeleter() noexcept { return Deleter; }
        NODISCARD FORCEINLINE const TDeleter& GetDeleter() const noexcept { return Deleter; }

        NODISCARD T* Release() noexcept
        {
            T* const Released = Value;
            Value = nullptr;
            return Released;
        }

        void Reset(T* InValue = nullptr) noexcept
        {
            T* const Old = Value;
            Value = InValue;
            if (Old != nullptr)
            {
                Deleter(Old);
            }
        }

        void Swap(TUniquePtr& Other) noexcept
        {
            T* const TempValue = Value;
            Value = Other.Value;
            Other.Value = TempValue;

            TDeleter TempDeleter = std::move(Deleter);
            Deleter = std::move(Other.Deleter);
            Other.Deleter = std::move(TempDeleter);
        }

        NODISCARD FORCEINLINE T* get() const noexcept { return Value; }
        FORCEINLINE T* release() noexcept { return Release(); }
        FORCEINLINE void reset(T* InValue = nullptr) noexcept { Reset(InValue); }

        NODISCARD friend bool operator==(const TUniquePtr& Left, const TUniquePtr& Right) noexcept
        {
            return Left.Value == Right.Value;
        }

        NODISCARD friend bool operator==(const TUniquePtr& Left, std::nullptr_t) noexcept
        {
            return Left.Value == nullptr;
        }

        NODISCARD friend auto operator<=>(const TUniquePtr& Left, const TUniquePtr& Right) noexcept
        {
            return Left.Value <=> Right.Value;
        }

    private:

        template <typename U, typename UDeleter> friend class TUniquePtr;

        T*                                Value = nullptr;
        LUMINA_NO_UNIQUE_ADDRESS TDeleter Deleter{};
    };

    template <typename T, typename... TArgs>
    requires (std::is_constructible_v<T, TArgs...>)
    NODISCARD TUniquePtr<T> MakeUnique(TArgs&&... Args)
    {
        return TUniquePtr<T>(Memory::New<T>(std::forward<TArgs>(Args)...));
    }

    template <typename T>
    FORCEINLINE void swap(TSharedPtr<T>& Left, TSharedPtr<T>& Right) noexcept { Left.Swap(Right); }

    template <typename T, typename D>
    FORCEINLINE void swap(TUniquePtr<T, D>& Left, TUniquePtr<T, D>& Right) noexcept { Left.Swap(Right); }

    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TSharedPtr<T>& Ptr) noexcept
    {
        return GetTypeHash(Ptr.Get());
    }

    template <typename T, typename D>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TUniquePtr<T, D>& Ptr) noexcept
    {
        return GetTypeHash(Ptr.Get());
    }

    // None of these hold a pointer into themselves, so a container can relocate them with a memcpy.
    template <typename T>
    struct TIsTriviallyRelocatable<TSharedPtr<T>> { static constexpr bool Value = true; };

    template <typename T>
    struct TIsTriviallyRelocatable<TWeakPtr<T>> { static constexpr bool Value = true; };

    template <typename T, typename D>
    struct TIsTriviallyRelocatable<TUniquePtr<T, D>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<D>;
    };
}
