#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"
#include "Invoke.h"
#include "Memory/Memcpy.h"
#include "Memory/Construct.h"

namespace Lumina::Containers
{
    /** Inline room for a closure over four pointers, which covers most engine callbacks without touching the heap. */
    inline constexpr size_t kFunctionInlineCapacity = 4 * sizeof(void*);

    template <typename T>
    struct TInPlaceType
    {
        explicit constexpr TInPlaceType() = default;
    };

    namespace Private
    {
        template <typename T>
        inline constexpr bool bIsInPlaceType = false;

        template <typename T>
        inline constexpr bool bIsInPlaceType<TInPlaceType<T>> = true;

        // A concept, not a variable template: a conjunction stops at the first false rather than substituting
        // into the rest, which is what keeps a function built from another function from recursing forever.
        template <typename TCallable, typename TSelf, typename TRet, bool bCopyable, typename... TArgs>
        concept StorableCallable =
            (!std::is_same_v<std::decay_t<TCallable>, TSelf>) &&
            (!bIsInPlaceType<std::decay_t<TCallable>>) &&
            std::is_constructible_v<std::decay_t<TCallable>, TCallable> &&
            std::is_invocable_r_v<TRet, std::decay_t<TCallable>&, TArgs...> &&
            (!bCopyable || std::is_copy_constructible_v<std::decay_t<TCallable>>);
    }

    template <typename TSignature, bool bCopyable, size_t InlineCapacity, ContainerAllocatorType TAllocator>
    class TBasicFunction;

    /** An owning callable; small targets live in the inline buffer, anything larger goes through TAllocator. */
    template <typename TRet, typename... TArgs, bool bCopyable, size_t InlineCapacity, ContainerAllocatorType TAllocator>
    class TBasicFunction<TRet(TArgs...), bCopyable, InlineCapacity, TAllocator>
    {
        static_assert(InlineCapacity >= sizeof(void*), "Inline storage has to be wide enough for the heap pointer.");

        union FStorage
        {
            void* Heap;
            uint8 Inline[InlineCapacity];
        };

        struct FVTable
        {
            TRet (*Invoke)(void* Storage, TArgs&&... Args);
            void (*Destroy)(void* Storage) noexcept;
            void (*Copy)(void* Dest, const void* Source);

            // Null when moving the storage is a plain byte copy, which is the common case.
            void (*Relocate)(void* Dest, void* Source) noexcept;

            bool   bInline;
            uint32 RelocateBytes;
        };

        template <typename TCallable>
        static constexpr bool bFitsInline =
            sizeof(TCallable) <= InlineCapacity &&
            alignof(TCallable) <= alignof(FStorage) &&
            std::is_nothrow_move_constructible_v<TCallable>;

        template <typename TCallable, bool bInline>
        struct TOps
        {
            static constexpr bool bNeedsRelocate = bInline && !TIsTriviallyRelocatable_V<TCallable>;

            NODISCARD static TCallable* Target(void* Storage) noexcept
            {
                if constexpr (bInline)
                {
                    return reinterpret_cast<TCallable*>(static_cast<FStorage*>(Storage)->Inline);
                }
                else
                {
                    return static_cast<TCallable*>(static_cast<FStorage*>(Storage)->Heap);
                }
            }

            static TRet InvokeThunk(void* Storage, TArgs&&... Args)
            {
                return InvokeR<TRet>(*Target(Storage), std::forward<TArgs>(Args)...);
            }

            static void DestroyThunk(void* Storage) noexcept
            {
                TCallable* Callable = Target(Storage);
                Callable->~TCallable();

                if constexpr (!bInline)
                {
                    TAllocator::Deallocate(Callable, sizeof(TCallable), alignof(TCallable));
                }
            }

