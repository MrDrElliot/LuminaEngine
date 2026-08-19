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
        using size_type       = size_t;
        using reference       = T&;
        using const_reference = const T&;
        using container_type  = TContainer;

        TQueue() = default;
        explicit TQueue(TContainer InStorage) : Storage(std::move(InStorage)) {}

        NODISCARD FORCEINLINE size_t size() const noexcept { return Storage.size(); }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Storage.empty(); }
        NODISCARD FORCEINLINE size_t Num() const noexcept { return Storage.size(); }

        NODISCARD FORCEINLINE T& front() noexcept { return Storage.front(); }
        NODISCARD FORCEINLINE const T& front() const noexcept { return Storage.front(); }
        NODISCARD FORCEINLINE T& back() noexcept { return Storage.back(); }
        NODISCARD FORCEINLINE const T& back() const noexcept { return Storage.back(); }

        FORCEINLINE void push(const T& Value) { Storage.push_back(Value); }
        FORCEINLINE void push(T&& Value) { Storage.push_back(std::move(Value)); }

        template <typename... TArgs>
        FORCEINLINE T& emplace(TArgs&&... Args) { return Storage.emplace_back(std::forward<TArgs>(Args)...); }

        FORCEINLINE void pop() noexcept { Storage.pop_front(); }
        FORCEINLINE void clear() noexcept { Storage.clear(); }

        FORCEINLINE void swap(TQueue& Other) noexcept { Storage.swap(Other.Storage); }

        NODISCARD FORCEINLINE TContainer& GetContainer() noexcept { return Storage; }
        NODISCARD FORCEINLINE const TContainer& GetContainer() const noexcept { return Storage; }

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
        using size_type       = size_t;
        using reference       = T&;
        using const_reference = const T&;
        using container_type  = TContainer;

        TStack() = default;
        explicit TStack(TContainer InStorage) : Storage(std::move(InStorage)) {}

        NODISCARD FORCEINLINE size_t size() const noexcept { return Storage.size(); }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Storage.empty(); }
        NODISCARD FORCEINLINE size_t Num() const noexcept { return Storage.size(); }

        NODISCARD FORCEINLINE T& top() noexcept { return Storage.back(); }
        NODISCARD FORCEINLINE const T& top() const noexcept { return Storage.back(); }

        FORCEINLINE void push(const T& Value) { Storage.push_back(Value); }
        FORCEINLINE void push(T&& Value) { Storage.push_back(std::move(Value)); }

        template <typename... TArgs>
        FORCEINLINE T& emplace(TArgs&&... Args) { return Storage.emplace_back(std::forward<TArgs>(Args)...); }

        FORCEINLINE void pop() noexcept { Storage.pop_back(); }
        FORCEINLINE void clear() noexcept { Storage.clear(); }

        FORCEINLINE void swap(TStack& Other) noexcept { Storage.swap(Other.Storage); }

        NODISCARD FORCEINLINE TContainer& GetContainer() noexcept { return Storage; }
        NODISCARD FORCEINLINE const TContainer& GetContainer() const noexcept { return Storage; }

        NODISCARD friend bool operator==(const TStack& Left, const TStack& Right)
        {
            return Left.Storage == Right.Storage;
        }

    private:

        TContainer Storage;
    };

    template <typename T, typename TContainer>
    FORCEINLINE void swap(TQueue<T, TContainer>& Left, TQueue<T, TContainer>& Right) noexcept { Left.swap(Right); }

    template <typename T, typename TContainer>
    FORCEINLINE void swap(TStack<T, TContainer>& Left, TStack<T, TContainer>& Right) noexcept { Left.swap(Right); }
}

namespace Lumina
{
    template <typename T>
    using TQueue = Containers::TQueue<T>;

    template <typename T, typename C = Containers::TDeque<T>>
    using TStack = Containers::TStack<T, C>;
}
