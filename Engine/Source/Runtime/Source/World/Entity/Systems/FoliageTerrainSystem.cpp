#include "RuntimePCH.h"
#include "FoliageTerrainSystem.h"
#include "SystemContext.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/TerrainSculptSystem.h"

namespace Lumina
{
    FSystemAccess SFoliageTerrainSystem::Access = FSystemAccess{}
        .Read<STransformComponent>()
        .Write<STerrainComponent, SFoliageComponent>();

    void SFoliageTerrainSystem::Startup(const FSystemContext& Context) noexcept
    {
        (void)Context.CreateView<STerrainComponent, STransformComponent>();
    }

    namespace
    {
        // A terrain's edited region, converted to a world-space XZ rect (with a one-cell margin).
        struct FTerrainDirty
        {
            STerrainComponent* Terrain = nullptr;
            FVector3 Origin = FVector3(0.0f);
            bool      bHasRect = false;
            float     MinX = 0.0f, MaxX = 0.0f, MinZ = 0.0f, MaxZ = 0.0f;
        };
    }

    void SFoliageTerrainSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        // Snapshot of terrain origin, accumulated dirty world-rect, and the running version sum.
        FTerrainDirty Terrains[8];
        int32 NumTerrains = 0;
        uint32 CombinedVersion = 0;

        Context.CreateView<STerrainComponent, STransformComponent>().each(
            [&](STerrainComponent& Terrain, const STransformComponent& Transform)
            {
                CombinedVersion += Terrain.CPUState.HeightmapVersion;
                if (NumTerrains >= 8)
                {
                    return;
                }

                FTerrainDirty& D = Terrains[NumTerrains++];
                D.Terrain = &Terrain;
                D.Origin  = Transform.GetWorldLocation();

                const FIntVector2 Min = Terrain.CPUState.FoliageDirtyMin;
                const FIntVector2 Max = Terrain.CPUState.FoliageDirtyMax;
                if (Max.x >= Min.x && Max.y >= Min.y && Terrain.Resolution > 1)
                {
                    const float HalfSize = Terrain.TileWorldSize * 0.5f;
                    const float Stride   = Terrain.TileWorldSize / float(Terrain.Resolution - 1);
                    const float Ox = D.Origin.x - HalfSize;
                    const float Oz = D.Origin.z - HalfSize;
                    D.MinX = Ox + float(Min.x) * Stride - Stride;
                    D.MaxX = Ox + float(Max.x) * Stride + Stride;
                    D.MinZ = Oz + float(Min.y) * Stride - Stride;
                    D.MaxZ = Oz + float(Max.y) * Stride + Stride;
                    D.bHasRect = true;
                }
            });

        if (NumTerrains == 0)
        {
            return;
        }

        // Re-project each foliage component whose synced version is stale.
        Context.CreateView<SFoliageComponent>().each(
            [&](SFoliageComponent& Foliage)
            {
                if (Foliage.LastTerrainVersion == CombinedVersion || Foliage.Instances.empty())
                {
                    Foliage.LastTerrainVersion = CombinedVersion;
                    return;
                }

                // Each instance reads the shared terrains and writes only its own Position.y.
                std::atomic<bool> bMovedAny{ false };

                const auto Reproject = [&](uint32 Index)
                {
                    SFoliageInstance& Inst = Foliage.Instances[Index];
                    if (!Foliage.IsValidType(Inst.TypeIndex))
                    {
                        return;
                    }
                    const SFoliageType& Type = Foliage.Types[Inst.TypeIndex];
                    if (!Type.bFollowTerrain)
                    {
                        return;
                    }

                    for (int32 t = 0; t < NumTerrains; ++t)
                    {
                        const FTerrainDirty& D = Terrains[t];
                        if (!D.bHasRect)
                        {
                            continue;
                        }
                        if (Inst.Position.x < D.MinX || Inst.Position.x > D.MaxX ||
                            Inst.Position.z < D.MinZ || Inst.Position.z > D.MaxZ)
                        {
                            continue;
                        }

                        float Height = 0.0f;
                        if (FTerrainSculptSystem::SampleHeight(*D.Terrain, D.Origin, Inst.Position.x, Inst.Position.z, Height))
                        {
                            const float NewY = Height + Type.ZOffset;
                            if (NewY != Inst.Position.y)
                            {
                                Inst.Position.y = NewY;
                                bMovedAny.store(true, std::memory_order_relaxed);
                            }
                            break;
                        }
                    }
                };

                const uint32 NumInstances = (uint32)Foliage.Instances.size();

                constexpr uint32 kParallelThreshold = 2048;
                if (NumInstances < kParallelThreshold || GTaskSystem == nullptr)
                {
                    for (uint32 i = 0; i < NumInstances; ++i)
                    {
                        Reproject(i);
                    }
                }
                else
                {
                    Task::ParallelFor(NumInstances, Reproject, 256);
                }

                Foliage.LastTerrainVersion = CombinedVersion;
                if (bMovedAny.load(std::memory_order_relaxed))
                {
                    // Invalidate the render cache so the moved instances rebake (transform/bounds changed).
                    Foliage.MarkInstancesChanged();
                }
            });

        // Clear consumed dirty regions (after every foliage component saw them this frame).
        for (int32 t = 0; t < NumTerrains; ++t)
        {
            Terrains[t].Terrain->CPUState.FoliageDirtyMin = FIntVector2(INT32_MAX);
            Terrains[t].Terrain->CPUState.FoliageDirtyMax = FIntVector2(INT32_MIN);
        }
    }
}
