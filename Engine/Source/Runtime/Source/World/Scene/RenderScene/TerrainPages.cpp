#include "RuntimePCH.h"
#include "TerrainPages.h"
#include "Containers/Algorithm.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Terrain/TerrainStore.h"

namespace Lumina
{
    namespace
    {
        struct FPageNode
        {
            int32       Depth = 0;
            FIntVector2 Coord = FIntVector2(0);
        };

        FPageNode DecodeNode(uint32 Node)
        {
            FPageNode Out;
            while (GetTerrainNodeDepthOffset(Out.Depth + 1) <= Node)
            {
                ++Out.Depth;
            }
            const uint32 Local = Node - GetTerrainNodeDepthOffset(Out.Depth);
            const uint32 Side  = 1u << Out.Depth;
            Out.Coord = FIntVector2((int32)(Local % Side), (int32)(Local / Side));
            return Out;
        }

        uint32 EncodeNode(int32 Depth, const FIntVector2& Coord)
        {
            return GetTerrainNodeDepthOffset(Depth) + (uint32)(Coord.y * (1 << Depth) + Coord.x);
        }

        int32 FloorDiv(int32 Value, int32 Divisor)
        {
            return Value >= 0 ? Value / Divisor : -((-Value + Divisor - 1) / Divisor);
        }

        // Mirrors TerrainNodeBounds in TerrainCommon.slang without the dilation, which only pads the cull.
        bool NodeBox(const FTerrainPageLayout& Layout, const TVector<FVector2>& NodeBounds, int32 TreeDepth, int32 Depth, const FIntVector2& Coord,
                     FVector3& OutMin, FVector3& OutMax)
        {
            const FVector2 Range = NodeBounds[EncodeNode(Depth, Coord)];
            if (Range.x > Range.y)
            {
                return false;
            }
            const int32 Span = GTerrainMeshletQuads << (TreeDepth - Depth);
            const int32 Last = Layout.Resolution - 1;
            const FIntVector2 Origin = Coord * Span;
            const FIntVector2 End(Math::Min(Origin.x + Span, Last), Math::Min(Origin.y + Span, Last));
            const float Low  = Math::Min(Range.x * Layout.MaxHeight, Range.y * Layout.MaxHeight);
            const float High = Math::Max(Range.x * Layout.MaxHeight, Range.y * Layout.MaxHeight);
            OutMin = FVector3(Layout.OriginXZ.x + float(Origin.x) * Layout.SampleSpacing, Layout.WorldY + Low, Layout.OriginXZ.y + float(Origin.y) * Layout.SampleSpacing);
            OutMax = FVector3(Layout.OriginXZ.x + float(End.x) * Layout.SampleSpacing, Layout.WorldY + High, Layout.OriginXZ.y + float(End.y) * Layout.SampleSpacing);
            return true;
        }

        float DistanceToBox(const FVector3& Point, const FVector3& Min, const FVector3& Max)
        {
            return Math::Length(Point - Math::Clamp(Point, Min, Max));
        }

        // The quadtree draws mip M nodes only where it splits their level M + 1 parents, within twice the split distance of mip M itself.
        bool WantsMip(const FTerrainPageLayout& Layout, const FTerrainPageView& View, int32 Mip, float Distance)
        {
            const float QuadWorld = Layout.SampleSpacing * float(1 << Mip);
            if (View.bOrthographic)
            {
                return 2.0f * QuadWorld * View.PixelsPerUnit > TERRAIN_QUAD_PIXELS;
            }
            return Distance < GTerrainPageRadiusScale * QuadWorld * View.PixelsPerUnit / TERRAIN_QUAD_PIXELS;
        }
    }