            static void CopyThunk(void* Dest, const void* Source)
            {
                const TCallable& Value = *Target(const_cast<void*>(Source));

                if constexpr (bInline)
                {
                    Memory::ConstructAt(Target(Dest), Value);
                }
                else
                {
                    void* Block = TAllocator::Allocate(sizeof(TCallable), alignof(TCallable));
                    Memory::ConstructAt(static_cast<TCallable*>(Block), Value);
                    static_cast<FStorage*>(Dest)->Heap = Block;
                }
            }

            static void RelocateThunk(void* Dest, void* Source) noexcept
            {
                TCallable* Value = Target(Source);
                Memory::ConstructAt(reinterpret_cast<TCallable*>(static_cast<FStorage*>(Dest)->Inline), std::move(*Value));
                Value->~TCallable();
            }

            NODISCARD static constexpr FVTable MakeVTable() noexcept
            {
                FVTable Table{};
                Table.Invoke = &TOps::InvokeThunk;
                Table.Destroy = &TOps::DestroyThunk;
                Table.bInline = bInline;
                Table.RelocateBytes = bInline ? sizeof(TCallable) : sizeof(void*);

                if constexpr (bCopyable)
                {
                    Table.Copy = &TOps::CopyThunk;
                }

                if constexpr (bNeedsRelocate)
                {
                    Table.Relocate = &TOps::RelocateThunk;
                }

                return Table;
            }

            static constexpr FVTable kVTable = MakeVTable();
        };

    public:

        using result_type = TRet;

        TBasicFunction() noexcept = default;
        TBasicFunction(std::nullptr_t) noexcept {}

        template <typename TCallable>
        requires Private::StorableCallable<TCallable, TBasicFunction, TRet, bCopyable, TArgs...>
        TBasicFunction(TCallable&& Callable)
        {
            using FTarget = std::decay_t<TCallable>;

            // A null function or member pointer has nothing to call, so it produces an empty function.
            if constexpr (std::is_pointer_v<FTarget> || std::is_member_pointer_v<FTarget>)
            {
                if (Callable == nullptr)
                {
                    return;
                }
            }

            EmplaceTarget<FTarget>(std::forward<TCallable>(Callable));
        }

        template <typename TCallable, typename... TCtorArgs>
        requires (std::is_constructible_v<TCallable, TCtorArgs...> &&
                  std::is_invocable_r_v<TRet, TCallable&, TArgs...> &&
                  (!bCopyable || std::is_copy_constructible_v<TCallable>))
        explicit TBasicFunction(TInPlaceType<TCallable>, TCtorArgs&&... Args)
        {
            EmplaceTarget<TCallable>(std::forward<TCtorArgs>(Args)...);
        }

        TBasicFunction(const TBasicFunction& Other)
        requires (bCopyable)
        {
            if (Other.VTable != nullptr)
            {
                Other.VTable->Copy(&Storage, &Other.Storage);
                VTable = Other.VTable;
            }
        }

        TBasicFunction(const TBasicFunction&)
        requires (!bCopyable) = delete;

        TBasicFunction(TBasicFunction&& Other) noexcept
        {
            TakeFrom(Other);
        }

        ~TBasicFunction()
        {
            if (VTable != nullptr)
            {
                VTable->Destroy(&Storage);
            }
        }

        TBasicFunction& operator=(const TBasicFunction& Other)
        requires (bCopyable)
        {
            if (this != &Other)
            {
                TBasicFunction Temp(Other);
                Swap(Temp);
            }

            return *this;
        }

        TBasicFunction& operator=(const TBasicFunction&)
        requires (!bCopyable) = delete;

        TBasicFunction& operator=(TBasicFunction&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                TakeFrom(Other);
            }

            return *this;
        }

        TBasicFunction& operator=(std::nullptr_t) noexcept
        {
            Reset();
            return *this;
        }

        template <typename TCallable>
        requires Private::StorableCallable<TCallable, TBasicFunction, TRet, bCopyable, TArgs...>
        TBasicFunction& operator=(TCallable&& Callable)
        {
            TBasicFunction Temp(std::forward<TCallable>(Callable));
            Swap(Temp);
            return *this;
        }

