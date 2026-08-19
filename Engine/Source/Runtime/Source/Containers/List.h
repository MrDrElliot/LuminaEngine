#pragma once

#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerAllocator.h"
#include "ContainerTraits.h"

namespace Lumina::Containers
{
    /** Doubly linked list. Every element address is stable for its whole life, which is the reason to pick it. */
    template <typename T, ContainerAllocatorType TAllocator = FHeapAllocator>
    class TList
    {
        struct FNodeBase
        {
            FNodeBase* Next = nullptr;
            FNodeBase* Prev = nullptr;
        };

        struct FNode : FNodeBase
        {
            T Value;

            template <typename... TArgs>
            explicit FNode(TArgs&&... Args) : Value(std::forward<TArgs>(Args)...) {}
        };

    public:

        using value_type      = T;
        using size_type       = size_t;
        using reference       = T&;
        using const_reference = const T&;
        using pointer         = T*;
        using const_pointer   = const T*;

        template <bool bConst>
        class TIterator
        {
            friend class TList;
            template <bool> friend class TIterator;

        public:

            using iterator_category = std::bidirectional_iterator_tag;
            using value_type        = T;
            using difference_type   = ptrdiff_t;
            using reference         = std::conditional_t<bConst, const T&, T&>;
            using pointer           = std::conditional_t<bConst, const T*, T*>;

            TIterator() noexcept = default;

            template <bool bOther>
            requires (bConst && !bOther)
            TIterator(const TIterator<bOther>& Other) noexcept : Node(Other.Node) {}

            NODISCARD reference operator*() const noexcept { return static_cast<FNode*>(Node)->Value; }
            NODISCARD pointer operator->() const noexcept { return &static_cast<FNode*>(Node)->Value; }

            TIterator& operator++() noexcept { Node = Node->Next; return *this; }
            TIterator operator++(int) noexcept { TIterator Copy = *this; Node = Node->Next; return Copy; }
            TIterator& operator--() noexcept { Node = Node->Prev; return *this; }
            TIterator operator--(int) noexcept { TIterator Copy = *this; Node = Node->Prev; return Copy; }

            NODISCARD friend bool operator==(const TIterator& Left, const TIterator& Right) noexcept
            {
                return Left.Node == Right.Node;
            }

        private:

            explicit TIterator(FNodeBase* InNode) noexcept : Node(InNode) {}

            FNodeBase* Node = nullptr;
        };

        using iterator       = TIterator<false>;
        using const_iterator = TIterator<true>;

        TList() noexcept { Sentinel.Next = &Sentinel; Sentinel.Prev = &Sentinel; }

        TList(const TList& Other) : TList()
        {
            for (const T& Element : Other)
            {
                push_back(Element);
            }
        }

        TList(TList&& Other) noexcept : TList()
        {
            Adopt(Other);
        }

        ~TList() { clear(); }

        TList& operator=(const TList& Other)
        {
            if (this != &Other)
            {
                clear();
                for (const T& Element : Other)
                {
                    push_back(Element);
                }
            }
            return *this;
        }

        TList& operator=(TList&& Other) noexcept
        {
            if (this != &Other)
            {
                clear();
                Adopt(Other);
            }
            return *this;
        }

        NODISCARD FORCEINLINE size_t size() const noexcept { return Count; }
        NODISCARD FORCEINLINE bool empty() const noexcept { return Count == 0; }
        NODISCARD FORCEINLINE size_t Num() const noexcept { return Count; }

        NODISCARD iterator begin() noexcept { return iterator(Sentinel.Next); }
        NODISCARD const_iterator begin() const noexcept { return const_iterator(const_cast<FNodeBase*>(Sentinel.Next)); }
        NODISCARD iterator end() noexcept { return iterator(&Sentinel); }
        NODISCARD const_iterator end() const noexcept { return const_iterator(const_cast<FNodeBase*>(&Sentinel)); }
        NODISCARD const_iterator cbegin() const noexcept { return begin(); }
        NODISCARD const_iterator cend() const noexcept { return end(); }

        NODISCARD FORCEINLINE T& front() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return static_cast<FNode*>(Sentinel.Next)->Value;
        }

