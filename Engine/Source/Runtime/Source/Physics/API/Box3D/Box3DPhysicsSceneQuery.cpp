#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DInternal.h"
#include "Box3DRagdollHandle.h"
#include "Box3DUtils.h"

#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        struct FQueryContext
        {
            FBox3DPhysicsScene*     Scene = nullptr;
            TSpan<const uint32>     IgnoreBodies;
            TVector<SRayResult>*    Hits = nullptr;
            SRayResult*             Closest = nullptr;
            TSpan<ECS::FEntity>     OutEntities;
            int32                   EntityCount = 0;
            FVector3                Start;
            FVector3                End;
            float                   Length = 0.0f;
            bool                    bFound = false;
        };

        // The ignore list is inline and tiny, so a linear scan beats any set on both branches and cache.
        FORCEINLINE bool IsIgnored(TSpan<const uint32> IgnoreBodies, uint32 Handle)
        {
            for (uint32 Ignored : IgnoreBodies)
            {
                if (Ignored == Handle)
                {
                    return true;
                }
            }
            return false;
        }

        b3QueryFilter MakeLayerFilter(ECollisionProfiles LayerMask)
        {
            b3QueryFilter Filter = b3DefaultQueryFilter();
            Filter.categoryBits = (uint64)LayerMask;
            Filter.maskBits = (uint64)LayerMask;
            return Filter;
        }

        void FillRayResult(SRayResult& Result, FBox3DPhysicsScene& Scene, b3ShapeId ShapeId,
                           const FVector3& Start, const FVector3& End, float Length,
                           const b3Vec3& Point, const b3Vec3& Normal, float Fraction)
        {
            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            void* UserData = b3Body_IsValid(BodyId) ? b3Body_GetUserData(BodyId) : nullptr;

            const ECS::FEntity Entity = UnpackEntity(UserData);

            Result.BodyID = (int64)UnpackHandle(UserData);
            Result.Entity = (Entity).Value;
            Result.Start = Start;
            Result.End = End;
            Result.Location = Box3DUtils::FromB3Vec3(Point);
            Result.Normal = Box3DUtils::FromB3Vec3(Normal);
            Result.Fraction = Fraction;
            Result.Distance = Fraction * Length;
            Result.BoneIndex = Scene.ResolveHitBoneIndex(Entity, BodyId);
        }

        float ClosestCastCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction,
                                  uint64_t, int, int, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            const uint32 Handle = HandleOfBody(BodyId);
            if (IsIgnored(Query.IgnoreBodies, Handle))
            {
                return -1.0f;
            }

            FillRayResult(*Query.Closest, *Query.Scene, ShapeId, Query.Start, Query.End, Query.Length, Point, Normal, Fraction);
            Query.bFound = true;

            // Returning the fraction clips the ray, so the traversal keeps narrowing toward the nearest hit.
            return Fraction;
        }

        float AllHitsCastCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction,
                                  uint64_t, int, int, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            const uint32 Handle = HandleOfBody(BodyId);
            if (IsIgnored(Query.IgnoreBodies, Handle))
            {
                return -1.0f;
            }

            SRayResult& Result = Query.Hits->emplace_back();
            FillRayResult(Result, *Query.Scene, ShapeId, Query.Start, Query.End, Query.Length, Point, Normal, Fraction);

            return 1.0f;
        }

        bool OverlapCallback(b3ShapeId ShapeId, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            void* UserData = b3Body_IsValid(BodyId) ? b3Body_GetUserData(BodyId) : nullptr;
            const uint32 Handle = UnpackHandle(UserData);

            if (IsIgnored(Query.IgnoreBodies, Handle))
            {
                return true;
            }

            const ECS::FEntity Entity = UnpackEntity(UserData);

            // Several shapes can share one body, so the last few entities are checked before appending.
            const int32 Written = Query.EntityCount;
            const int32 ScanFrom = Math::Max(0, Written - 8);
            for (int32 i = ScanFrom; i < Written; ++i)
            {
                if (Query.OutEntities[i] == Entity)
                {
                    return true;
                }
            }

            if (Written >= (int32)Query.OutEntities.size())
            {
                return false;
            }

            Query.OutEntities[Written] = Entity;
            Query.EntityCount = Written + 1;
            return true;
        }
    }

    int32 FBox3DPhysicsScene::ResolveHitBoneIndex(ECS::FEntity Entity, b3BodyId BodyId) const
    {
        if (Entity == ECS::NullEntity)
        {
            return INDEX_NONE;
        }

        ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
        const SRagdollComponent* RagdollComp = Registry.TryGet<SRagdollComponent>(Entity);
        if (RagdollComp == nullptr || !RagdollComp->Ragdoll)
        {
            return INDEX_NONE;
        }

        // Ragdolls are a handful of bodies, so a linear scan beats maintaining a body-to-bone map.
        const FPhysicsRagdollHandle& Handle = *RagdollComp->Ragdoll;
        for (size_t i = 0; i < Handle.Bodies.size(); ++i)
        {
            if (B3_ID_EQUALS(Handle.Bodies[i], BodyId))
            {
                return i < Handle.JointToBone.size() ? Handle.JointToBone[i] : INDEX_NONE;
            }
        }

        return INDEX_NONE;
    }

    TOptional<SRayResult> FBox3DPhysicsScene::CastRay(const SRayCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= LE_SMALL_NUMBER)
        {
            return {};
        }

        const b3QueryFilter Filter = MakeLayerFilter(Settings.LayerMask);
        SRayResult Result{};

        if (Settings.IgnoreBodies.empty())
        {
            const b3RayResult Hit = b3World_CastRayClosest(WorldId, Box3DUtils::ToB3Vec3(Settings.Start),
                                                           Box3DUtils::ToB3Vec3(Delta), Filter);
            if (!Hit.hit)
            {
                return {};
            }

            FillRayResult(Result, *this, Hit.shapeId, Settings.Start, Settings.End, Length, Hit.point, Hit.normal, Hit.fraction);
            return Result;
        }

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = TSpan<const uint32>(Settings.IgnoreBodies.data(), Settings.IgnoreBodies.size());
        Query.Closest = &Result;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastRay(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), Box3DUtils::ToB3Vec3(Delta),
                        Filter, &ClosestCastCallback, &Query);

        if (!Query.bFound)
        {
            return {};
        }

        return Result;
    }

    void FBox3DPhysicsScene::CastRayAll(const SRayCastSettings& Settings, TVector<SRayResult>& OutHits)
    {
        LUMINA_PROFILE_SCOPE();

        OutHits.clear();

        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= LE_SMALL_NUMBER)
        {
            return;
        }

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = TSpan<const uint32>(Settings.IgnoreBodies.data(), Settings.IgnoreBodies.size());
        Query.Hits = &OutHits;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastRay(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), Box3DUtils::ToB3Vec3(Delta),
                        MakeLayerFilter(Settings.LayerMask), &AllHitsCastCallback, &Query);

        Algo::Sort(OutHits.begin(), OutHits.end(), [](const SRayResult& A, const SRayResult& B)
        {
            return A.Fraction < B.Fraction;
        });
    }

    void FBox3DPhysicsScene::CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits)
    {
        LUMINA_PROFILE_SCOPE();

        OutHits.clear();

        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= LE_SMALL_NUMBER)
        {
            return;
        }

        // A sphere proxy is one point with a radius, so the sweep needs no scratch geometry.
        const b3Vec3 Origin{ 0.0f, 0.0f, 0.0f };
        const b3ShapeProxy Proxy{ &Origin, 1, Settings.Radius };

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = TSpan<const uint32>(Settings.IgnoreBodies.data(), Settings.IgnoreBodies.size());
        Query.Hits = &OutHits;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastShape(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), &Proxy, Box3DUtils::ToB3Vec3(Delta),
                          MakeLayerFilter(Settings.LayerMask), &AllHitsCastCallback, &Query);

        Algo::Sort(OutHits.begin(), OutHits.end(), [](const SRayResult& A, const SRayResult& B)
        {
            return A.Fraction < B.Fraction;
        });
    }

    TOptional<SRayResult> FBox3DPhysicsScene::CastSphereClosest(const SSphereCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= LE_SMALL_NUMBER)
        {
            return {};
        }

        const b3Vec3 Origin{ 0.0f, 0.0f, 0.0f };
        const b3ShapeProxy Proxy{ &Origin, 1, Settings.Radius };

        SRayResult Result{};

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = TSpan<const uint32>(Settings.IgnoreBodies.data(), Settings.IgnoreBodies.size());
        Query.Closest = &Result;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastShape(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), &Proxy, Box3DUtils::ToB3Vec3(Delta),
                          MakeLayerFilter(Settings.LayerMask), &ClosestCastCallback, &Query);

        if (!Query.bFound)
        {
            return {};
        }

        return Result;
    }

    int32 FBox3DPhysicsScene::OverlapSphere(const FVector3& Center, float Radius, TSpan<const uint32> IgnoreBodies, TSpan<ECS::FEntity> OutEntities)
    {
        LUMINA_PROFILE_SCOPE();

        const b3Vec3 Origin{ 0.0f, 0.0f, 0.0f };
        const b3ShapeProxy Proxy{ &Origin, 1, Radius };

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = IgnoreBodies;
        Query.OutEntities = OutEntities;

        b3World_OverlapShape(WorldId, Box3DUtils::ToB3Vec3(Center), &Proxy, b3DefaultQueryFilter(), &OverlapCallback, &Query);

        return Query.EntityCount;
    }

    int32 FBox3DPhysicsScene::OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, TSpan<const uint32> IgnoreBodies, TSpan<ECS::FEntity> OutEntities)
    {
        LUMINA_PROFILE_SCOPE();

        b3Vec3 Corners[8];
        for (int32 i = 0; i < 8; ++i)
        {
            const FVector3 Sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            Corners[i] = Box3DUtils::ToB3Vec3(Math::Rotate(Rotation, Sign * HalfExtents));
        }

        const b3ShapeProxy Proxy{ Corners, 8, 0.0f };

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = IgnoreBodies;
        Query.OutEntities = OutEntities;

        b3World_OverlapShape(WorldId, Box3DUtils::ToB3Vec3(Center), &Proxy, b3DefaultQueryFilter(), &OverlapCallback, &Query);

        return Query.EntityCount;
    }

    int32 FBox3DPhysicsScene::CollidePoint(const FVector3& Point, TSpan<const uint32> IgnoreBodies, TSpan<ECS::FEntity> OutEntities)
    {
        LUMINA_PROFILE_SCOPE();

        // A zero-radius point proxy is the containment test, with no sweep.
        const b3Vec3 Origin{ 0.0f, 0.0f, 0.0f };
        const b3ShapeProxy Proxy{ &Origin, 1, 0.0f };

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreBodies = IgnoreBodies;
        Query.OutEntities = OutEntities;

        b3World_OverlapShape(WorldId, Box3DUtils::ToB3Vec3(Point), &Proxy, b3DefaultQueryFilter(), &OverlapCallback, &Query);

        return Query.EntityCount;
    }
}