    void TerrainPages::SelectWanted(const FTerrainPageLayout& Layout, const TVector<FVector2>& NodeBounds, const FTerrainPageView& View,
                                    TVector<FTerrainPageRequest>& Out)
    {
        Out.Reset();
        if (Layout.Resolution < 2)
        {
            return;
        }
        const int32 TreeDepth = GetTerrainTreeDepth(Layout.Resolution);
        const int32 PageDepth = GetTerrainPageDepth(TreeDepth);
        if ((uint32)NodeBounds.Num() != GetTerrainNodeCount(PageDepth))
        {
            return;
        }

        FVector3 Min, Max;
        if (!NodeBox(Layout, NodeBounds, TreeDepth, 0, FIntVector2(0), Min, Max))
        {
            return;
        }
        Out.Add({ 0u, PageDepth, DistanceToBox(View.Camera, Min, Max) });

        // Breadth first, so the list doubles as the queue and every parent lands before its children.
        for (int32 Index = 0; Index < Out.Num(); ++Index)
        {
            const FTerrainPageRequest Parent = Out[Index];
            if (Parent.Mip == 0)
            {
                continue;
            }
            const FPageNode Node = DecodeNode(Parent.Node);
            for (int32 Child = 0; Child < 4; ++Child)
            {
                const FIntVector2 Coord = Node.Coord * 2 + FIntVector2(Child & 1, Child >> 1);
                if (!NodeBox(Layout, NodeBounds, TreeDepth, Node.Depth + 1, Coord, Min, Max))
                {
                    continue;
                }
                const float Distance = DistanceToBox(View.Camera, Min, Max);
                if (WantsMip(Layout, View, Parent.Mip - 1, Distance))
                {
                    Out.Add({ EncodeNode(Node.Depth + 1, Coord), Parent.Mip - 1, Distance });
                }
            }
        }
    }

