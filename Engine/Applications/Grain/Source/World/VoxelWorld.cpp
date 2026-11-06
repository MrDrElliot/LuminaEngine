#include "VoxelWorld.h"
#include "Noise.h"

#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RHICore.h"
#include "TaskSystem/TaskSystem.h"

#include <cstring>

namespace Grain
{
    namespace
    {
        constexpr float kCaveTopDepth    = 2.0f;
        constexpr float kCaveBottomDepth = 9.0f;
        constexpr float kSurfaceDepth    = 0.30f;
        constexpr float kSoilDepth       = 1.7f;
        constexpr float kCanopyReach     = 9.0f;

        constexpr int32 kTreeCellVoxels = 64;
        constexpr int32 kTreeCellsX     = kWorldVoxelsX / kTreeCellVoxels;
        constexpr int32 kTreeCellsZ     = kWorldVoxelsZ / kTreeCellVoxels;

        constexpr int32 kCoarseVoxels = 64;
        constexpr int32 kCoarseX      = kWorldVoxelsX / kCoarseVoxels;
        constexpr int32 kCoarseZ      = kWorldVoxelsZ / kCoarseVoxels;

        float TerrainHeight(float WorldX, float WorldZ, uint32 Seed)
        {
            const float S = float(Seed & 1023u) * 0.37f;

            // One lattice unit is about 33 m, so a 160 m world carries a real spread of features.
            const float X = WorldX * 0.030f + S;
            const float Z = WorldZ * 0.030f - S;

            // Warping the sample point is what turns round hills into valleys with drainage.
            const float WarpX = X + Noise::Fbm2D(X * 0.6f, Z * 0.6f, 3) * 1.7f;
            const float WarpZ = Z + Noise::Fbm2D(X * 0.6f + 41.3f, Z * 0.6f + 7.9f, 3) * 1.7f;

            const float Rolling = Noise::Fbm2D(WarpX, WarpZ, 6);
            const float Detail  = Noise::Fbm2D(WarpX * 4.0f, WarpZ * 4.0f, 4);
            const float Ridge   = Noise::Ridged2D(WarpX * 0.70f, WarpZ * 0.70f, 5);
            const float Massif  = Noise::Fbm2D(X * 0.45f, Z * 0.45f, 3);

            // Fine grain breaks the contour terraces that a smooth height field leaves in voxels.
            const float Grain = Noise::Fbm2D(WorldX * 0.31f, WorldZ * 0.31f, 3);

            // A normalized fbm clusters hard around 0.5, so both fields are stretched before use.
            const float Shaped = Math::Clamp((Rolling - 0.5f) * 2.2f + 0.5f, 0.0f, 1.0f);
            const float Mass   = Math::Clamp((Massif - 0.5f) * 2.6f + 0.5f, 0.0f, 1.0f);

            const float MountainMask = Math::Clamp((Mass - 0.52f) * 3.2f, 0.0f, 1.0f);
            const float Basin        = Math::Clamp((0.46f - Mass) * 3.2f, 0.0f, 1.0f);

            // Centered, or the normalized fbm never dips far enough below sea level to cut a lake.
            float Height = 30.0f;
            Height += (Shaped - 0.5f) * 34.0f;
            Height += (Detail - 0.5f) * 4.4f;
            Height += Grain * 0.34f;
            Height += Ridge * Ridge * MountainMask * 56.0f;
            Height -= Basin * 14.0f;

            return Height;
        }

        bool IsCaveField(float WorldX, float WorldY, float WorldZ, uint32 Seed)
        {
            const float S = float(Seed & 255u) * 0.13f;
            const float A = Noise::Value3D(WorldX * 0.055f + S, WorldY * 0.075f, WorldZ * 0.055f - S);

            // Folded so the field forms continuous tunnels rather than isolated bubbles.
            const float TubeA = Math::Abs(A * 2.0f - 1.0f);
            if (TubeA > 0.11f)
            {
                return false;
            }

            const float B = Noise::Value3D(WorldX * 0.130f - S, WorldY * 0.160f, WorldZ * 0.130f + S);
            return Math::Abs(B * 2.0f - 1.0f) < 0.26f;
        }

        uint8 StrataAt(int32 VoxelX, int32 VoxelY, int32 VoxelZ, float Depth)
        {
            const float WorldY = float(VoxelY) * kVoxelSize;

            const float Band = Noise::Value2D(WorldY * 0.35f, float(VoxelX) * 0.004f);
            if (Band > 0.72f)
            {
                return uint8(EMaterial::Clay);
            }
            if (Band < 0.19f)
            {
                return uint8(EMaterial::Gravel);
            }

            if (Depth > 4.0f)
            {
                const float Vein = Noise::Value3D(float(VoxelX) * 0.06f, WorldY * 0.09f, float(VoxelZ) * 0.06f);
                if (Vein > 0.855f)
                {
                    return uint8(EMaterial::Ore);
                }
            }

            return uint8(EMaterial::Stone);
        }
    }

