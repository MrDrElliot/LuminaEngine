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
    void SFoliageTerrainSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
    }

    namespace FoliageTerrainSystemInternal
    {
        // A terrain's edited region, converted to a world-space XZ rect (with a one-cell margin).
        struct FTerrainDirty
        {
            ECS::FEntity Entity;
            const STerrainComponent* Terrain = nullptr;
            FVector3 Origin = FVector3(0.0f);
            bool      bHasRect = false;
            float     MinX = 0.0f, MaxX = 0.0f, MinZ = 0.0f, MaxZ = 0.0f;
        };
    }

    void SFoliageTerrainSystem::OnUpdate(const SSystemContext&)
    {
        using FoliageTerrainSystemInternal::FTerrainDirty;

        FTerrainDirty Dirties[8];
        int32 NumTerrains = 0;

        Terrains.ForEach([&](ECS::FEntity Entity, const STerrainComponent& Terrain, const STransformComponent& Transform)
        {
            const FIntVector2 Min = Terrain.CPUState.FoliageDirtyMin;
            const FIntVector2 Max = Terrain.CPUState.FoliageDirtyMax;
            if (NumTerrains >= 8 || Max.x < Min.x || Max.y < Min.y)
            {
                return;
            }

            FTerrainDirty& D = Dirties[NumTerrains++];
            D.Entity  = Entity;
            D.Terrain = &Terrain;
            D.Origin  = Transform.GetWorldLocation();

            if (Terrain.Resolution > 1)
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

        FoliageSets.ForEach([&](ECS::FEntity Entity, SFoliageComponent& Foliage)
        {
            if (Foliage.Instances.IsEmpty())
            {
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
                    const FTerrainDirty& D = Dirties[t];
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

            const uint32 NumInstances = (uint32)Foliage.Instances.Num();

            constexpr uint32 kParallelThreshold = 2048;
            Jobs::ParallelForOrSerial(NumInstances, kParallelThreshold, Reproject, 256);

            if (bMovedAny.load(std::memory_order_relaxed))
            {
                Foliage.MarkInstancesChanged();

                DirtyTracker->Mark(Entity, EPrimitiveSource::Foliage, EPrimitiveDirty::Data);
            }
        });

        // Clear consumed dirty regions (after every foliage component saw them this frame).
        for (int32 t = 0; t < NumTerrains; ++t)
        {
            STerrainComponent& Terrain = TerrainResets.Get<STerrainComponent>(Dirties[t].Entity);
            Terrain.CPUState.FoliageDirtyMin = FIntVector2(INT32_MAX);
            Terrain.CPUState.FoliageDirtyMax = FIntVector2(INT32_MIN);
        }
    }
}