        NODISCARD FORCEINLINE const T& front() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return static_cast<const FNode*>(Sentinel.Next)->Value;
        }

        NODISCARD FORCEINLINE T& back() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return static_cast<FNode*>(Sentinel.Prev)->Value;
        }

        NODISCARD FORCEINLINE const T& back() const noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            return static_cast<const FNode*>(Sentinel.Prev)->Value;
        }

        template <typename... TArgs>
        T& emplace_back(TArgs&&... Args)
        {
            return *InsertBefore(&Sentinel, std::forward<TArgs>(Args)...);
        }

        template <typename... TArgs>
        T& emplace_front(TArgs&&... Args)
        {
            return *InsertBefore(Sentinel.Next, std::forward<TArgs>(Args)...);
        }

        FORCEINLINE T& push_back(const T& Value) { return emplace_back(Value); }
        FORCEINLINE T& push_back(T&& Value) { return emplace_back(std::move(Value)); }
        FORCEINLINE T& push_front(const T& Value) { return emplace_front(Value); }
        FORCEINLINE T& push_front(T&& Value) { return emplace_front(std::move(Value)); }

        template <typename... TArgs>
        iterator emplace(const_iterator Position, TArgs&&... Args)
        {
            T* Value = InsertBefore(Position.Node, std::forward<TArgs>(Args)...);
            return iterator(NodeOf(Value));
        }

        FORCEINLINE iterator insert(const_iterator Position, const T& Value) { return emplace(Position, Value); }
        FORCEINLINE iterator insert(const_iterator Position, T&& Value) { return emplace(Position, std::move(Value)); }

        void pop_back() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Unlink(Sentinel.Prev);
        }

        void pop_front() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Unlink(Sentinel.Next);
        }

        iterator erase(const_iterator Position) noexcept
        {
            LUMINA_CONTAINER_CHECK(Position.Node != &Sentinel);
            FNodeBase* Next = Position.Node->Next;
            Unlink(Position.Node);
            return iterator(Next);
        }

        void clear() noexcept
        {
            FNodeBase* Node = Sentinel.Next;
            while (Node != &Sentinel)
            {
                FNodeBase* Next = Node->Next;
                Destroy(static_cast<FNode*>(Node));
                Node = Next;
            }
            Sentinel.Next = &Sentinel;
            Sentinel.Prev = &Sentinel;
            Count = 0;
        }

        void swap(TList& Other) noexcept
        {
            TList Staged(std::move(*this));
            *this = std::move(Other);
            Other = std::move(Staged);
        }

        NODISCARD friend bool operator==(const TList& Left, const TList& Right)
        {
            if (Left.Count != Right.Count)
            {
                return false;
            }
            const_iterator A = Left.begin();
            const_iterator B = Right.begin();
            for (; A != Left.end(); ++A, ++B)
            {
                if (!(*A == *B))
                {
                    return false;
                }
            }
            return true;
        }

    private:

        NODISCARD static FNodeBase* NodeOf(T* Value) noexcept
        {
            return static_cast<FNodeBase*>(reinterpret_cast<FNode*>(reinterpret_cast<uint8*>(Value) - offsetof(FNode, Value)));
        }

        template <typename... TArgs>
        T* InsertBefore(FNodeBase* Position, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(FNode), alignof(FNode));
            FNode* Node = ::new (Block) FNode(std::forward<TArgs>(Args)...);

            Node->Prev = Position->Prev;
            Node->Next = Position;
            Position->Prev->Next = Node;
            Position->Prev = Node;
            ++Count;
            return &Node->Value;
        }

        void Unlink(FNodeBase* Node) noexcept
        {
            Node->Prev->Next = Node->Next;
            Node->Next->Prev = Node->Prev;
            Destroy(static_cast<FNode*>(Node));
            --Count;
        }

        static void Destroy(FNode* Node) noexcept
        {
            Node->~FNode();
            TAllocator::Deallocate(Node, sizeof(FNode), alignof(FNode));
        }

        void Adopt(TList& Other) noexcept
        {
            if (Other.Count == 0)
            {
                return;
            }
            Sentinel.Next = Other.Sentinel.Next;
            Sentinel.Prev = Other.Sentinel.Prev;
            Sentinel.Next->Prev = &Sentinel;
            Sentinel.Prev->Next = &Sentinel;
            Count = Other.Count;

            Other.Sentinel.Next = &Other.Sentinel;
            Other.Sentinel.Prev = &Other.Sentinel;
            Other.Count = 0;
        }

        FNodeBase Sentinel;
        size_t    Count = 0;
    };

    template <typename T, ContainerAllocatorType TAllocator>
    FORCEINLINE void swap(TList<T, TAllocator>& Left, TList<T, TAllocator>& Right) noexcept
    {
        Left.swap(Right);
    }
}

namespace Lumina
{
    template <typename T>
    using TList = Containers::TList<T>;
}