    void FVoxelWorld::BuildHeightfield()
    {
        Columns.SetNum(kWorldVoxelsX * kWorldVoxelsZ);

        const uint32 LocalSeed = Seed;

        Jobs::ParallelFor(uint32(kWorldVoxelsZ), [this, LocalSeed](const Jobs::FParallelRange& Range)
        {
            for (uint32 Z = Range.Start; Z < Range.End; ++Z)
            {
                FSurface* Row = Columns.GetData() + Z * kWorldVoxelsX;
                for (int32 X = 0; X < kWorldVoxelsX; ++X)
                {
                    Row[X].Height = TerrainHeight(float(X) * kVoxelSize, float(Z) * kVoxelSize, LocalSeed);
                }
            }
        }, 8);

        // Slope needs both neighbors, so surface materials are a second pass over the finished heights.
        Jobs::ParallelFor(uint32(kWorldVoxelsZ), [this](const Jobs::FParallelRange& Range)
        {
            for (uint32 Z = Range.Start; Z < Range.End; ++Z)
            {
                const int32 RowBase = Z * kWorldVoxelsX;
                const int32 RowPrev = Math::Max(int32(Z) - 4, 0) * kWorldVoxelsX;
                const int32 RowNext = Math::Min(int32(Z) + 4, kWorldVoxelsZ - 1) * kWorldVoxelsX;

                for (int32 X = 0; X < kWorldVoxelsX; ++X)
                {
                    FSurface& Out = Columns[RowBase + X];

                    const int32 XPrev = Math::Max(X - 4, 0);
                    const int32 XNext = Math::Min(X + 4, kWorldVoxelsX - 1);

                    const float DX = Columns[RowBase + XNext].Height - Columns[RowBase + XPrev].Height;
                    const float DZ = Columns[RowNext + X].Height - Columns[RowPrev + X].Height;

                    const float Slope  = Math::Sqrt(DX * DX + DZ * DZ) / (8.0f * kVoxelSize);
                    const float Height = Out.Height;
                    const float Blend  = Noise::Value2D(float(X) * 0.03f, float(Z) * 0.03f);

                    uint8 Surface = uint8(EMaterial::Grass);
                    uint8 Sub     = uint8(EMaterial::Dirt);

                    // Altitude wins over slope, or the peaks are pure rock and never get a snow cap.
                    if (Height > 50.0f + Blend * 14.0f && Slope < 1.9f + Blend * 0.5f)
                    {
                        Surface = uint8(EMaterial::Snow);
                        Sub     = uint8(EMaterial::Stone);
                    }
                    else if (Slope > 2.30f + Blend * 0.7f)
                    {
                        Surface = uint8(EMaterial::Rock);
                        Sub     = uint8(EMaterial::Stone);
                    }
                    else if (Height < kSeaLevel + 1.3f + Blend * 0.9f)
                    {
                        Surface = uint8(EMaterial::Sand);
                        Sub     = uint8(EMaterial::Sand);
                    }
                    else if (Slope > 1.05f && Blend > 0.58f)
                    {
                        Surface = uint8(EMaterial::Moss);
                        Sub     = uint8(EMaterial::Dirt);
                    }

                    Out.Surface    = Surface;
                    Out.SubSurface = Sub;
                    Out.SlopeScale = Math::Min(Math::Sqrt(1.0f + Slope * Slope), 5.0f);
                }
            }
        }, 8);

        {
            int64 Tally[int32(EMaterial::Count)] = {};
            float MinH = 1e9f;
            float MaxH = -1e9f;
            for (const FSurface& Column : Columns)
            {
                ++Tally[Column.Surface];
                MinH = Math::Min(MinH, Column.Height);
                MaxH = Math::Max(MaxH, Column.Height);
            }
            LOG_INFO("Grain: height {:.1f} to {:.1f} m. grass {} dirt {} stone {} rock {} sand {} snow {} moss {}",
                MinH, MaxH, Tally[1], Tally[2], Tally[3], Tally[4], Tally[5], Tally[6], Tally[12]);
        }

        CoarseMin.Init(0.0f, kCoarseX * kCoarseZ);
        CoarseMax.Init(0.0f, kCoarseX * kCoarseZ);

        Jobs::ParallelFor(uint32(kCoarseZ), [this](const Jobs::FParallelRange& Range)
        {
            for (uint32 CZ = Range.Start; CZ < Range.End; ++CZ)
            {
                for (int32 CX = 0; CX < kCoarseX; ++CX)
                {
                    float MinH = 1e9f;
                    float MaxH = -1e9f;

                    for (int32 Z = 0; Z < kCoarseVoxels; ++Z)
                    {
                        const int32 RowBase = (int32(CZ) * kCoarseVoxels + Z) * kWorldVoxelsX;
                        for (int32 X = 0; X < kCoarseVoxels; ++X)
                        {
                            const float H = Columns[RowBase + (CX * kCoarseVoxels + X)].Height;
                            MinH = Math::Min(MinH, H);
                            MaxH = Math::Max(MaxH, H);
                        }
                    }

                    CoarseMin[CZ * kCoarseX + CX] = MinH;
                    CoarseMax[CZ * kCoarseX + CX] = MaxH;
                }
            }
        }, 1);
    }

    void FVoxelWorld::ScatterTrees()
    {
        Trees.Init(FTree{}, kTreeCellsX * kTreeCellsZ);

        for (int32 CZ = 0; CZ < kTreeCellsZ; ++CZ)
        {
            for (int32 CX = 0; CX < kTreeCellsX; ++CX)
            {
                if (Noise::Hash21(CX * 7919 + int32(Seed), CZ * 104729) > 0.46f)
                {
                    continue;
                }

                const int32 VX = CX * kTreeCellVoxels + int32(Noise::Hash21(CX + 11, CZ * 3 + 5) * 40.0f) + 12;
                const int32 VZ = CZ * kTreeCellVoxels + int32(Noise::Hash21(CX * 5 + 3, CZ + 17) * 40.0f) + 12;

                const FSurface& Column = ColumnAt(VX, VZ);
                const bool bSoil = Column.Surface == uint8(EMaterial::Grass)
                                || Column.Surface == uint8(EMaterial::Moss);

                if (!bSoil || Column.Height < kSeaLevel + 1.5f || Column.Height > 54.0f)
                {
                    continue;
                }

                FTree& Tree = Trees[CZ * kTreeCellsX + CX];
                Tree.VoxelX = VX;
                Tree.VoxelZ = VZ;
                Tree.BaseY  = int32(Column.Height / kVoxelSize);
                Tree.Trunk  = 46 + int32(Noise::Hash21(VX, VZ) * 44.0f);
                Tree.Radius = 13.0f + Noise::Hash21(VZ, VX) * 10.0f;
                Tree.bValid = true;
            }
        }
    }

    const FSurface& FVoxelWorld::ColumnAt(int32 X, int32 Z) const
    {
        const int32 CX = Math::Clamp(X, 0, kWorldVoxelsX - 1);
        const int32 CZ = Math::Clamp(Z, 0, kWorldVoxelsZ - 1);
        return Columns[CZ * kWorldVoxelsX + CX];
    }

    float FVoxelWorld::SampleHeight(float WorldX, float WorldZ) const
    {
        if (Columns.IsEmpty())
        {
            return kSeaLevel;
        }
        return ColumnAt(int32(WorldX / kVoxelSize), int32(WorldZ / kVoxelSize)).Height;
    }

