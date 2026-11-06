#pragma once

#include <iterator>
#include "Memory/Construct.h"
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
        using size_type       = int32;
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
                AddTail(Element);
            }
        }

        TList(TList&& Other) noexcept : TList()
        {
            Adopt(Other);
        }

        ~TList() { Empty(); }

        TList& operator=(const TList& Other)
        {
            if (this != &Other)
            {
                Empty();
                for (const T& Element : Other)
                {
                    AddTail(Element);
                }
            }
            return *this;
        }

        TList& operator=(TList&& Other) noexcept
        {
            if (this != &Other)
            {
                Empty();
                Adopt(Other);
            }
            return *this;
        }

        NODISCARD FORCEINLINE int32 Num() const noexcept { return Count; }
        NODISCARD FORCEINLINE bool IsEmpty() const noexcept { return Count == 0; }

        template <typename TSelf>
        NODISCARD auto begin(this TSelf&& Self) noexcept
        {
            return std::conditional_t<std::is_const_v<std::remove_reference_t<TSelf>>, const_iterator, iterator>(Self.Sentinel.Next);
        }

        template <typename TSelf>
        NODISCARD auto end(this TSelf&& Self) noexcept
        {
            return std::conditional_t<std::is_const_v<std::remove_reference_t<TSelf>>, const_iterator, iterator>(const_cast<FNodeBase*>(&Self.Sentinel));
        }
        NODISCARD const_iterator cbegin() const noexcept { return begin(); }
        NODISCARD const_iterator cend() const noexcept { return end(); }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Head(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>&
        {
            LUMINA_CONTAINER_CHECK(Self.Count != 0);
            return static_cast<TLikeSelf<TSelf, FNode>*>(Self.Sentinel.Next)->Value;
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE auto Tail(this TSelf&& Self) noexcept -> TLikeSelf<TSelf, T>&
        {
            LUMINA_CONTAINER_CHECK(Self.Count != 0);
            return static_cast<TLikeSelf<TSelf, FNode>*>(Self.Sentinel.Prev)->Value;
        }

        template <typename... TArgs>
        T& EmplaceTail(TArgs&&... Args)
        {
            return InsertBefore(&Sentinel, std::forward<TArgs>(Args)...)->Value;
        }

        template <typename... TArgs>
        T& EmplaceHead(TArgs&&... Args)
        {
            return InsertBefore(Sentinel.Next, std::forward<TArgs>(Args)...)->Value;
        }

        FORCEINLINE T& AddTail(const T& Value) { return EmplaceTail(Value); }
        FORCEINLINE T& AddTail(T&& Value) { return EmplaceTail(std::move(Value)); }
        FORCEINLINE T& AddHead(const T& Value) { return EmplaceHead(Value); }
        FORCEINLINE T& AddHead(T&& Value) { return EmplaceHead(std::move(Value)); }

        template <typename... TArgs>
        iterator Emplace(const_iterator Position, TArgs&&... Args)
        {
            return iterator(InsertBefore(Position.Node, std::forward<TArgs>(Args)...));
        }

        FORCEINLINE iterator Insert(const_iterator Position, const T& Value) { return Emplace(Position, Value); }
        FORCEINLINE iterator Insert(const_iterator Position, T&& Value) { return Emplace(Position, std::move(Value)); }

        void RemoveTail() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Unlink(Sentinel.Prev);
        }

        void RemoveHead() noexcept
        {
            LUMINA_CONTAINER_CHECK(Count != 0);
            Unlink(Sentinel.Next);
        }

        iterator Remove(const_iterator Position) noexcept
        {
            LUMINA_CONTAINER_CHECK(Position.Node != &Sentinel);
            FNodeBase* Next = Position.Node->Next;
            Unlink(Position.Node);
            return iterator(Next);
        }

        void Empty() noexcept
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

        friend void swap(TList& Left, TList& Right) noexcept
        {
            TList Staged(std::move(Left));
            Left = std::move(Right);
            Right = std::move(Staged);
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

        template <typename... TArgs>
        FNode* InsertBefore(FNodeBase* Position, TArgs&&... Args)
        {
            void* Block = TAllocator::Allocate(sizeof(FNode), alignof(FNode));
            FNode* Node = Memory::ConstructAt(static_cast<FNode*>(Block), std::forward<TArgs>(Args)...);

            Node->Prev = Position->Prev;
            Node->Next = Position;
            Position->Prev->Next = Node;
            Position->Prev = Node;
            ++Count;
            return Node;
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
        int32     Count = 0;
    };

}

namespace Lumina
{
    template <typename T>
    using TList = Containers::TList<T>;
}
