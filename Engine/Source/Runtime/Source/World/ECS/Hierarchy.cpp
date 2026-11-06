#include "RuntimePCH.h"
#include "Hierarchy.h"

namespace Lumina::ECS
{
    uint32 FHierarchy::GetSiblingIndex(FEntity Entity) const
    {
        uint32 Index = 0;
        for (FEntity Prev = GetPrevSibling(Entity); !Prev.IsNull(); Prev = GetPrevSibling(Prev))
        {
            ++Index;
        }
        return Index;
    }

    void FHierarchy::BuildSiblingIndices(TVector<uint32>& OutByEntityIndex) const
    {
        OutByEntityIndex.Init(0u, Nodes.Num());
        for (const FHierarchyNode& Node : Nodes)
        {
            uint32 Index = 0;
            for (FEntity Child = Node.Self.IsNull() ? FEntity() : Node.First; !Child.IsNull(); Child = Nodes[Child.GetIndex()].Next)
            {
                OutByEntityIndex[Child.GetIndex()] = Index++;
            }
        }
    }

    bool FHierarchy::IsDescendantOf(FEntity Entity, FEntity Ancestor) const
    {
        if (Entity.IsNull() || Ancestor.IsNull())
        {
            return false;
        }

        for (FEntity Current = GetParent(Entity); !Current.IsNull(); Current = GetParent(Current))
        {
            if (Current == Ancestor)
            {
                return true;
            }
        }
        return false;
    }

    FEntity FHierarchy::GetRoot(FEntity Entity) const
    {
        FEntity Current = Entity;
        for (FEntity Parent = GetParent(Current); !Parent.IsNull(); Parent = GetParent(Current))
        {
            Current = Parent;
        }
        return Current;
    }

    FHierarchyNode& FHierarchy::Touch(FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        if (Index >= static_cast<uint32>(Nodes.Num()))
        {
            Nodes.SetNum(Index + 1u);
        }

        FHierarchyNode& Node = Nodes[Index];
        if (Node.Self != Entity)
        {
            Node = FHierarchyNode();
            Node.Self = Entity;
        }
        return Node;
    }

    FHierarchyNode* FHierarchy::Lookup(FEntity Entity)
    {
        const uint32 Index = Entity.GetIndex();
        return !Entity.IsNull() && Index < static_cast<uint32>(Nodes.Num()) && Nodes[Index].Self == Entity ? &Nodes[Index] : nullptr;
    }

    void FHierarchy::SetLinked(FHierarchyNode& Node, bool bWasLinked)
    {
        const bool bIsLinked = Node.IsLinked();
        if (bIsLinked != bWasLinked)
        {
            LinkedCount = bIsLinked ? LinkedCount + 1u : LinkedCount - 1u;
        }
        bOrderDirty = true;
    }

    void FHierarchy::Link(FEntity Child, FEntity Parent, uint32 Position)
    {
        // Both touched before either reference is taken, since a touch can grow the array.
        Touch(Parent);
        Touch(Child);
        FHierarchyNode& ParentNode = *Lookup(Parent);
        FHierarchyNode& ChildNode  = *Lookup(Child);

        const bool bParentWasLinked = ParentNode.IsLinked();
        const bool bChildWasLinked  = ChildNode.IsLinked();

        // Appending is the common case and stays constant time however many children the parent has.
        FEntity Next = Position >= ParentNode.ChildCount ? FEntity() : ParentNode.First;
        for (uint32 Skipped = 0; Skipped < Position && !Next.IsNull(); ++Skipped)
        {
            Next = Lookup(Next)->Next;
        }
        const FEntity Prev = Next.IsNull() ? ParentNode.Last : Lookup(Next)->Prev;

        ChildNode.Parent = Parent;
        ChildNode.Prev   = Prev;
        ChildNode.Next   = Next;

        (Prev.IsNull() ? ParentNode.First : Lookup(Prev)->Next) = Child;
        (Next.IsNull() ? ParentNode.Last  : Lookup(Next)->Prev) = Child;
        ++ParentNode.ChildCount;

        SetLinked(ParentNode, bParentWasLinked);
        SetLinked(ChildNode, bChildWasLinked);
    }

    void FHierarchy::Unlink(FEntity Child)
    {
        FHierarchyNode* ChildNode = Lookup(Child);
        if (ChildNode == nullptr || ChildNode->Parent.IsNull())
        {
            return;
        }

        FHierarchyNode& ParentNode = *Lookup(ChildNode->Parent);
        const bool bParentWasLinked = ParentNode.IsLinked();

        (ChildNode->Prev.IsNull() ? ParentNode.First : Lookup(ChildNode->Prev)->Next) = ChildNode->Next;
        (ChildNode->Next.IsNull() ? ParentNode.Last  : Lookup(ChildNode->Next)->Prev) = ChildNode->Prev;
        --ParentNode.ChildCount;

        ChildNode->Parent = FEntity();
        ChildNode->Prev   = FEntity();
        ChildNode->Next   = FEntity();

        SetLinked(ParentNode, bParentWasLinked);
        SetLinked(*ChildNode, true);
    }

    void FHierarchy::Forget(FEntity Entity)
    {
        if (FHierarchyNode* Node = Lookup(Entity))
        {
            const bool bWasLinked = Node->IsLinked();
            *Node = FHierarchyNode();
            SetLinked(*Node, bWasLinked);
        }
    }

    void FHierarchy::Reset()
    {
        Nodes.Reset();
        LinkedCount = 0;
        Order.Reset();
        SubtreeSize.Reset();
        ParentSlot.Reset();
        SlotByIndex.Reset();
        bOrderDirty = false;
    }

    void FHierarchy::EnsureOrder() const
    {
        if (!bOrderDirty)
        {
            return;
        }
        LUMINA_PROFILE_SCOPE();
        bOrderDirty = false;

        Order.Reset();
        ParentSlot.Reset();
        SlotByIndex.Init(NoSlot, Nodes.Num());

        struct FPending
        {
            FEntity Entity;
            uint32  ParentSlot;
        };
        TVector<FPending> Pending;

        for (const FHierarchyNode& Root : Nodes)
        {
            if (Root.Self.IsNull() || !Root.Parent.IsNull() || Root.First.IsNull())
            {
                continue;
            }

            Pending.Emplace(Root.Self, NoSlot);
            while (!Pending.IsEmpty())
            {
                const FPending Current = Pending.Last();
                Pending.Pop();

                const uint32 Slot = (uint32)Order.Num();
                SlotByIndex[Current.Entity.GetIndex()] = Slot;
                Order.Add(Current.Entity);
                ParentSlot.Add(Current.ParentSlot);

                // Pushed last to first, so the first child is popped next and siblings keep their order.
                for (FEntity Child = Nodes[Current.Entity.GetIndex()].Last; !Child.IsNull(); Child = Nodes[Child.GetIndex()].Prev)
                {
                    Pending.Emplace(Child, Slot);
                }
            }
        }

        SubtreeSize.Init(1u, Order.Num());
        for (int32 Slot = Order.Num(); Slot-- > 0;)
        {
            if (ParentSlot[Slot] != NoSlot)
            {
                SubtreeSize[ParentSlot[Slot]] += SubtreeSize[Slot];
            }
        }
    }
}