    bool FVoxelWorld::FindCavePosition(FVector3& OutPosition) const
    {
        int32 BestClearance = 0;
        int32 Crystals = 0;

        // Near the world edge a cave opens onto the boundary and the shot fills with sky.
        const int32 Margin = kWorldVoxelsX / 4;

        for (int32 Z = Margin; Z < kWorldVoxelsZ - Margin; Z += 11)
        {
            for (int32 X = Margin; X < kWorldVoxelsX - Margin; X += 11)
            {
                const float Height = ColumnAt(X, Z).Height;
                if (Height < kSeaLevel + 8.0f)
                {
                    continue;
                }

                for (float Depth = 3.0f; Depth < 8.0f; Depth += 0.25f)
                {
                    const int32 Y = int32((Height - Depth) / kVoxelSize);
                    if (SampleVoxel(X, Y, Z) != uint8(EMaterial::Air))
                    {
                        continue;
                    }

                    // A slit passes a horizontal probe, so the pocket has to be open above and below too.
                    if (SampleVoxel(X, Y + 1, Z) != uint8(EMaterial::Air)
                     || SampleVoxel(X, Y - 1, Z) != uint8(EMaterial::Air))
                    {
                        continue;
                    }

                    int32 Clearance = 0;
                    for (int32 Step = 1; Step <= 40; ++Step)
                    {
                        const bool bOpen = SampleVoxel(X + Step, Y, Z) == uint8(EMaterial::Air)
                                        && SampleVoxel(X, Y, Z + Step) == uint8(EMaterial::Air);
                        if (!bOpen)
                        {
                            break;
                        }
                        ++Clearance;
                    }

                    for (int32 Step = -12; Step <= 12; ++Step)
                    {
                        if (SampleVoxel(X + Step, Y + 2, Z) == uint8(EMaterial::Crystal))
                        {
                            ++Crystals;
                        }
                    }

                    if (Clearance > BestClearance)
                    {
                        BestClearance = Clearance;
                        OutPosition = { (float(X) + 0.5f) * kVoxelSize,
                                        (float(Y) + 0.5f) * kVoxelSize,
                                        (float(Z) + 0.5f) * kVoxelSize };
                    }
                }
            }
        }

        LOG_INFO("Grain: best cave clearance {} voxels, {} crystals nearby.", BestClearance, Crystals);
        return BestClearance > 4;
    }

    uint8 FVoxelWorld::SampleTree(int32 X, int32 Y, int32 Z) const
    {
        const int32 CellX = X / kTreeCellVoxels;
        const int32 CellZ = Z / kTreeCellVoxels;

        for (int32 DZ = -1; DZ <= 1; ++DZ)
        {
            const int32 CZ = CellZ + DZ;
            if (CZ < 0 || CZ >= kTreeCellsZ)
            {
                continue;
            }

            for (int32 DX = -1; DX <= 1; ++DX)
            {
                const int32 CX = CellX + DX;
                if (CX < 0 || CX >= kTreeCellsX)
                {
                    continue;
                }

                const FTree& Tree = Trees[CZ * kTreeCellsX + CX];
                if (!Tree.bValid)
                {
                    continue;
                }

                const float RY = float(Y - Tree.BaseY);
                if (RY < 0.0f || RY > float(Tree.Trunk) + Tree.Radius * 1.7f)
                {
                    continue;
                }

                const float RX = float(X - Tree.VoxelX);
                const float RZ = float(Z - Tree.VoxelZ);
                const float R2 = RX * RX + RZ * RZ;

                const float TrunkRadius = 2.7f - RY / float(Tree.Trunk) * 1.2f;
                if (RY < float(Tree.Trunk) && R2 < TrunkRadius * TrunkRadius)
                {
                    return uint8(EMaterial::Wood);
                }

                const float CrownY = float(Tree.Trunk) * 0.80f + Tree.Radius * 0.55f;
                const float DY = (RY - CrownY) / (Tree.Radius * 1.2f);
                const float DR = Math::Sqrt(R2) / Tree.Radius;
                const float Blob = DR * DR + DY * DY;

                if (Blob < 1.0f)
                {
                    if (Blob < 0.62f)
                    {
                        return uint8(EMaterial::Leaves);
                    }

                    // Breaking up the outer shell keeps canopies from reading as smooth ellipsoids.
                    const float Ragged = Noise::Value3D(float(X) * 0.19f, float(Y) * 0.19f, float(Z) * 0.19f);
                    if (Ragged > (Blob - 0.62f) * 2.3f)
                    {
                        return uint8(EMaterial::Leaves);
                    }
                }
            }
        }

        return uint8(EMaterial::Air);
    }

    uint8 FVoxelWorld::SampleVoxel(int32 X, int32 Y, int32 Z) const
    {
        const FSurface& Column = ColumnAt(X, Z);
        const float WorldY = float(Y) * kVoxelSize;
        const float Depth  = Column.Height - WorldY;

        if (Depth < 0.0f)
        {
            if (WorldY < Column.Height + kCanopyReach)
            {
                const uint8 Tree = SampleTree(X, Y, Z);
                if (Tree != uint8(EMaterial::Air))
                {
                    return Tree;
                }
            }
            return WorldY < kSeaLevel ? uint8(EMaterial::Water) : uint8(EMaterial::Air);
        }

        if (Depth > kCaveTopDepth && Depth < kCaveBottomDepth
            && IsCaveField(float(X) * kVoxelSize, WorldY, float(Z) * kVoxelSize, Seed))
        {
            // Crystal seams line the tunnels, which is what lights the caves once bounce rays run.
            if (Noise::Value3D(float(X) * 0.29f, WorldY * 0.29f, float(Z) * 0.29f) > 0.845f)
            {
                return uint8(EMaterial::Crystal);
            }
            return uint8(EMaterial::Air);
        }

        if (Depth < kSurfaceDepth * Column.SlopeScale)
        {
            return Column.Surface;
        }
        if (Depth < kSoilDepth * Column.SlopeScale)
        {
            return Column.SubSurface;
        }
        return StrataAt(X, Y, Z, Depth);
    }

