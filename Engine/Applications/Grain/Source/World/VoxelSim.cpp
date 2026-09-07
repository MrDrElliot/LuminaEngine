#include "VoxelSim.h"

#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RHICore.h"
#include "TaskSystem/TaskSystem.h"

#include <cstring>

namespace Grain
{
    bool FVoxelSim::Initialize(const FVoxelWorld& World, const FVector3& Center)
    {
        const double Started = PlatformTime::Seconds();

        const int32 HalfSide = kSimSide / 2;
        const int32 BaseX = Math::Clamp(int32(Center.x / kVoxelSize) - HalfSide, 0, kWorldVoxelsX - kSimSide);
        const int32 BaseY = Math::Clamp(int32(Center.y / kVoxelSize) - HalfSide, 0, kWorldVoxelsY - kSimSide);
        const int32 BaseZ = Math::Clamp(int32(Center.z / kVoxelSize) - HalfSide, 0, kWorldVoxelsZ - kSimSide);

        OriginVoxels = { float(BaseX), float(BaseY), float(BaseZ) };

        TVector<uint32> Grid;
        Grid.resize(size_t(kSimSide) * size_t(kSimSide) * size_t(kSimSide));

        Task::ParallelFor(uint32(kSimSide), [&](const Task::FParallelRange& Range)
        {
            for (uint32 Y = Range.Start; Y < Range.End; ++Y)
            {
                for (int32 Z = 0; Z < kSimSide; ++Z)
                {
                    const size_t RowBase = (size_t(Y) * size_t(kSimSide) + size_t(Z)) * size_t(kSimSide);
                    for (int32 X = 0; X < kSimSide; ++X)
                    {
                        const uint32 Material = World.SampleVoxelPublic(BaseX + X, BaseY + int32(Y), BaseZ + Z);

                        // Water seeds as volume rather than material, so it can flow and half fill a cell.
                        Grid[RowBase + size_t(X)] = Material == uint32(EMaterial::Water)
                            ? (kMassFull << 8)
                            : Material;
                    }
                }
            }
        }, 4);

        // The spring sits over the highest ground in the volume, so the flow has somewhere to run.
        int32 BestX = kSimSide / 2;
        int32 BestZ = kSimSide / 2;
        float BestHeight = -1e9f;

        for (int32 Z = 24; Z < kSimSide - 24; Z += 4)
        {
            for (int32 X = 24; X < kSimSide - 24; X += 4)
            {
                const float Height = World.SampleHeight(float(BaseX + X) * kVoxelSize, float(BaseZ + Z) * kVoxelSize);
                if (Height > BestHeight && Height < float(BaseY + kSimSide - 20) * kVoxelSize)
                {
                    BestHeight = Height;
                    BestX = X;
                    BestZ = Z;
                }
            }
        }

        // The run has to be aimed at, so the steepest descent around the spring is recorded here.
        {
            const float WorldX = float(BaseX + BestX) * kVoxelSize;
            const float WorldZ = float(BaseZ + BestZ) * kVoxelSize;
            const float Reach = 3.0f;

            const float DX = World.SampleHeight(WorldX + Reach, WorldZ) - World.SampleHeight(WorldX - Reach, WorldZ);
            const float DZ = World.SampleHeight(WorldX, WorldZ + Reach) - World.SampleHeight(WorldX, WorldZ - Reach);

            const float Length = Math::Sqrt(DX * DX + DZ * DZ);
            Downhill = Length > 0.001f ? FVector3{ -DX / Length, 0.0f, -DZ / Length }
                                       : FVector3{ 1.0f, 0.0f, 0.0f };
        }

        const int32 SourceY = Math::Clamp(int32(BestHeight / kVoxelSize) - BaseY + 10, 4, kSimSide - 4);
        SourceVoxels = { float(BestX), float(SourceY), float(BestZ) };
        SourceMaterial = uint32(EMaterial::Water);

        const uint64 GridBytes = Grid.size() * sizeof(uint32);
        const uint64 CoarseBytes = uint64(kSimCoarseSide) * kSimCoarseSide * kSimCoarseSide * sizeof(uint32);

        GridAlloc = RHI::Malloc(GridBytes, RHI::EMemoryType::Default);
        CoarseAlloc = RHI::Malloc(CoarseBytes, RHI::EMemoryType::Default);

        if (GridAlloc.Gpu == 0 || CoarseAlloc.Gpu == 0)
        {
            LOG_ERROR("Grain: failed to allocate the simulation volume.");
            return false;
        }

        RHI::SetDebugName(GridAlloc.Gpu, "Grain.SimGrid");
        RHI::SetDebugName(CoarseAlloc.Gpu, "Grain.SimCoarse");

        const RHI::FGPUAllocation Staging = RHI::Malloc(GridBytes, RHI::EMemoryType::CPUWrite);
        if (Staging.Gpu == 0 || Staging.Cpu == nullptr)
        {
            LOG_ERROR("Grain: failed to stage the simulation volume.");
            return false;
        }

        std::memcpy(Staging.Cpu, Grid.data(), size_t(GridBytes));

        const RHI::FCmdListH CL = RHI::OpenCommandList();
        RHI::CmdMemcpy(CL, { GridAlloc.Gpu, size_t(GridBytes) }, { Staging.Gpu, size_t(GridBytes) });
        RHI::CmdMemzero(CL, { CoarseAlloc.Gpu, CoarseBytes });
        RHI::Barriers::TransferToAll(CL);
        const uint64 Value = RHI::Submit(RHI::EQueueType::Graphics, TSpan<const RHI::FCmdListH>{&CL, 1});
        RHI::WaitSemaphore(RHI::GetQueueTimeline(RHI::EQueueType::Graphics), Value);

        RHI::Retire(Staging);

        LOG_INFO("Grain: sim volume {:.1f} m at ({}, {}, {}) voxels, spring at ground {:.1f} m, {:.2f} s.",
            kSimExtent, BaseX, BaseY, BaseZ, BestHeight, PlatformTime::Seconds() - Started);

        return true;
    }

    void FVoxelSim::Release()
    {
        if (GridAlloc.Gpu != 0)
        {
            RHI::Retire(GridAlloc);
            GridAlloc = {};
        }
        if (CoarseAlloc.Gpu != 0)
        {
            RHI::Retire(CoarseAlloc);
            CoarseAlloc = {};
        }
    }
}
