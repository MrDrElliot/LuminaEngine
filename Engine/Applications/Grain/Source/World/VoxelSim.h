#pragma once

#include "VoxelWorld.h"
#include "Renderer/RHI.h"

namespace Grain
{
    // Voxels per axis of the simulation volume, which is dense so a cell can move without allocating.
    inline constexpr int32 kSimSide       = 256;
    inline constexpr int32 kSimCoarseStep = 8;
    inline constexpr int32 kSimCoarseSide = kSimSide / kSimCoarseStep;

    inline constexpr float kSimExtent = float(kSimSide) * kVoxelSize;

    // Mirrored in the shader's kVoxelCommon block.
    inline constexpr uint32 kMassFull = 4096;

    class FVoxelSim
    {
    public:

        bool Initialize(const FVoxelWorld& World, const FVector3& Center);
        void Release();

        NODISCARD RHI::GPUPtr GetGridAddress() const { return GridAlloc.Gpu; }
        NODISCARD RHI::GPUPtr GetCoarseAddress() const { return CoarseAlloc.Gpu; }
        NODISCARD const FVector3& GetOriginVoxels() const { return OriginVoxels; }
        NODISCARD bool IsValid() const { return GridAlloc.Gpu != 0; }

        NODISCARD FVector3 GetSpringWorld() const
        {
            return { (OriginVoxels.x + SourceVoxels.x) * kVoxelSize,
                     (OriginVoxels.y + SourceVoxels.y) * kVoxelSize,
                     (OriginVoxels.z + SourceVoxels.z) * kVoxelSize };
        }

        NODISCARD FVector3 GetSourceVoxels() const { return SourceVoxels; }
        NODISCARD const FVector3& GetDownhill() const { return Downhill; }
        NODISCARD uint32 GetSourceMaterial() const { return SourceMaterial; }

    private:

        RHI::FGPUAllocation GridAlloc;
        RHI::FGPUAllocation CoarseAlloc;

        FVector3 OriginVoxels { 0.0f, 0.0f, 0.0f };
        FVector3 SourceVoxels { 0.0f, 0.0f, 0.0f };
        FVector3 Downhill { 1.0f, 0.0f, 0.0f };
        uint32   SourceMaterial = 0;
    };
}