    uint32 FVoxelWorld::BuildLeaf(FBuildBuffer& Buffer, int32 BaseX, int32 BaseY, int32 BaseZ) const
    {
        uint8 Voxels[kNodeSlots];
        uint32 Mask[kMaskWords] = {};
        uint32 Solid = 0;

        for (int32 Y = 0; Y < kNodeSide; ++Y)
        {
            for (int32 Z = 0; Z < kNodeSide; ++Z)
            {
                for (int32 X = 0; X < kNodeSide; ++X)
                {
                    const uint32 Slot = SlotIndex(X, Y, Z);
                    const uint8 Material = SampleVoxel(BaseX + X, BaseY + Y, BaseZ + Z);
                    Voxels[Slot] = Material;

                    if (Material != uint8(EMaterial::Air))
                    {
                        SetBit(Mask, Slot);
                        ++Solid;
                    }
                }
            }
        }

        if (Solid == 0)
        {
            return ~0u;
        }

        int32 Counts[int32(EMaterial::Count)] = {};
        for (int32 Slot = 0; Slot < kNodeSlots; ++Slot)
        {
            ++Counts[Voxels[Slot]];
        }
        Counts[0] = 0;

        uint8 Palette[4] = { 0, 0, 0, 0 };
        int32 PaletteSize = 0;

        // An emissive vein is always the rarest material in its node, so frequency alone would drop it.
        for (const EMaterial Lit : { EMaterial::Crystal, EMaterial::Lava })
        {
            if (Counts[int32(Lit)] > 0)
            {
                Palette[PaletteSize++] = uint8(Lit);
                Counts[int32(Lit)] = 0;
            }
        }

        for (int32 Pick = PaletteSize; Pick < 4; ++Pick)
        {
            int32 Best = -1;
            for (int32 Material = 1; Material < int32(EMaterial::Count); ++Material)
            {
                if (Counts[Material] > 0 && (Best < 0 || Counts[Material] > Counts[Best]))
                {
                    Best = Material;
                }
            }
            if (Best < 0)
            {
                break;
            }
            Palette[PaletteSize++] = uint8(Best);
            Counts[Best] = 0;
        }

        const uint32 NodeIndex = uint32(Buffer.Nodes.Num());
        Buffer.Nodes.Emplace();
        Buffer.Masks.SetNum(Buffer.Masks.Num() + kMaskWords);

        uint32* NodeMask = Buffer.Masks.GetData() + NodeIndex * kMaskWords;
        for (int32 i = 0; i < kMaskWords; ++i)
        {
            NodeMask[i] = Mask[i];
        }

        FVoxNode& Node = Buffer.Nodes[NodeIndex];
        Node.Flags = kFlagPresent | kFlagLeaf;

        if (PaletteSize == 1)
        {
            Node.Flags  |= kFlagUniform;
            Node.Palette = Palette[0];

            if (Solid == uint32(kNodeSlots))
            {
                Node.Flags |= kFlagSolid;
                ++Buffer.SolidNodes;
            }
            else
            {
                ++Buffer.LeafNodes;
            }
            return NodeIndex;
        }

        Node.Palette = uint32(Palette[0]) | (uint32(Palette[1]) << 8)
                     | (uint32(Palette[2]) << 16) | (uint32(Palette[3]) << 24);
        Node.PayloadBase = uint32(Buffer.Payload.Num());

        constexpr int32 kPayloadWords = kNodeSlots / 16;
        Buffer.Payload.SetNum(Buffer.Payload.Num() + kPayloadWords, 0u);
        uint32* Words = Buffer.Payload.GetData() + Node.PayloadBase;

        for (int32 Slot = 0; Slot < kNodeSlots; ++Slot)
        {
            const uint8 Material = Voxels[Slot];
            if (Material == uint8(EMaterial::Air))
            {
                continue;
            }

            uint32 Index = 0;
            for (int32 i = 0; i < PaletteSize; ++i)
            {
                if (Palette[i] == Material)
                {
                    Index = uint32(i);
                    break;
                }
            }
            Words[Slot >> 4] |= Index << ((uint32(Slot) & 15u) * 2u);
        }

        ++Buffer.LeafNodes;
        ++Buffer.PaletteNodes;
        return NodeIndex;
    }

