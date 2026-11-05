#pragma once

#include <utility>

#include "ContainerTraits.h"
#include "Deque.h"

namespace Lumina::Containers
{
    /** First in, first out. An adapter, so the storage container is the caller's choice. */
    template <typename T, typename TContainer = TDeque<T>>
    class TQueue
    {
    public:

        using value_type      = T;
        using size_type       = int32;
        using reference       = T&;
        using const_reference = const T&;
        using container_type  = TContainer;

        TQueue() = default;
        explicit TQueue(TContainer InStorage) : Storage(std::move(InStorage)) {}

        NODISCARD FORCEINLINE int32 Num() const noexcept { return Storage.Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Storage.IsEmpty(); }

        /** The oldest entry, the one Pop removes next. */
        template <typename TSelf>
        NODISCARD FORCEINLINE auto Peek(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self.Storage.First(); }

        FORCEINLINE void Enqueue(const T& Value) { Storage.PushLast(Value); }
        FORCEINLINE void Enqueue(T&& Value) { Storage.PushLast(std::move(Value)); }

        template <typename... TArgs>
        FORCEINLINE T& Emplace(TArgs&&... Args) { return Storage.EmplaceLast(std::forward<TArgs>(Args)...); }

        FORCEINLINE void Pop() noexcept { Storage.PopFirst(); }

        bool Dequeue(T& OutValue)
        {
            if (Storage.IsEmpty())
            {
                return false;
            }
            OutValue = std::move(Storage.First());
            Storage.PopFirst();
            return true;
        }

        FORCEINLINE void Reset() noexcept { Storage.Reset(); }

        friend void swap(TQueue& Left, TQueue& Right) noexcept
        {
            using std::swap;
            swap(Left.Storage, Right.Storage);
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto GetContainer(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, TContainer>& { return Self.Storage; }

        NODISCARD friend bool operator==(const TQueue& Left, const TQueue& Right)
        {
            return Left.Storage == Right.Storage;
        }

    private:

        TContainer Storage;
    };

    /** Last in, first out, over the same storage the queue uses. */
    template <typename T, typename TContainer = TDeque<T>>
    class TStack
    {
    public:

        using value_type      = T;
        using size_type       = int32;
        using reference       = T&;
        using const_reference = const T&;
        using container_type  = TContainer;

        TStack() = default;
        explicit TStack(TContainer InStorage) : Storage(std::move(InStorage)) {}

        NODISCARD FORCEINLINE int32 Num() const noexcept { return Storage.Num(); }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Storage.IsEmpty(); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Top(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>& { return Self.Storage.Last(); }

        FORCEINLINE void Push(const T& Value) { Storage.PushLast(Value); }
        FORCEINLINE void Push(T&& Value) { Storage.PushLast(std::move(Value)); }

        template <typename... TArgs>
        FORCEINLINE T& Emplace(TArgs&&... Args) { return Storage.EmplaceLast(std::forward<TArgs>(Args)...); }

        FORCEINLINE void Pop() noexcept { Storage.PopLast(); }
        FORCEINLINE void Reset() noexcept { Storage.Reset(); }

        friend void swap(TStack& Left, TStack& Right) noexcept
        {
            using std::swap;
            swap(Left.Storage, Right.Storage);
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto GetContainer(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, TContainer>& { return Self.Storage; }

        NODISCARD friend bool operator==(const TStack& Left, const TStack& Right)
        {
            return Left.Storage == Right.Storage;
        }

    private:

        TContainer Storage;
    };

}

namespace Lumina
{
    template <typename T>
    using TQueue = Containers::TQueue<T>;

    template <typename T, typename C = Containers::TDeque<T>>
    using TStack = Containers::TStack<T, C>;
}