        TRet operator()(TArgs... Args) const
        {
            LUMINA_CONTAINER_CHECK(VTable != nullptr);
            return VTable->Invoke(const_cast<FStorage*>(&Storage), std::forward<TArgs>(Args)...);
        }

        NODISCARD explicit operator bool() const noexcept { return VTable != nullptr; }
        NODISCARD bool IsSet() const noexcept { return VTable != nullptr; }

        /** True when the target sits in the inline buffer rather than on the heap. */
        NODISCARD bool IsInline() const noexcept { return VTable != nullptr && VTable->bInline; }

        void Reset() noexcept
        {
            if (VTable != nullptr)
            {
                VTable->Destroy(&Storage);
                VTable = nullptr;
            }
        }

        void Swap(TBasicFunction& Other) noexcept
        {
            if (this == &Other)
            {
                return;
            }

            TBasicFunction Temp(std::move(Other));
            Other.TakeFrom(*this);
            TakeFrom(Temp);
        }

        NODISCARD friend bool operator==(const TBasicFunction& Function, std::nullptr_t) noexcept
        {
            return !Function.IsSet();
        }

    private:

        template <typename TCallable, typename... TCtorArgs>
        void EmplaceTarget(TCtorArgs&&... Args)
        {
            using FOps = TOps<TCallable, bFitsInline<TCallable>>;

            if constexpr (bFitsInline<TCallable>)
            {
                Memory::ConstructAt(reinterpret_cast<TCallable*>(Storage.Inline), std::forward<TCtorArgs>(Args)...);
            }
            else
            {
                void* Block = TAllocator::Allocate(sizeof(TCallable), alignof(TCallable));
                Memory::ConstructAt(static_cast<TCallable*>(Block), std::forward<TCtorArgs>(Args)...);
                Storage.Heap = Block;
            }

            VTable = &FOps::kVTable;
        }

        void TakeFrom(TBasicFunction& Other) noexcept
        {
            if (Other.VTable == nullptr)
            {
                return;
            }

            if (Other.VTable->Relocate != nullptr)
            {
                Other.VTable->Relocate(&Storage, &Other.Storage);
            }
            else
            {
                Memory::Memcpy(&Storage, &Other.Storage, Other.VTable->RelocateBytes);
            }

            VTable = Other.VTable;
            Other.VTable = nullptr;
        }

        FStorage       Storage;
        const FVTable* VTable = nullptr;
    };

    template <typename TSignature, size_t InlineCapacity = kFunctionInlineCapacity,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TCopyableFunction = TBasicFunction<TSignature, true, InlineCapacity, TAllocator>;

    template <typename TSignature, size_t InlineCapacity = kFunctionInlineCapacity,
              ContainerAllocatorType TAllocator = FHeapAllocator>
    using TMoveOnlyFunction = TBasicFunction<TSignature, false, InlineCapacity, TAllocator>;

    template <typename TSignature, bool bCopyable, size_t InlineCapacity, ContainerAllocatorType TAllocator>
    FORCEINLINE void swap(TBasicFunction<TSignature, bCopyable, InlineCapacity, TAllocator>& Left,
                          TBasicFunction<TSignature, bCopyable, InlineCapacity, TAllocator>& Right) noexcept
    {
        Left.Swap(Right);
    }
}

namespace Lumina
{
    template <typename TSignature, size_t InlineCapacity = Containers::kFunctionInlineCapacity>
    using TCopyableFunction = Containers::TCopyableFunction<TSignature, InlineCapacity>;

    template <typename TSignature, size_t InlineCapacity = Containers::kFunctionInlineCapacity>
    using TMoveOnlyFunction = Containers::TMoveOnlyFunction<TSignature, InlineCapacity>;

    /** The default engine function object: copyable, so it can live inside a copyable type. */
    template <typename TSignature>
    using TFunction = Containers::TCopyableFunction<TSignature>;

    template <typename T>
    using TInPlaceType = Containers::TInPlaceType<T>;
}