    uint32 FVoxelWorld::BuildNode(FBuildBuffer& Buffer, int32 Level, int32 BaseX, int32 BaseY, int32 BaseZ) const
    {
        const int32 ChildSpan = kLevelVoxels[Level - 1];
        const int32 Span = ChildSpan * kNodeSide;

        float MinH = 1e9f;
        float MaxH = -1e9f;

        const int32 CoarseX0 = Math::Clamp(BaseX / kCoarseVoxels, 0, kCoarseX - 1);
        const int32 CoarseZ0 = Math::Clamp(BaseZ / kCoarseVoxels, 0, kCoarseZ - 1);
        const int32 CoarseX1 = Math::Clamp((BaseX + Span - 1) / kCoarseVoxels, 0, kCoarseX - 1);
        const int32 CoarseZ1 = Math::Clamp((BaseZ + Span - 1) / kCoarseVoxels, 0, kCoarseZ - 1);

        for (int32 CZ = CoarseZ0; CZ <= CoarseZ1; ++CZ)
        {
            for (int32 CX = CoarseX0; CX <= CoarseX1; ++CX)
            {
                MinH = Math::Min(MinH, CoarseMin[CZ * kCoarseX + CX]);
                MaxH = Math::Max(MaxH, CoarseMax[CZ * kCoarseX + CX]);
            }
        }

        const float NodeBottom = float(BaseY) * kVoxelSize;
        const float NodeTop    = float(BaseY + Span) * kVoxelSize;

        if (NodeBottom > MaxH + kCanopyReach && NodeBottom >= kSeaLevel)
        {
            return ~0u;
        }

        // Everything below the deepest cave is one material, so the whole subtree collapses into one node.
        if (NodeTop < MinH - kCaveBottomDepth)
        {
            const uint32 SolidIndex = uint32(Buffer.Nodes.Num());
            Buffer.Nodes.Emplace();
            Buffer.Masks.SetNum(Buffer.Masks.Num() + kMaskWords, 0xFFFFFFFFu);

            Buffer.Nodes[SolidIndex].Flags   = kFlagPresent | kFlagSolid | kFlagUniform;
            Buffer.Nodes[SolidIndex].Palette = uint32(EMaterial::Stone);
            ++Buffer.SolidNodes;
            return SolidIndex;
        }

        uint32 ChildIndex[kNodeSlots];
        uint32 Mask[kMaskWords] = {};
        uint32 Present = 0;

        // A run of children that all collapse can be truncated, because a solid child appends one record.
        const int32 NodesBefore = Buffer.Nodes.Num();
        bool bAllSolid = true;
        uint32 SharedMaterial = ~0u;

        for (int32 Y = 0; Y < kNodeSide; ++Y)
        {
            for (int32 Z = 0; Z < kNodeSide; ++Z)
            {
                for (int32 X = 0; X < kNodeSide; ++X)
                {
                    const uint32 Slot = SlotIndex(X, Y, Z);

                    const int32 ChildX = BaseX + X * ChildSpan;
                    const int32 ChildY = BaseY + Y * ChildSpan;
                    const int32 ChildZ = BaseZ + Z * ChildSpan;

                    const uint32 Child = Level == 2
                        ? BuildLeaf(Buffer, ChildX, ChildY, ChildZ)
                        : BuildNode(Buffer, Level - 1, ChildX, ChildY, ChildZ);

                    ChildIndex[Slot] = Child;
                    if (Child == ~0u)
                    {
                        bAllSolid = false;
                        continue;
                    }

                    SetBit(Mask, Slot);
                    ++Present;

                    const FVoxNode& ChildNode = Buffer.Nodes[Child];
                    if ((ChildNode.Flags & (kFlagSolid | kFlagUniform)) != (kFlagSolid | kFlagUniform))
                    {
                        bAllSolid = false;
                    }
                    else if (SharedMaterial == ~0u)
                    {
                        SharedMaterial = ChildNode.Palette;
                    }
                    else if (SharedMaterial != ChildNode.Palette)
                    {
                        bAllSolid = false;
                    }
                }
            }
        }

        if (Present == 0)
        {
            return ~0u;
        }

        // Uniform rock under the cave floor collapses all the way up, which is most of the world by volume.
        if (bAllSolid && Present == uint32(kNodeSlots))
        {
            Buffer.Nodes.SetNum(NodesBefore);
            Buffer.Masks.SetNum(NodesBefore * kMaskWords);
            Buffer.SolidNodes -= kNodeSlots;

            const uint32 CollapsedIndex = uint32(Buffer.Nodes.Num());
            Buffer.Nodes.Emplace();
            Buffer.Masks.SetNum(Buffer.Masks.Num() + kMaskWords, 0xFFFFFFFFu);

            Buffer.Nodes[CollapsedIndex].Flags   = kFlagPresent | kFlagSolid | kFlagUniform;
            Buffer.Nodes[CollapsedIndex].Palette = SharedMaterial;
            ++Buffer.SolidNodes;
            return CollapsedIndex;
        }

        // Child records stay wherever the recursion put them, so the parent points at a compacted index run.
        const uint32 ChildBase = uint32(Buffer.ChildIndices.Num());
        for (int32 Slot = 0; Slot < kNodeSlots; ++Slot)
        {
            if (ChildIndex[Slot] != ~0u)
            {
                Buffer.ChildIndices.Add(ChildIndex[Slot]);
            }
        }

        // The dominant child material is what a distant ray shades with once it stops descending.
        int32 Tally[int32(EMaterial::Count)] = {};
        for (int32 Slot = 0; Slot < kNodeSlots; ++Slot)
        {
            if (ChildIndex[Slot] != ~0u)
            {
                ++Tally[Buffer.Nodes[ChildIndex[Slot]].Palette & 0xFFu];
            }
        }

        uint32 Dominant = uint32(EMaterial::Stone);
        int32 Best = 0;
        for (int32 Material = 1; Material < int32(EMaterial::Count); ++Material)
        {
            if (Tally[Material] > Best)
            {
                Best = Tally[Material];
                Dominant = uint32(Material);
            }
        }

        const uint32 NodeIndex = uint32(Buffer.Nodes.Num());
        Buffer.Nodes.Emplace();
        Buffer.Masks.SetNum(Buffer.Masks.Num() + kMaskWords);

        Buffer.Nodes[NodeIndex].ChildBase = ChildBase;
        Buffer.Nodes[NodeIndex].Palette   = Dominant;
        Buffer.Nodes[NodeIndex].Flags     = kFlagPresent;

        uint32* NodeMask = Buffer.Masks.GetData() + NodeIndex * kMaskWords;
        for (int32 i = 0; i < kMaskWords; ++i)
        {
            NodeMask[i] = Mask[i];
        }

        ++Buffer.MidNodes;
        return NodeIndex;
    }

    void FVoxelWorld::Merge(TVector<FBuildBuffer>& Buffers, const TVector<uint32>& RootLocal)
    {
        Nodes.Init(FVoxNode{}, kRootCount);
        Masks.Init(0u, kRootCount * kMaskWords);
        ChildIndices.Reset();
        Payload.Reset();

        for (int32 Root = 0; Root < kRootCount; ++Root)
        {
            FBuildBuffer& Buffer = Buffers[Root];

            Stats.LeafNodes    += Buffer.LeafNodes;
            Stats.MidNodes     += Buffer.MidNodes;
            Stats.SolidNodes   += Buffer.SolidNodes;
            Stats.PaletteNodes += Buffer.PaletteNodes;

            if (RootLocal[Root] == ~0u)
            {
                continue;
            }

            const uint32 NodeOffset    = uint32(Nodes.Num());
            const uint32 ChildOffset   = uint32(ChildIndices.Num());
            const uint32 PayloadOffset = uint32(Payload.Num());

            for (uint32 Index : Buffer.ChildIndices)
            {
                ChildIndices.Add(Index + NodeOffset);
            }
            Payload.Append(Buffer.Payload);

            for (FVoxNode Node : Buffer.Nodes)
            {
                if (Node.Flags & kFlagLeaf)
                {
                    if ((Node.Flags & kFlagUniform) == 0)
                    {
                        Node.PayloadBase += PayloadOffset;
                    }
                }
                else if ((Node.Flags & kFlagSolid) == 0)
                {
                    Node.ChildBase += ChildOffset;
                }
                Nodes.Add(Node);
            }

            Masks.Append(Buffer.Masks);

            // The root slot holds a copy of its own subtree root, so the shader can index the grid directly.
            const uint32 LocalRoot = RootLocal[Root];
            Nodes[Root] = Nodes[NodeOffset + LocalRoot];

            const uint32* Source = Masks.GetData() + (NodeOffset + LocalRoot) * kMaskWords;
            uint32* Dest = Masks.GetData() + Root * kMaskWords;
            for (int32 i = 0; i < kMaskWords; ++i)
            {
                Dest[i] = Source[i];
            }

            Buffer = FBuildBuffer{};
        }
    }