    void TerrainPages::Build(const FTerrainStore& Store, const FTerrainPageLayout& Layout, uint32 Node, uint16* OutHeights, uint16* OutNormals, uint32* OutWeights)
    {
        const FPageNode Page = DecodeNode(Node);
        const int32 Mip      = Store.GetPageDepth() - Page.Depth;
        const FIntVector2 Min = Page.Coord * GTerrainPageQuads - FIntVector2(GTerrainPageGutter);
        const FIntVector2 Size(GTerrainPageTexels);

        float Heights[GTerrainPageTexelCount];
        Store.ReadHeights(Mip, Min, Size, Heights);
        Store.ReadWeights(Mip, Min, Size, OutWeights);
        for (int32 Texel = 0; Texel < GTerrainPageTexelCount; ++Texel)
        {
            OutHeights[Texel] = (uint16)(Math::Clamp(Heights[Texel], 0.0f, 1.0f) * 65535.0f + 0.5f);
        }

        // Central differences one texel apart, packed as XZ the way shaders rebuild Y from unit length.
        const float Scale = Layout.MaxHeight / (2.0f * float(1 << Mip) * Layout.SampleSpacing);
        for (int32 TexelY = 0; TexelY < GTerrainPageTexels; ++TexelY)
        {
            const int32 Row  = TexelY * GTerrainPageTexels;
            const int32 Up   = Math::Min(TexelY + 1, GTerrainPageTexels - 1) * GTerrainPageTexels;
            const int32 Down = Math::Max(TexelY - 1, 0) * GTerrainPageTexels;
            for (int32 TexelX = 0; TexelX < GTerrainPageTexels; ++TexelX)
            {
                const float DHDX = (Heights[Row + Math::Min(TexelX + 1, GTerrainPageTexels - 1)] - Heights[Row + Math::Max(TexelX - 1, 0)]) * Scale;
                const float DHDZ = (Heights[Up + TexelX] - Heights[Down + TexelX]) * Scale;
                const float InvLength = 1.0f / Math::Sqrt(DHDX * DHDX + 1.0f + DHDZ * DHDZ);
                const uint32 X = (uint32)Math::Clamp((-DHDX * InvLength * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
                const uint32 Z = (uint32)Math::Clamp((-DHDZ * InvLength * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f);
                OutNormals[Row + TexelX] = (uint16)(X | (Z << 8));
            }
        }
    }

    void FTerrainPageStreamer::BeginFrame()
    {
        ++Frame;
        Pending.Reset();
        EvictionOrder.Reset();
        bEvictionOrderBuilt = false;
        if (Slots.IsEmpty())
        {
            Slots.SetNum(GTerrainPageAtlasSlots);
            FreeSlots.Reserve(GTerrainPageAtlasSlots);
            for (uint32 Slot = GTerrainPageAtlasSlots; Slot > 0u; --Slot)
            {
                FreeSlots.Add(Slot - 1u);
            }
        }
    }

    FTerrainPageTable& FTerrainPageStreamer::Prepare(ECS::FEntity Terrain, int32 Resolution, float MaxHeight, float TileWorldSize)
    {
        FTerrainPageTable& Table = Tables.FindOrAdd(Terrain);
        // Normals bake in the height scale and spacing, so changing either rebuilds every resident page.
        const bool bShapeChanged = Table.MaxHeight != MaxHeight || Table.TileWorldSize != TileWorldSize;
        Table.MaxHeight     = MaxHeight;
        Table.TileWorldSize = TileWorldSize;
        if (Table.Resolution != Resolution)
        {
            for (uint32 Slot : Table.Slots)
            {
                if (Slot != GTerrainPageNone)
                {
                    FreeSlot(Slot);
                }
            }
            const uint32 Count = Resolution >= 2 ? GetTerrainNodeCount(GetTerrainPageDepth(GetTerrainTreeDepth(Resolution))) : 0u;
            Table.Slots.Init(GTerrainPageNone, (int32)Count);
            Table.Stale.Init(0u, (int32)Count);
            Table.DirtyRuns.Reset();
            Table.Resolution = Resolution;
            Table.bReset     = true;
        }
        else if (bShapeChanged)
        {
            MarkAllStale(Terrain);
        }
        return Table;
    }

    void FTerrainPageStreamer::MarkStale(ECS::FEntity Terrain, const FIntVector2& Min, const FIntVector2& Max)
    {
        FTerrainPageTable* Table = Tables.Find(Terrain);
        if (Table == nullptr || Table->Resolution < 2 || Min.x > Max.x || Min.y > Max.y)
        {
            return;
        }
        const int32 PageDepth = GetTerrainPageDepth(GetTerrainTreeDepth(Table->Resolution));
        for (int32 Depth = 0; Depth <= PageDepth; ++Depth)
        {
            const int32 Step = 1 << (PageDepth - Depth);
            const int32 Span = GTerrainPageQuads * Step;
            const int32 Side = 1 << Depth;
            // A page reads samples from Gutter steps before its origin to its far edge plus Gutter minus one steps after it.
            const int32 Reach = (GTerrainPageQuads + GTerrainPageGutter - 1) * Step;
            const int32 X0 = Math::Max(0, -FloorDiv(-(Min.x - Reach), Span));
            const int32 Y0 = Math::Max(0, -FloorDiv(-(Min.y - Reach), Span));
            const int32 X1 = Math::Min(Side - 1, FloorDiv(Max.x + GTerrainPageGutter * Step, Span));
            const int32 Y1 = Math::Min(Side - 1, FloorDiv(Max.y + GTerrainPageGutter * Step, Span));
            for (int32 Y = Y0; Y <= Y1; ++Y)
            {
                for (int32 X = X0; X <= X1; ++X)
                {
                    const uint32 Node = EncodeNode(Depth, FIntVector2(X, Y));
                    if (Table->Slots[Node] != GTerrainPageNone)
                    {
                        Table->Stale[Node] = 1u;
                    }
                }
            }
        }
    }

    void FTerrainPageStreamer::MarkAllStale(ECS::FEntity Terrain)
    {
        if (FTerrainPageTable* Table = Tables.Find(Terrain))
        {
            for (int32 Node = 0; Node < Table->Slots.Num(); ++Node)
            {
                Table->Stale[Node] = Table->Slots[Node] != GTerrainPageNone ? 1u : 0u;
            }
        }
    }

    void FTerrainPageStreamer::Request(ECS::FEntity Terrain, const TSharedPtr<FTerrainStore>& Store, const FTerrainPageLayout& Layout, const FTerrainPageView& View)
    {
        FTerrainPageTable* Table = Tables.Find(Terrain);
        if (Table == nullptr || Store == nullptr)
        {
            return;
        }
        TerrainPages::SelectWanted(Layout, Store->GetBounds(), View, Wanted);
        for (const FTerrainPageRequest& Want : Wanted)
        {
            const uint32 Slot = Table->Slots[Want.Node];
            if (Slot != GTerrainPageNone)
            {
                Slots[Slot].LastWanted = Frame;
                if (Table->Stale[Want.Node] == 0u)
                {
                    continue;
                }
            }
            Pending.Add({ Terrain, Store, Layout, Want, Slot != GTerrainPageNone });
        }
    }

    void FTerrainPageStreamer::Commit()
    {
        // Stale pages first since they are already on screen, then coarse before fine, then near before far.
        Algo::Sort(Pending, [](const FPending& A, const FPending& B)
        {
            if (A.bStale != B.bStale)
            {
                return A.bStale;
            }
            if (A.Request.Mip != B.Request.Mip)
            {
                return A.Request.Mip > B.Request.Mip;
            }
            return A.Request.Distance < B.Request.Distance;
        });

        // An editor that extracts without rendering would otherwise queue pages forever.
        const uint32 MaxQueued = GTerrainPageUploadsPerFrame * 4u;
        TVector<int32> Building;
        for (int32 Index = 0; Index < Pending.Num(); ++Index)
        {
            const FPending& Item = Pending[Index];
            if ((uint32)Building.Num() >= GTerrainPageUploadsPerFrame || (uint32)(UploadSlots.Num() + Building.Num()) >= MaxQueued)
            {
                break;
            }
            FTerrainPageTable* Table = Tables.Find(Item.Terrain);
            if (Table == nullptr)
            {
                continue;
            }

            const uint32 Node = Item.Request.Node;
            uint32 Slot = Table->Slots[Node];
            if (Slot == GTerrainPageNone)
            {
                Slot = AllocateSlot();
                if (Slot == GTerrainPageNone)
                {
                    break;
                }
                Slots[Slot] = { Item.Terrain, Node, Frame };
                SetEntry(*Table, Node, Slot);
            }
            Table->Stale[Node] = 0u;
            UploadSlots.Add(Slot);
            Building.Add(Index);
        }

        // Disk reads and decodes dominate a cold page, so pages build across the workers.
        const int32 First = UploadHeights.Num();
        UploadHeights.SetNum(First + Building.Num() * GTerrainPageTexelCount);
        UploadNormals.SetNum(First + Building.Num() * GTerrainPageTexelCount);
        UploadWeights.SetNum(First + Building.Num() * GTerrainPageTexelCount);
        Jobs::ParallelFor((uint32)Building.Num(), [&](uint32 Index)
        {
            const FPending& Item = Pending[Building[(int32)Index]];
            const int32 Offset = First + (int32)Index * GTerrainPageTexelCount;
            TerrainPages::Build(*Item.Store, Item.Layout, Item.Request.Node,
                                UploadHeights.GetData() + Offset, UploadNormals.GetData() + Offset, UploadWeights.GetData() + Offset);
        }, 1);
        // Requests hold their terrain's store, which a deleted terrain must not outlive.
        Pending.Reset();
    }

    void FTerrainPageStreamer::Prune(const TVector<ECS::FEntity>& Live)
    {
        TVector<ECS::FEntity> Gone;
        for (const auto& [Terrain, Table] : Tables)
        {
            if (!Algo::Contains(Live, Terrain))
            {
                Gone.Add(Terrain);
            }
        }
        for (ECS::FEntity Terrain : Gone)
        {
            for (uint32 Slot : Tables.Find(Terrain)->Slots)
            {
                if (Slot != GTerrainPageNone)
                {
                    FreeSlot(Slot);
                }
            }
            Tables.Remove(Terrain);
        }
    }

    uint32 FTerrainPageStreamer::AllocateSlot()
    {
        if (!FreeSlots.IsEmpty())
        {
            return FreeSlots.Pop();
        }

        // Least recently wanted at the back, and nothing wanted this frame is ever a candidate.
        if (!bEvictionOrderBuilt)
        {
            bEvictionOrderBuilt = true;
            for (uint32 Slot = 0; Slot < (uint32)Slots.Num(); ++Slot)
            {
                if (Slots[Slot].Node != GTerrainPageNone && Slots[Slot].LastWanted < Frame)
                {
                    EvictionOrder.Add(Slot);
                }
            }
            Algo::Sort(EvictionOrder, [this](uint32 A, uint32 B) { return Slots[A].LastWanted > Slots[B].LastWanted; });
        }
        if (EvictionOrder.IsEmpty())
        {
            return GTerrainPageNone;
        }

        const uint32 Slot  = EvictionOrder.Pop();
        const FSlot  Owner = Slots[Slot];
        if (FTerrainPageTable* Table = Tables.Find(Owner.Terrain))
        {
            SetEntry(*Table, Owner.Node, GTerrainPageNone);
            Table->Stale[Owner.Node] = 0u;
        }
        Slots[Slot] = {};
        return Slot;
    }

    void FTerrainPageStreamer::FreeSlot(uint32 Slot)
    {
        Slots[Slot] = {};
        FreeSlots.Add(Slot);
    }

    void FTerrainPageStreamer::SetEntry(FTerrainPageTable& Table, uint32 Node, uint32 Slot)
    {
        Table.Slots[Node] = Slot;
        Table.DirtyRuns.Add(FUIntVector2(Node, 1u));
    }
}
