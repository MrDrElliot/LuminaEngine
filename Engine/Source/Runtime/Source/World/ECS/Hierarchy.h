#pragma once

#include "Entity.h"
#include "Containers/Vector.h"

namespace Lumina::ECS
{
    // One entity's place in the tree. Siblings form a doubly linked list the parent heads with First and Last.
    struct FHierarchyNode
    {
        FEntity Self;
        FEntity Parent;
        FEntity First;
        FEntity Last;
        FEntity Prev;
        FEntity Next;
        uint32  ChildCount = 0;

        NODISCARD FORCEINLINE bool IsLinked() const { return !Parent.IsNull() || !First.IsNull(); }
    };

    // The registry's parent links, plus a lazily rebuilt preorder in which every subtree is one contiguous range.
    class FHierarchy
    {
    public:

        static constexpr uint32 NoSlot = Constants::kIndexNoneU32;

        // A child position past the last sibling, which is where a new child goes by default.
        static constexpr uint32 AppendPosition = Constants::kIndexNoneU32;

        // Null when the entity has neither a parent nor children.
        NODISCARD FORCEINLINE const FHierarchyNode* Find(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (Index >= Nodes.size())
            {
                return nullptr;
            }
            const FHierarchyNode& Node = Nodes[Index];
            return Node.Self == Entity && Node.IsLinked() ? &Node : nullptr;
        }

        NODISCARD FORCEINLINE bool IsLinked(FEntity Entity) const { return Find(Entity) != nullptr; }

        NODISCARD FORCEINLINE FEntity GetParent(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->Parent : FEntity();
        }

        NODISCARD FORCEINLINE uint32 GetChildCount(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->ChildCount : 0u;
        }

        NODISCARD FORCEINLINE FEntity GetFirstChild(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->First : FEntity();
        }

        NODISCARD FORCEINLINE FEntity GetLastChild(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->Last : FEntity();
        }

        NODISCARD FORCEINLINE FEntity GetNextSibling(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->Next : FEntity();
        }

        NODISCARD FORCEINLINE FEntity GetPrevSibling(FEntity Entity) const
        {
            const FHierarchyNode* Node = Find(Entity);
            return Node != nullptr ? Node->Prev : FEntity();
        }

        // Where the entity sits among its parent's children, counted from the first. Walks the earlier siblings.
        NODISCARD RUNTIME_API uint32 GetSiblingIndex(FEntity Entity) const;

        // Every child's sibling index by entity index in one pass, for a caller about to ask for all of them.
        RUNTIME_API void BuildSiblingIndices(TVector<uint32>& OutByEntityIndex) const;

        // Reads the next sibling before calling Func, so Func may detach the child it was handed.
        template<typename TFunc>
        void ForEachChild(FEntity Parent, TFunc&& Func) const
        {
            FEntity Child = GetFirstChild(Parent);
            while (!Child.IsNull())
            {
                const FEntity Next = GetNextSibling(Child);
                Func(Child);
                Child = Next;
            }
        }

        // Parents before children and siblings in order, without building the preorder.
        template<typename TFunc>
        void ForEachDescendant(FEntity Root, TFunc&& Func) const
        {
            TVector<FEntity> Pending;
            for (FEntity Child = GetLastChild(Root); !Child.IsNull(); Child = GetPrevSibling(Child))
            {
                Pending.push_back(Child);
            }
            while (!Pending.empty())
            {
                const FEntity Current = Pending.back();
                Pending.pop_back();
                Func(Current);
                for (FEntity Child = GetLastChild(Current); !Child.IsNull(); Child = GetPrevSibling(Child))
                {
                    Pending.push_back(Child);
                }
            }
        }

        NODISCARD RUNTIME_API bool IsDescendantOf(FEntity Entity, FEntity Ancestor) const;
        NODISCARD RUNTIME_API FEntity GetRoot(FEntity Entity) const;

        NODISCARD FORCEINLINE uint32 NumLinked() const { return LinkedCount; }

        //~ Preorder. Valid until the next link edit; EnsureOrder is not safe against concurrent edits.

        RUNTIME_API void EnsureOrder() const;

        NODISCARD FORCEINLINE uint32 GetSlot(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (Index >= SlotByIndex.size())
            {
                return NoSlot;
            }
            const uint32 Slot = SlotByIndex[Index];
            return Slot != NoSlot && Order[Slot] == Entity ? Slot : NoSlot;
        }

        // Skips the ownership check, which costs a miss. Only for a live entity while the order is current.
        NODISCARD FORCEINLINE uint32 GetSlotOfLive(FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            return Index < SlotByIndex.size() ? SlotByIndex[Index] : NoSlot;
        }

        NODISCARD FORCEINLINE const TVector<FEntity>& GetOrder() const { return Order; }
        NODISCARD FORCEINLINE const TVector<uint32>& GetSubtreeSizes() const { return SubtreeSize; }
        NODISCARD FORCEINLINE const TVector<uint32>& GetParentSlots() const { return ParentSlot; }

    private:

        friend class FRegistry;

        FHierarchyNode& Touch(FEntity Entity);
        FHierarchyNode* Lookup(FEntity Entity);

        // The caller has checked Child is detached and Parent is not inside Child's subtree.
        void Link(FEntity Child, FEntity Parent, uint32 Position);
        void Unlink(FEntity Child);
        void Forget(FEntity Entity);
        void Reset();

        void SetLinked(FHierarchyNode& Node, bool bWasLinked);

        TVector<FHierarchyNode> Nodes;
        uint32                  LinkedCount = 0;

        mutable TVector<FEntity> Order;
        mutable TVector<uint32>  SubtreeSize;
        mutable TVector<uint32>  ParentSlot;
        mutable TVector<uint32>  SlotByIndex;
        mutable bool             bOrderDirty = false;
    };
}