    // Derived from the finished masks, and only interior nodes are indexed by rank, so digging out voxel bits never invalidates it.
    void FVoxelWorld::BuildRankPrefix()
    {
        const int32 NodeCount = Nodes.Num();
        Prefix.Init(0u, NodeCount * 8);

        Jobs::ParallelFor(uint32(NodeCount), [this](const Jobs::FParallelRange& Range)
        {
            for (uint32 Node = Range.Start; Node < Range.End; ++Node)
            {
                const uint32* Words = Masks.GetData() + Node * kMaskWords;
                uint32* Out = Prefix.GetData() + Node * 8;

                uint32 Running = 0;
                for (int32 Word = 0; Word < kMaskWords; ++Word)
                {
                    const uint32 Shift = (uint32(Word) & 1u) != 0u ? 16u : 0u;
                    Out[Word >> 1] |= (Running & 0xFFFFu) << Shift;
                    Running += uint32(std::popcount(Words[Word]));
                }
            }
        }, 512);
    }

    void FVoxelWorld::Generate(uint32 InSeed)
    {
        Seed = InSeed;
        const double Started = PlatformTime::Seconds();

        BuildHeightfield();
        ScatterTrees();

        const double Heightfield = PlatformTime::Seconds();

        TVector<FBuildBuffer> Buffers;
        Buffers.SetNum(kRootCount);

        TVector<uint32> RootLocal;
        RootLocal.Init(~0u, kRootCount);

        Jobs::ParallelFor(uint32(kRootCount), [this, &Buffers, &RootLocal](const Jobs::FParallelRange& Range)
        {
            for (uint32 Root = Range.Start; Root < Range.End; ++Root)
            {
                const int32 RX = int32(Root) % kRootCountX;
                const int32 RZ = (int32(Root) / kRootCountX) % kRootCountZ;
                const int32 RY = int32(Root) / (kRootCountX * kRootCountZ);

                RootLocal[Root] = BuildNode(Buffers[Root], 3,
                    RX * kVoxelsPerRoot, RY * kVoxelsPerRoot, RZ * kVoxelsPerRoot);
            }
        }, 1);

        Merge(Buffers, RootLocal);
        BuildRankPrefix();

        Stats.NodeBytes    = Nodes.Num() * sizeof(FVoxNode);
        Stats.MaskBytes    = Masks.Num() * sizeof(uint32);
        Stats.ChildBytes   = ChildIndices.Num() * sizeof(uint32);
        Stats.PayloadBytes = Payload.Num() * sizeof(uint32);
        Stats.BuildSeconds = PlatformTime::Seconds() - Started;

        LOG_INFO("Grain: world {} x {} x {} voxels at {:.1f} cm.",
            kWorldVoxelsX, kWorldVoxelsY, kWorldVoxelsZ, kVoxelSize * 100.0f);
        LOG_INFO("Grain: built in {:.2f} s ({:.2f} s heightfield). {} leaf, {} mid, {} solid, {} palette.",
            Stats.BuildSeconds, Heightfield - Started,
            Stats.LeafNodes, Stats.MidNodes, Stats.SolidNodes, Stats.PaletteNodes);
        LOG_INFO("Grain: {:.1f} MB nodes, {:.1f} MB masks, {:.1f} MB rank, {:.1f} MB child, {:.1f} MB payload.",
            double(Stats.NodeBytes) / 1048576.0, double(Stats.MaskBytes) / 1048576.0,
            double(Prefix.Num() * sizeof(uint32)) / 1048576.0,
            double(Stats.ChildBytes) / 1048576.0, double(Stats.PayloadBytes) / 1048576.0);
    }

    bool FVoxelWorld::IsSolidVoxel(int32 X, int32 Y, int32 Z) const
    {
        if (X < 0 || Y < 0 || Z < 0 || X >= kWorldVoxelsX || Y >= kWorldVoxelsY || Z >= kWorldVoxelsZ)
        {
            return false;
        }

        if (Nodes.IsEmpty())
        {
            return false;
        }

        const int32 RootX = X >> 9;
        const int32 RootY = Y >> 9;
        const int32 RootZ = Z >> 9;

        uint32 NodeIndex = uint32((RootY * kRootCountZ + RootZ) * kRootCountX + RootX);
        FVoxNode Node = Nodes[NodeIndex];

        if ((Node.Flags & kFlagPresent) == 0u)
        {
            return false;
        }

        int32 BaseX = RootX << 9;
        int32 BaseY = RootY << 9;
        int32 BaseZ = RootZ << 9;
        int32 NodeSize = kVoxelsPerRoot;

        for (int32 Level = 0; Level < 3; ++Level)
        {
            if ((Node.Flags & kFlagSolid) != 0u)
            {
                return true;
            }

            const int32 ChildSize = NodeSize >> 3;
            const int32 LocalX = (X - BaseX) / ChildSize;
            const int32 LocalY = (Y - BaseY) / ChildSize;
            const int32 LocalZ = (Z - BaseZ) / ChildSize;
            const uint32 Slot = SlotIndex(LocalX, LocalY, LocalZ);

            const uint32* Mask = Masks.GetData() + NodeIndex * kMaskWords;
            if (!TestBit(Mask, Slot))
            {
                return false;
            }

            if ((Node.Flags & kFlagLeaf) != 0u)
            {
                return true;
            }

            NodeIndex = ChildIndices[Node.ChildBase + PopcountBefore(Mask, Slot)];
            Node = Nodes[NodeIndex];

            BaseX += LocalX * ChildSize;
            BaseY += LocalY * ChildSize;
            BaseZ += LocalZ * ChildSize;
            NodeSize = ChildSize;
        }

        return true;
    }

    bool FVoxelWorld::FindLeafNode(int32 X, int32 Y, int32 Z, uint32& OutNode, int32 OutBase[3]) const
    {
        if (X < 0 || Y < 0 || Z < 0 || X >= kWorldVoxelsX || Y >= kWorldVoxelsY || Z >= kWorldVoxelsZ
            || Nodes.IsEmpty())
        {
            return false;
        }

        const int32 RootX = X >> 9;
        const int32 RootY = Y >> 9;
        const int32 RootZ = Z >> 9;

        uint32 NodeIndex = uint32((RootY * kRootCountZ + RootZ) * kRootCountX + RootX);
        FVoxNode Node = Nodes[NodeIndex];

        if ((Node.Flags & kFlagPresent) == 0u)
        {
            return false;
        }

        int32 BaseX = RootX << 9;
        int32 BaseY = RootY << 9;
        int32 BaseZ = RootZ << 9;
        int32 NodeSize = kVoxelsPerRoot;

        for (int32 Level = 0; Level < 3; ++Level)
        {
            if ((Node.Flags & kFlagLeaf) != 0u)
            {
                OutNode = NodeIndex;
                OutBase[0] = BaseX;
                OutBase[1] = BaseY;
                OutBase[2] = BaseZ;
                return true;
            }

            if ((Node.Flags & kFlagSolid) != 0u)
            {
                return false;
            }

            const int32 ChildSize = NodeSize >> 3;
            const int32 LocalX = (X - BaseX) / ChildSize;
            const int32 LocalY = (Y - BaseY) / ChildSize;
            const int32 LocalZ = (Z - BaseZ) / ChildSize;
            const uint32 Slot = SlotIndex(LocalX, LocalY, LocalZ);

            const uint32* Mask = Masks.GetData() + NodeIndex * kMaskWords;
            if (!TestBit(Mask, Slot))
            {
                return false;
            }

            NodeIndex = ChildIndices[Node.ChildBase + PopcountBefore(Mask, Slot)];
            Node = Nodes[NodeIndex];

            BaseX += LocalX * ChildSize;
            BaseY += LocalY * ChildSize;
            BaseZ += LocalZ * ChildSize;
            NodeSize = ChildSize;
        }

        return false;
    }

