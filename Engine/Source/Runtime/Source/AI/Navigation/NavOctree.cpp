#include "RuntimePCH.h"
#include "NavOctree.h"

namespace Lumina
{
    FNavOctree::FNavOctree(float InRootHalfSize, float InMinHalfSize)
        : RootHalfSize(InRootHalfSize)
        , MinHalfSize(InMinHalfSize)
    {
        Clear();
    }

    void FNavOctree::Clear()
    {
        Nodes.Reset();
        Elements.Reset();
        FreeElements.Reset();
        Count = 0;

        FNode Root;
        Root.HalfSize = RootHalfSize;
        Nodes.Add(Move(Root));
    }

    uint32 FNavOctree::NodeFor(const FVector3& Min, const FVector3& Max)
    {
        const FVector3 Center = (Min + Max) * 0.5f;
        const FVector3 HalfExtent = (Max - Min) * 0.5f;
        const float Size = Math::Max(HalfExtent.x, Math::Max(HalfExtent.y, HalfExtent.z));

        uint32 Node = 0;
        for (;;)
        {
            const float ChildHalf = Nodes[Node].HalfSize * 0.5f;
            const FVector3 NodeCenter = Nodes[Node].Center;
            const FVector3 Offset = Center - NodeCenter;

            // Past the root's own box nothing deeper can hold it, so it stays at the root.
            const bool bInside = Math::Abs(Offset.x) <= Nodes[Node].HalfSize && Math::Abs(Offset.y) <= Nodes[Node].HalfSize && Math::Abs(Offset.z) <= Nodes[Node].HalfSize;
            if (ChildHalf < MinHalfSize || Size > ChildHalf || !bInside)
            {
                return Node;
            }

            if (Nodes[Node].FirstChild == kNone)
            {
                const uint32 First = (uint32)Nodes.Num();
                Nodes[Node].FirstChild = First;
                for (uint32 Octant = 0; Octant < 8; ++Octant)
                {
                    FNode Child;
                    Child.HalfSize = ChildHalf;
                    Child.Center = NodeCenter + FVector3((Octant & 1) ? ChildHalf : -ChildHalf, (Octant & 2) ? ChildHalf : -ChildHalf, (Octant & 4) ? ChildHalf : -ChildHalf);
                    Nodes.Add(Move(Child));
                }
            }

            const uint32 Octant = (Offset.x >= 0.0f ? 1u : 0u) | (Offset.y >= 0.0f ? 2u : 0u) | (Offset.z >= 0.0f ? 4u : 0u);
            Node = Nodes[Node].FirstChild + Octant;
        }
    }

    void FNavOctree::Link(uint32 Element, uint32 Node)
    {
        FElement& E = Elements[Element];
        E.Node = Node;
        E.Slot = (uint32)Nodes[Node].Elements.Num();
        Nodes[Node].Elements.Add(Element);
    }

    void FNavOctree::Unlink(uint32 Element)
    {
        FElement& E = Elements[Element];
        TVector<uint32>& Slots = Nodes[E.Node].Elements;
        const uint32 Moved = Slots.Last();
        Slots[E.Slot] = Moved;
        Elements[Moved].Slot = E.Slot;
        Slots.Pop();
        E.Node = kNone;
    }

    uint32 FNavOctree::Add(const FVector3& Min, const FVector3& Max, uint32 Payload)
    {
        uint32 Element;
        if (!FreeElements.IsEmpty())
        {
            Element = FreeElements.Last();
            FreeElements.Pop();
        }
        else
        {
            Element = (uint32)Elements.Num();
            Elements.Emplace();
        }

        FElement& E = Elements[Element];
        E.Min = Min;
        E.Max = Max;
        E.Payload = Payload;
        Link(Element, NodeFor(Min, Max));
        ++Count;
        return Element;
    }

    void FNavOctree::Update(uint32 Element, const FVector3& Min, const FVector3& Max)
    {
        const uint32 Node = NodeFor(Min, Max);
        Elements[Element].Min = Min;
        Elements[Element].Max = Max;
        if (Node != Elements[Element].Node)
        {
            Unlink(Element);
            Link(Element, Node);
        }
    }

    void FNavOctree::Remove(uint32 Element)
    {
        if (Element >= static_cast<uint32>(Elements.Num()) || Elements[Element].Node == kNone)
        {
            return;
        }
        Unlink(Element);
        FreeElements.Add(Element);
        --Count;
    }
}
