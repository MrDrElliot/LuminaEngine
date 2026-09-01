#pragma once

#include "VoxelTypes.h"
#include "Containers/Vector.h"
#include "Renderer/RHI.h"

namespace Grain
{
    struct FWorldStats
    {
        uint32 LeafNodes    = 0;
        uint32 MidNodes     = 0;
        uint32 SolidNodes   = 0;
        uint32 PaletteNodes = 0;
        uint64 NodeBytes    = 0;
        uint64 MaskBytes    = 0;
        uint64 ChildBytes   = 0;
        uint64 PayloadBytes = 0;
        double BuildSeconds = 0.0;
    };

    struct FSurface
    {
        float  Height     = 0.0f;
        // Vertical depth understates the skin on a slope, so layer depths scale by this.
        float  SlopeScale = 1.0f;
        uint8  Surface    = 0;
        uint8  SubSurface = 0;
        uint16 Pad        = 0;
    };

    struct FTree
    {
        int32 VoxelX = 0;
        int32 VoxelZ = 0;
        int32 BaseY  = 0;
        int32 Trunk  = 0;
        float Radius = 0.0f;
        bool  bValid = false;
    };

    // One root's subtree, built without touching any other root so the roots can be built in parallel.
    struct FBuildBuffer
    {
        TVector<FVoxNode> Nodes;
        TVector<uint32>   Masks;
        TVector<uint32>   ChildIndices;
        TVector<uint32>   Payload;

        uint32 LeafNodes    = 0;
        uint32 MidNodes     = 0;
        uint32 SolidNodes   = 0;
        uint32 PaletteNodes = 0;
    };

    class FVoxelWorld
    {
    public:

        void Generate(uint32 InSeed);
        bool Upload();
        void Release();

        NODISCARD RHI::GPUPtr GetNodeAddress() const { return NodeAlloc.Gpu; }
        NODISCARD RHI::GPUPtr GetMaskAddress() const { return MaskAlloc.Gpu; }
        NODISCARD RHI::GPUPtr GetPrefixAddress() const { return PrefixAlloc.Gpu; }
        NODISCARD RHI::GPUPtr GetChildAddress() const { return ChildAlloc.Gpu; }
        NODISCARD RHI::GPUPtr GetPayloadAddress() const { return PayloadAlloc.Gpu; }

        NODISCARD const FWorldStats& GetStats() const { return Stats; }
        NODISCARD float SampleHeight(float WorldX, float WorldZ) const;

        // Scans the cave band for an open pocket, so a screenshot can actually stand in one.
        NODISCARD bool FindCavePosition(FVector3& OutPosition) const;

        // The simulation volume seeds itself from the same generator the tree was built from.
        NODISCARD uint32 SampleVoxelPublic(int32 X, int32 Y, int32 Z) const
        {
            const bool bInside = X >= 0 && Y >= 0 && Z >= 0
                && X < kWorldVoxelsX && Y < kWorldVoxelsY && Z < kWorldVoxelsZ;
            return bInside ? uint32(SampleVoxel(X, Y, Z)) : 0u;
        }

    private:

        uint32 BuildNode(FBuildBuffer& Buffer, int32 Level, int32 BaseX, int32 BaseY, int32 BaseZ) const;
        uint32 BuildLeaf(FBuildBuffer& Buffer, int32 BaseX, int32 BaseY, int32 BaseZ) const;

        NODISCARD uint8 SampleVoxel(int32 X, int32 Y, int32 Z) const;
        NODISCARD uint8 SampleTree(int32 X, int32 Y, int32 Z) const;
        NODISCARD const FSurface& ColumnAt(int32 X, int32 Z) const;

        void BuildHeightfield();
        void ScatterTrees();
        void Merge(TVector<FBuildBuffer>& Buffers, const TVector<uint32>& RootLocal);
        void BuildRankPrefix();

        TVector<FSurface> Columns;
        TVector<FTree>    Trees;
        TVector<float>    CoarseMin;
        TVector<float>    CoarseMax;

        TVector<FVoxNode> Nodes;
        TVector<uint32>   Masks;
        TVector<uint32>   Prefix;
        TVector<uint32>   ChildIndices;
        TVector<uint32>   Payload;

        RHI::FGPUAllocation NodeAlloc;
        RHI::FGPUAllocation MaskAlloc;
        RHI::FGPUAllocation PrefixAlloc;
        RHI::FGPUAllocation ChildAlloc;
        RHI::FGPUAllocation PayloadAlloc;

        FWorldStats Stats;
        uint32      Seed = 0;
    };
}