    uint8 FVoxelWorld::MaterialAtVoxel(int32 X, int32 Y, int32 Z) const
    {
        if (!IsSolidVoxel(X, Y, Z))
        {
            return uint8(EMaterial::Air);
        }

        uint32 LeafNode = 0;
        int32 LeafBase[3] = { 0, 0, 0 };
        if (!FindLeafNode(X, Y, Z, LeafNode, LeafBase))
        {
            // A collapsed interior node carries one material across its whole span.
            return uint8(EMaterial::Stone);
        }

        const FVoxNode& Node = Nodes[LeafNode];
        const uint32 Slot = SlotIndex(X - LeafBase[0], Y - LeafBase[1], Z - LeafBase[2]);

        if ((Node.Flags & kFlagUniform) != 0u)
        {
            return uint8(Node.Palette & 0xFFu);
        }

        const uint32 Word = Payload[Node.PayloadBase + (Slot >> 4)];
        const uint32 Index = (Word >> ((Slot & 15u) * 2u)) & 3u;
        return uint8((Node.Palette >> (Index * 8u)) & 0xFFu);
    }

    bool FVoxelWorld::NodeOverlaps(uint32 NodeIndex, const int32 NodeMin[3], int32 NodeSize,
                                   const int32 Min[3], const int32 Max[3]) const
    {
        const FVoxNode& Node = Nodes[NodeIndex];

        if ((Node.Flags & kFlagPresent) == 0u)
        {
            return false;
        }

        if ((Node.Flags & kFlagSolid) != 0u)
        {
            return true;
        }

        const int32 ChildSize = NodeSize >> 3;
        const uint32* Mask = Masks.GetData() + NodeIndex * kMaskWords;
        const bool bLeaf = (Node.Flags & kFlagLeaf) != 0u;

        int32 Low[3];
        int32 High[3];
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            Low[Axis]  = Math::Clamp((Min[Axis] - NodeMin[Axis]) / ChildSize, 0, kNodeSide - 1);
            High[Axis] = Math::Clamp((Max[Axis] - NodeMin[Axis]) / ChildSize, 0, kNodeSide - 1);
        }

