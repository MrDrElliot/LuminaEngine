#pragma once

#include "ScenePrimitiveSet.h"
#include "SceneCullMath.h"
#include "Renderer/MeshletHeaderSlab.h"
#include "Memory/Memcpy.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include <immintrin.h>

namespace Lumina
{
    // Every pool a sync reads, resolved once per Sync rather than once per entity.
    struct FScenePrimitiveSet::FSyncPools
    {
        ECS::TComponentStorage<STransformComponent>     Transform    = {};
        ECS::TComponentStorage<SStaticMeshComponent>    StaticMesh   = {};
        ECS::TComponentStorage<SSkeletalMeshComponent>  SkeletalMesh = {};
        ECS::TComponentStorage<SDynamicMeshComponent>   DynamicMesh  = {};
        ECS::TComponentStorage<SFoliageComponent>       Foliage      = {};
        const ECS::FSparseSet*                          Disabled     = nullptr;
        const ECS::FSparseSet*                          Sources[kLinkedSources] = {};

        FMeshResolveCache*                              ResolveCache = &FMeshResolveCache::Get();

        explicit FSyncPools(ECS::FRegistry& Registry)
            : Transform(Registry.GetStorage<STransformComponent>())
            , StaticMesh(Registry.GetStorage<SStaticMeshComponent>())
            , SkeletalMesh(Registry.GetStorage<SSkeletalMeshComponent>())
            , DynamicMesh(Registry.GetStorage<SDynamicMeshComponent>())
            , Foliage(Registry.GetStorage<SFoliageComponent>())
            , Disabled(Registry.GetStorage<SDisabledTag>().GetSet())
        {
            Sources[(uint32)EPrimitiveSource::StaticMesh]   = StaticMesh.GetSet();
            Sources[(uint32)EPrimitiveSource::DynamicMesh]  = DynamicMesh.GetSet();
            Sources[(uint32)EPrimitiveSource::SkeletalMesh] = SkeletalMesh.GetSet();
        }
    };

    namespace ScenePrimitiveSetInternal
    {
        inline constexpr uint8  kMovedSources     = (uint8)((1u << (uint32)EPrimitiveSource::Foliage) - 1u);
        // Below this many movers the CPU copies cost less than keeping a stream and catching its stoppers up.
        inline constexpr uint32 kMinStreamedMoves = 1024;
        // Below this many movers or synced primitives the work runs serially, faster than it fans out.
        inline constexpr uint32 kParallelMoveThreshold = 1024;
        inline constexpr uint32 kParallelSyncThreshold = 256;
        // Below this a field binds faster serially than it takes to fan out.
        inline constexpr uint32 kFreshFoliageParallelThreshold = 8192;
        inline constexpr uint32 kActiveBit = (uint32)EInstanceFlags::Active << 16;

        template <typename TArray>
        void ReserveGeometric(TArray& Array, SIZE_T Target)
        {
            const SIZE_T Capacity = Array.Max();
            if (Target <= Capacity)
            {
                return;
            }
            Array.Reserve(IntCastChecked<int32>(Math::Max(Target, Capacity + Capacity / 2u)));
        }

        // Retained slots mirror the device buffer byte for byte, so an equal write needs no upload.
        template <typename T>
        FORCEINLINE bool StoreIfChanged(T& Slot, const T& Value)
        {
            if (Memory::Memcmp(&Slot, &Value, sizeof(T)) == 0)
            {
                return false;
            }
            Slot = Value;
            return true;
        }

        // Appends one released batch reference as a (batch, skinned, count) run, merged with the last when they match.
        FORCEINLINE void DeferBatchRelease(TVector<FUIntVector3>& Runs, uint32 BatchIndex, bool bSkinned)
        {
            if (!Runs.IsEmpty() && Runs.Last().x == BatchIndex && Runs.Last().y == (uint32)bSkinned)
            {
                ++Runs.Last().z;
                return;
            }
            Runs.Emplace(BatchIndex, (uint32)bSkinned, 1u);
        }

        // A full write-combined line for the GPU, which the CPU never reads back.
        FORCEINLINE void StreamRecord(FTransformStreamRecord& Dest, const FTransformStreamRecord& Value)
        {
            const float* Source = reinterpret_cast<const float*>(&Value);
            float* Target = reinterpret_cast<float*>(&Dest);
            _mm256_stream_ps(Target, _mm256_loadu_ps(Source));
            _mm256_stream_ps(Target + 8, _mm256_loadu_ps(Source + 8));
        }

        FORCEINLINE FTransformStreamRecord MakeStreamRecord(const FMatrix4& Matrix, const FVector3& LocalCenter, float Radius)
        {
            FTransformStreamRecord Out;
            Out.Transform    = PackTransform3x4(Matrix);
            Out.SphereBounds = SceneCull::TransformSphere(Matrix, LocalCenter, Radius);
            return Out;
        }

        FORCEINLINE float TransformMaxScale(const FMatrix4& Transform)
        {
            const FVector3 AxisX = FVector3(Transform[0]);
            const FVector3 AxisY = FVector3(Transform[1]);
            const FVector3 AxisZ = FVector3(Transform[2]);
            return Math::Sqrt(Math::Max(Math::Dot(AxisX, AxisX), Math::Max(Math::Dot(AxisY, AxisY), Math::Dot(AxisZ, AxisZ))));
        }

        // The flags every retained instance derives from its owner, its surface's material and whether it draws at all.
        FORCEINLINE EInstanceFlags ComposeInstanceFlags(EInstanceFlags BaseFlags, const FSurfaceBinding& Binding, bool bCastShadow,
                                                        bool bCastFarShadow, bool bActive, bool bHasGeometry)
        {
            EInstanceFlags Flags = BaseFlags | Binding.MaterialFlags;
            if (bCastShadow && Binding.bMaterialCastsShadows)
            {
                Flags |= EInstanceFlags::CastShadow;
                if (bCastFarShadow)
                {
                    Flags |= EInstanceFlags::CastFarShadow;
                }
            }
            if (bActive)
            {
                Flags |= EInstanceFlags::Active;
            }
            if (bHasGeometry)
            {
                Flags |= EInstanceFlags::HasGeometry;
            }
            return Flags;
        }

        // The raster picks the vertex stride from EInstanceFlags::Skinned and the vertex data from the header slot, so a disagreement draws a shattered mesh.
        inline void WarnOnGeometryKindMismatch(uint32 HeaderSlot, EInstanceFlags Flags, uint32 EntityID, EPrimitiveSource Source)
        {
#if USING(WITH_EDITOR)
            if (HeaderSlot == MeshletHeaderSlab::kNullSlot)
            {
                return;
            }

            const bool bHeaderSkinned = MeshletHeaderSlab::IsSkinnedSlot(HeaderSlot);
            const bool bFlagSkinned   = EnumHasAnyFlags(Flags, EInstanceFlags::Skinned);
            if (bHeaderSkinned == bFlagSkinned)
            {
                return;
            }

            LOG_ERROR("ScenePrimitiveSet: entity {} source {} names header slot {} ({} geometry) but its "
                      "instance flags say {}. It will rasterize at the wrong vertex stride.",
                      EntityID, (uint32)Source, HeaderSlot,
                      bHeaderSkinned ? "skinned" : "static", bFlagSkinned ? "skinned" : "static");
#else
            (void)HeaderSlot; (void)Flags; (void)EntityID; (void)Source;
#endif
        }
    }
}
