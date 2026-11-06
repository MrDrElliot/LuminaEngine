#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    // A loose octree of boxes, where an element lives in the deepest node twice its size, so a move is a relink and nothing splits.
    class RUNTIME_API FNavOctree
    {
    public:

        static constexpr uint32 kNone = UINT32_MAX;

        explicit FNavOctree(float InRootHalfSize = 1048576.0f, float InMinHalfSize = 8.0f);

        // Payload is the caller's own index, which queries hand back.
        uint32 Add(const FVector3& Min, const FVector3& Max, uint32 Payload);
        void   Update(uint32 Element, const FVector3& Min, const FVector3& Max);
        void   Remove(uint32 Element);
        void   Clear();

        NODISCARD uint32 Num() const { return Count; }
        NODISCARD uint32 NumNodes() const { return (uint32)Nodes.Num(); }

        // Visit(Payload, Min, Max) for every element whose box touches the query box.
        template<typename FnVisit>
        void Query(const FVector3& Min, const FVector3& Max, FnVisit&& Visit) const
        {
            // Seven siblings wait per level at most, so the deepest tree fits with room to spare.
            uint32 Stack[256];
            int32 Top = 0;
            Stack[Top++] = 0;
            while (Top > 0)
            {
                const FNode& Node = Nodes[Stack[--Top]];
                for (uint32 Element : Node.Elements)
                {
                    const FElement& E = Elements[Element];
                    if (E.Min.x <= Max.x && E.Max.x >= Min.x && E.Min.y <= Max.y && E.Max.y >= Min.y && E.Min.z <= Max.z && E.Max.z >= Min.z)
                    {
                        Visit(E.Payload, E.Min, E.Max);
                    }
                }
                if (Node.FirstChild == kNone)
                {
                    continue;
                }

                // A child holds anything centered inside it and no bigger than it, so its reach is twice its half size.
                for (uint32 Child = Node.FirstChild; Child < Node.FirstChild + 8; ++Child)
                {
                    const FNode& C = Nodes[Child];
                    const float Reach = C.HalfSize * 2.0f;
                    const bool bOccupied = !C.Elements.IsEmpty() || C.FirstChild != kNone;
                    if (bOccupied &&
                        C.Center.x - Reach <= Max.x && C.Center.x + Reach >= Min.x &&
                        C.Center.y - Reach <= Max.y && C.Center.y + Reach >= Min.y &&
                        C.Center.z - Reach <= Max.z && C.Center.z + Reach >= Min.z)
                    {
                        Stack[Top++] = Child;
                    }
                }
            }
        }

    private:

        struct FNode
        {
            FVector3        Center = FVector3(0.0f);
            float           HalfSize = 0.0f;
            uint32          FirstChild = kNone;
            TVector<uint32> Elements;
        };

        struct FElement
        {
            FVector3 Min = FVector3(0.0f);
            FVector3 Max = FVector3(0.0f);
            uint32   Payload = 0;
            uint32   Node = kNone;
            uint32   Slot = 0;
        };

        uint32 NodeFor(const FVector3& Min, const FVector3& Max);
        void   Link(uint32 Element, uint32 Node);
        void   Unlink(uint32 Element);

        TVector<FNode>    Nodes;
        TVector<FElement> Elements;
        TVector<uint32>   FreeElements;
        float             RootHalfSize;
        float             MinHalfSize;
        uint32            Count = 0;
    };
}