        for (int32 Y = Low[1]; Y <= High[1]; ++Y)
        {
            for (int32 Z = Low[2]; Z <= High[2]; ++Z)
            {
                for (int32 X = Low[0]; X <= High[0]; ++X)
                {
                    const uint32 Slot = SlotIndex(X, Y, Z);
                    if (!TestBit(Mask, Slot))
                    {
                        continue;
                    }

                    if (bLeaf)
                    {
                        return true;
                    }

                    const int32 ChildMin[3] =
                    {
                        NodeMin[0] + X * ChildSize,
                        NodeMin[1] + Y * ChildSize,
                        NodeMin[2] + Z * ChildSize,
                    };

                    const uint32 Child = ChildIndices[Node.ChildBase + PopcountBefore(Mask, Slot)];
                    if (NodeOverlaps(Child, ChildMin, ChildSize, Min, Max))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool FVoxelWorld::OverlapsBox(const int32 Min[3], const int32 Max[3]) const
    {
        if (Nodes.IsEmpty())
        {
            return false;
        }

        const int32 Limit[3] = { kWorldVoxelsX - 1, kWorldVoxelsY - 1, kWorldVoxelsZ - 1 };

        int32 Low[3];
        int32 High[3];
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (Max[Axis] < 0 || Min[Axis] > Limit[Axis])
            {
                return false;
            }
            Low[Axis]  = Math::Clamp(Min[Axis], 0, Limit[Axis]);
            High[Axis] = Math::Clamp(Max[Axis], 0, Limit[Axis]);
        }

        const int32 RootLow[3]  = { Low[0] >> 9, Low[1] >> 9, Low[2] >> 9 };
        const int32 RootHigh[3] = { High[0] >> 9, High[1] >> 9, High[2] >> 9 };

        for (int32 RY = RootLow[1]; RY <= RootHigh[1]; ++RY)
        {
            for (int32 RZ = RootLow[2]; RZ <= RootHigh[2]; ++RZ)
            {
                for (int32 RX = RootLow[0]; RX <= RootHigh[0]; ++RX)
                {
                    const int32 NodeMin[3] = { RX << 9, RY << 9, RZ << 9 };
                    const uint32 Root = uint32((RY * kRootCountZ + RZ) * kRootCountX + RX);

                    if (NodeOverlaps(Root, NodeMin, kVoxelsPerRoot, Low, High))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    void FVoxelWorld::CarveSphere(const FVector3& Center, float Radius)
    {
        if (Nodes.IsEmpty() || Radius <= 0.0f)
        {
            return;
        }

        const float CenterX = Center.x / kVoxelSize;
        const float CenterY = Center.y / kVoxelSize;
        const float CenterZ = Center.z / kVoxelSize;
        const float VoxelRadius = Radius / kVoxelSize;
        const float RadiusSquared = VoxelRadius * VoxelRadius;

        const int32 MaxCellX = (kWorldVoxelsX >> 3) - 1;
        const int32 MaxCellY = (kWorldVoxelsY >> 3) - 1;
        const int32 MaxCellZ = (kWorldVoxelsZ >> 3) - 1;

        // Whole leaf cells at a time, so the tree walk runs once per cell instead of once per voxel.
        const int32 CellX0 = Math::Clamp(int32(Math::Floor((CenterX - VoxelRadius) / 8.0f)), 0, MaxCellX);
        const int32 CellX1 = Math::Clamp(int32(Math::Floor((CenterX + VoxelRadius) / 8.0f)), 0, MaxCellX);
        const int32 CellY0 = Math::Clamp(int32(Math::Floor((CenterY - VoxelRadius) / 8.0f)), 0, MaxCellY);
        const int32 CellY1 = Math::Clamp(int32(Math::Floor((CenterY + VoxelRadius) / 8.0f)), 0, MaxCellY);
        const int32 CellZ0 = Math::Clamp(int32(Math::Floor((CenterZ - VoxelRadius) / 8.0f)), 0, MaxCellZ);
        const int32 CellZ1 = Math::Clamp(int32(Math::Floor((CenterZ + VoxelRadius) / 8.0f)), 0, MaxCellZ);

        for (int32 CellY = CellY0; CellY <= CellY1; ++CellY)
        {
            for (int32 CellZ = CellZ0; CellZ <= CellZ1; ++CellZ)
            {
                for (int32 CellX = CellX0; CellX <= CellX1; ++CellX)
                {
                    uint32 LeafNode = 0;
                    int32 LeafBase[3] = { 0, 0, 0 };
                    if (!FindLeafNode(CellX * 8 + 4, CellY * 8 + 4, CellZ * 8 + 4, LeafNode, LeafBase))
                    {
                        continue;
                    }

                    uint32* Mask = Masks.GetData() + LeafNode * kMaskWords;
                    bool bCleared = false;

                    for (int32 Y = 0; Y < kNodeSide; ++Y)
                    {
                        const float DY = float(LeafBase[1] + Y) + 0.5f - CenterY;
                        for (int32 Z = 0; Z < kNodeSide; ++Z)
                        {
                            const float DZ = float(LeafBase[2] + Z) + 0.5f - CenterZ;
                            for (int32 X = 0; X < kNodeSide; ++X)
                            {
                                const float DX = float(LeafBase[0] + X) + 0.5f - CenterX;
                                if (DX * DX + DY * DY + DZ * DZ >= RadiusSquared)
                                {
                                    continue;
                                }

                                const uint32 Slot = SlotIndex(X, Y, Z);
                                Mask[Slot >> 5] &= ~(1u << (Slot & 31u));
                                bCleared = true;
                            }
                        }
                    }

                    if (bCleared)
                    {
                        // A solid leaf ignores its mask, so it has to stop being solid to show the hole.
                        Nodes[LeafNode].Flags &= ~kFlagSolid;
                    }
                }
            }
        }
    }

    bool FVoxelWorld::Upload()
    {
        if (Nodes.IsEmpty())
        {
            return false;
        }

        if (ChildIndices.IsEmpty())
        {
            ChildIndices.Add(0u);
        }
        if (Payload.IsEmpty())
        {
            Payload.Add(0u);
        }

        NodeAlloc    = RHI::Malloc(Nodes.Num() * sizeof(FVoxNode), RHI::EMemoryType::Default);
        MaskAlloc    = RHI::Malloc(Masks.Num() * sizeof(uint32), RHI::EMemoryType::Default);
        PrefixAlloc  = RHI::Malloc(Prefix.Num() * sizeof(uint32), RHI::EMemoryType::Default);
        ChildAlloc   = RHI::Malloc(ChildIndices.Num() * sizeof(uint32), RHI::EMemoryType::Default);
        PayloadAlloc = RHI::Malloc(Payload.Num() * sizeof(uint32), RHI::EMemoryType::Default);

        if (NodeAlloc.Gpu == 0 || MaskAlloc.Gpu == 0 || PrefixAlloc.Gpu == 0
            || ChildAlloc.Gpu == 0 || PayloadAlloc.Gpu == 0)
        {
            LOG_ERROR("Grain: failed to allocate the voxel pools.");
            return false;
        }

        RHI::SetDebugName(NodeAlloc.Gpu, "Grain.Nodes");
        RHI::SetDebugName(MaskAlloc.Gpu, "Grain.Masks");
        RHI::SetDebugName(PrefixAlloc.Gpu, "Grain.Prefix");
        RHI::SetDebugName(ChildAlloc.Gpu, "Grain.Children");
        RHI::SetDebugName(PayloadAlloc.Gpu, "Grain.Payload");

        TVector<RHI::FGPUAllocation> Staging;
        const RHI::FCmdListH CL = RHI::OpenCommandList();

        auto Stage = [&Staging, CL](const RHI::FGPUAllocation& Dest, const void* Data, uint64 Size) -> bool
        {
            const RHI::FGPUAllocation Source = RHI::Malloc(Size, RHI::EMemoryType::CPUWrite);
            if (Source.Gpu == 0 || Source.Cpu == nullptr)
            {
                return false;
            }

            std::memcpy(Source.Cpu, Data, size_t(Size));
            RHI::CmdMemcpy(CL, { Dest.Gpu, size_t(Size) }, { Source.Gpu, size_t(Size) });
            Staging.Add(Source);
            return true;
        };

        const bool bStaged =
            Stage(NodeAlloc, Nodes.GetData(), Nodes.Num() * sizeof(FVoxNode)) &&
            Stage(MaskAlloc, Masks.GetData(), Masks.Num() * sizeof(uint32)) &&
            Stage(PrefixAlloc, Prefix.GetData(), Prefix.Num() * sizeof(uint32)) &&
            Stage(ChildAlloc, ChildIndices.GetData(), ChildIndices.Num() * sizeof(uint32)) &&
            Stage(PayloadAlloc, Payload.GetData(), Payload.Num() * sizeof(uint32));

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Compute | RHI::EStageFlags::PixelShader | RHI::EStageFlags::VertexShader,
            RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::IndexRead);
        const uint64 Value = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), Value);

        for (const RHI::FGPUAllocation& Source : Staging)
        {
            RHI::Retire(Source);
        }

        if (!bStaged)
        {
            LOG_ERROR("Grain: failed to stage the voxel pools.");
            return false;
        }

        return true;
    }

    void FVoxelWorld::Release()
    {
        for (RHI::FGPUAllocation* Allocation : { &NodeAlloc, &MaskAlloc, &PrefixAlloc,
                                                 &ChildAlloc, &PayloadAlloc })
        {
            if (Allocation->Gpu != 0)
            {
                RHI::Retire(*Allocation);
                *Allocation = {};
            }
        }
    }
}
