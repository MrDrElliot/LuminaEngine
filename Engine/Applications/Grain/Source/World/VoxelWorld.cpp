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
        Columns.resize(size_t(kWorldVoxelsX) * size_t(kWorldVoxelsZ));

        const uint32 LocalSeed = Seed;

        Task::ParallelFor(uint32(kWorldVoxelsZ), [this, LocalSeed](const Task::FParallelRange& Range)
        {
            for (uint32 Z = Range.Start; Z < Range.End; ++Z)
            {
                FSurface* Row = Columns.data() + size_t(Z) * size_t(kWorldVoxelsX);
                for (int32 X = 0; X < kWorldVoxelsX; ++X)
                {
                    Row[X].Height = TerrainHeight(float(X) * kVoxelSize, float(Z) * kVoxelSize, LocalSeed);
                }
            }
        }, 8);

        // Slope needs both neighbors, so surface materials are a second pass over the finished heights.
        Task::ParallelFor(uint32(kWorldVoxelsZ), [this](const Task::FParallelRange& Range)
        {
            for (uint32 Z = Range.Start; Z < Range.End; ++Z)
            {
                const size_t RowBase = size_t(Z) * size_t(kWorldVoxelsX);
                const size_t RowPrev = size_t(Math::Max(int32(Z) - 4, 0)) * size_t(kWorldVoxelsX);
                const size_t RowNext = size_t(Math::Min(int32(Z) + 4, kWorldVoxelsZ - 1)) * size_t(kWorldVoxelsX);

                for (int32 X = 0; X < kWorldVoxelsX; ++X)
                {
                    FSurface& Out = Columns[RowBase + size_t(X)];

                    const int32 XPrev = Math::Max(X - 4, 0);
                    const int32 XNext = Math::Min(X + 4, kWorldVoxelsX - 1);

                    const float DX = Columns[RowBase + size_t(XNext)].Height - Columns[RowBase + size_t(XPrev)].Height;
                    const float DZ = Columns[RowNext + size_t(X)].Height - Columns[RowPrev + size_t(X)].Height;

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

        CoarseMin.assign(size_t(kCoarseX) * size_t(kCoarseZ), 0.0f);
        CoarseMax.assign(size_t(kCoarseX) * size_t(kCoarseZ), 0.0f);

        Task::ParallelFor(uint32(kCoarseZ), [this](const Task::FParallelRange& Range)
        {
            for (uint32 CZ = Range.Start; CZ < Range.End; ++CZ)
            {
                for (int32 CX = 0; CX < kCoarseX; ++CX)
                {
                    float MinH = 1e9f;
                    float MaxH = -1e9f;

                    for (int32 Z = 0; Z < kCoarseVoxels; ++Z)
                    {
                        const size_t RowBase = size_t(int32(CZ) * kCoarseVoxels + Z) * size_t(kWorldVoxelsX);
                        for (int32 X = 0; X < kCoarseVoxels; ++X)
                        {
                            const float H = Columns[RowBase + size_t(CX * kCoarseVoxels + X)].Height;
                            MinH = Math::Min(MinH, H);
                            MaxH = Math::Max(MaxH, H);
                        }
                    }

                    CoarseMin[size_t(CZ) * size_t(kCoarseX) + size_t(CX)] = MinH;
                    CoarseMax[size_t(CZ) * size_t(kCoarseX) + size_t(CX)] = MaxH;
                }
            }
        }, 1);
    }

    void FVoxelWorld::ScatterTrees()
    {
        Trees.assign(size_t(kTreeCellsX) * size_t(kTreeCellsZ), FTree{});

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

                FTree& Tree = Trees[size_t(CZ) * size_t(kTreeCellsX) + size_t(CX)];
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
        return Columns[size_t(CZ) * size_t(kWorldVoxelsX) + size_t(CX)];
    }

    float FVoxelWorld::SampleHeight(float WorldX, float WorldZ) const
    {
        if (Columns.empty())
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

                const FTree& Tree = Trees[size_t(CZ) * size_t(kTreeCellsX) + size_t(CX)];
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

        const uint32 NodeIndex = uint32(Buffer.Nodes.size());
        Buffer.Nodes.emplace_back();
        Buffer.Masks.resize(Buffer.Masks.size() + size_t(kMaskWords));

        uint32* NodeMask = Buffer.Masks.data() + size_t(NodeIndex) * size_t(kMaskWords);
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
        Node.PayloadBase = uint32(Buffer.Payload.size());

        constexpr int32 kPayloadWords = kNodeSlots / 16;
        Buffer.Payload.resize(Buffer.Payload.size() + size_t(kPayloadWords), 0u);
        uint32* Words = Buffer.Payload.data() + size_t(Node.PayloadBase);

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
                MinH = Math::Min(MinH, CoarseMin[size_t(CZ) * size_t(kCoarseX) + size_t(CX)]);
                MaxH = Math::Max(MaxH, CoarseMax[size_t(CZ) * size_t(kCoarseX) + size_t(CX)]);
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
            const uint32 SolidIndex = uint32(Buffer.Nodes.size());
            Buffer.Nodes.emplace_back();
            Buffer.Masks.resize(Buffer.Masks.size() + size_t(kMaskWords), 0xFFFFFFFFu);

            Buffer.Nodes[SolidIndex].Flags   = kFlagPresent | kFlagSolid | kFlagUniform;
            Buffer.Nodes[SolidIndex].Palette = uint32(EMaterial::Stone);
            ++Buffer.SolidNodes;
            return SolidIndex;
        }

        uint32 ChildIndex[kNodeSlots];
        uint32 Mask[kMaskWords] = {};
        uint32 Present = 0;

        // A run of children that all collapse can be truncated, because a solid child appends one record.
        const size_t NodesBefore = Buffer.Nodes.size();
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
            Buffer.Nodes.resize(NodesBefore);
            Buffer.Masks.resize(NodesBefore * size_t(kMaskWords));
            Buffer.SolidNodes -= kNodeSlots;

            const uint32 CollapsedIndex = uint32(Buffer.Nodes.size());
            Buffer.Nodes.emplace_back();
            Buffer.Masks.resize(Buffer.Masks.size() + size_t(kMaskWords), 0xFFFFFFFFu);

            Buffer.Nodes[CollapsedIndex].Flags   = kFlagPresent | kFlagSolid | kFlagUniform;
            Buffer.Nodes[CollapsedIndex].Palette = SharedMaterial;
            ++Buffer.SolidNodes;
            return CollapsedIndex;
        }

        // Child records stay wherever the recursion put them, so the parent points at a compacted index run.
        const uint32 ChildBase = uint32(Buffer.ChildIndices.size());
        for (int32 Slot = 0; Slot < kNodeSlots; ++Slot)
        {
            if (ChildIndex[Slot] != ~0u)
            {
                Buffer.ChildIndices.push_back(ChildIndex[Slot]);
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

        const uint32 NodeIndex = uint32(Buffer.Nodes.size());
        Buffer.Nodes.emplace_back();
        Buffer.Masks.resize(Buffer.Masks.size() + size_t(kMaskWords));

        Buffer.Nodes[NodeIndex].ChildBase = ChildBase;
        Buffer.Nodes[NodeIndex].Palette   = Dominant;
        Buffer.Nodes[NodeIndex].Flags     = kFlagPresent;

        uint32* NodeMask = Buffer.Masks.data() + size_t(NodeIndex) * size_t(kMaskWords);
        for (int32 i = 0; i < kMaskWords; ++i)
        {
            NodeMask[i] = Mask[i];
        }

        ++Buffer.MidNodes;
        return NodeIndex;
    }

    void FVoxelWorld::Merge(TVector<FBuildBuffer>& Buffers, const TVector<uint32>& RootLocal)
    {
        Nodes.assign(size_t(kRootCount), FVoxNode{});
        Masks.assign(size_t(kRootCount) * size_t(kMaskWords), 0u);
        ChildIndices.clear();
        Payload.clear();

        for (int32 Root = 0; Root < kRootCount; ++Root)
        {
            FBuildBuffer& Buffer = Buffers[size_t(Root)];

            Stats.LeafNodes    += Buffer.LeafNodes;
            Stats.MidNodes     += Buffer.MidNodes;
            Stats.SolidNodes   += Buffer.SolidNodes;
            Stats.PaletteNodes += Buffer.PaletteNodes;

            if (RootLocal[size_t(Root)] == ~0u)
            {
                continue;
            }

            const uint32 NodeOffset    = uint32(Nodes.size());
            const uint32 ChildOffset   = uint32(ChildIndices.size());
            const uint32 PayloadOffset = uint32(Payload.size());

            for (uint32 Index : Buffer.ChildIndices)
            {
                ChildIndices.push_back(Index + NodeOffset);
            }
            Payload.insert(Payload.end(), Buffer.Payload.begin(), Buffer.Payload.end());

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
                Nodes.push_back(Node);
            }

            Masks.insert(Masks.end(), Buffer.Masks.begin(), Buffer.Masks.end());

            // The root slot holds a copy of its own subtree root, so the shader can index the grid directly.
            const uint32 LocalRoot = RootLocal[size_t(Root)];
            Nodes[size_t(Root)] = Nodes[size_t(NodeOffset + LocalRoot)];

            const uint32* Source = Masks.data() + size_t(NodeOffset + LocalRoot) * size_t(kMaskWords);
            uint32* Dest = Masks.data() + size_t(Root) * size_t(kMaskWords);
            for (int32 i = 0; i < kMaskWords; ++i)
            {
                Dest[i] = Source[i];
            }

            Buffer = FBuildBuffer{};
        }
    }

    // Derived from the finished masks rather than threaded through the build, and only interior nodes
    // are ever indexed by rank, so digging out voxel bits never invalidates it.
    void FVoxelWorld::BuildRankPrefix()
    {
        const size_t NodeCount = Nodes.size();
        Prefix.assign(NodeCount * 8, 0u);

        Task::ParallelFor(uint32(NodeCount), [this](const Task::FParallelRange& Range)
        {
            for (uint32 Node = Range.Start; Node < Range.End; ++Node)
            {
                const uint32* Words = Masks.data() + size_t(Node) * size_t(kMaskWords);
                uint32* Out = Prefix.data() + size_t(Node) * 8;

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
        Buffers.resize(size_t(kRootCount));

        TVector<uint32> RootLocal;
        RootLocal.assign(size_t(kRootCount), ~0u);

        Task::ParallelFor(uint32(kRootCount), [this, &Buffers, &RootLocal](const Task::FParallelRange& Range)
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

        Stats.NodeBytes    = Nodes.size() * sizeof(FVoxNode);
        Stats.MaskBytes    = Masks.size() * sizeof(uint32);
        Stats.ChildBytes   = ChildIndices.size() * sizeof(uint32);
        Stats.PayloadBytes = Payload.size() * sizeof(uint32);
        Stats.BuildSeconds = PlatformTime::Seconds() - Started;

        LOG_INFO("Grain: world {} x {} x {} voxels at {:.1f} cm.",
            kWorldVoxelsX, kWorldVoxelsY, kWorldVoxelsZ, kVoxelSize * 100.0f);
        LOG_INFO("Grain: built in {:.2f} s ({:.2f} s heightfield). {} leaf, {} mid, {} solid, {} palette.",
            Stats.BuildSeconds, Heightfield - Started,
            Stats.LeafNodes, Stats.MidNodes, Stats.SolidNodes, Stats.PaletteNodes);
        LOG_INFO("Grain: {:.1f} MB nodes, {:.1f} MB masks, {:.1f} MB rank, {:.1f} MB child, {:.1f} MB payload.",
            double(Stats.NodeBytes) / 1048576.0, double(Stats.MaskBytes) / 1048576.0,
            double(Prefix.size() * sizeof(uint32)) / 1048576.0,
            double(Stats.ChildBytes) / 1048576.0, double(Stats.PayloadBytes) / 1048576.0);
    }

    bool FVoxelWorld::Upload()
    {
        if (Nodes.empty())
        {
            return false;
        }

        if (ChildIndices.empty())
        {
            ChildIndices.push_back(0u);
        }
        if (Payload.empty())
        {
            Payload.push_back(0u);
        }

        NodeAlloc    = RHI::Malloc(Nodes.size() * sizeof(FVoxNode), RHI::EMemoryType::Default);
        MaskAlloc    = RHI::Malloc(Masks.size() * sizeof(uint32), RHI::EMemoryType::Default);
        PrefixAlloc  = RHI::Malloc(Prefix.size() * sizeof(uint32), RHI::EMemoryType::Default);
        ChildAlloc   = RHI::Malloc(ChildIndices.size() * sizeof(uint32), RHI::EMemoryType::Default);
        PayloadAlloc = RHI::Malloc(Payload.size() * sizeof(uint32), RHI::EMemoryType::Default);

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
            Staging.push_back(Source);
            return true;
        };

        const bool bStaged =
            Stage(NodeAlloc, Nodes.data(), Nodes.size() * sizeof(FVoxNode)) &&
            Stage(MaskAlloc, Masks.data(), Masks.size() * sizeof(uint32)) &&
            Stage(PrefixAlloc, Prefix.data(), Prefix.size() * sizeof(uint32)) &&
            Stage(ChildAlloc, ChildIndices.data(), ChildIndices.size() * sizeof(uint32)) &&
            Stage(PayloadAlloc, Payload.data(), Payload.size() * sizeof(uint32));

        RHI::Barriers::TransferToAll(CL);
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
