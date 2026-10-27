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
            TSpan<const ECS::FEntity>     IgnoreEntities;
            TVector<SRayResult>*    Hits = nullptr;
            SRayResult*             Closest = nullptr;
            TSpan<ECS::FEntity>     OutEntities;
            int32                   EntityCount = 0;
            FVector3                Start;
            FVector3                End;
            float                   Length = 0.0f;
            bool                    bFound = false;

            // The nearest hit so far, filled into Closest once the traversal ends rather than at every closer hit.
            b3ShapeId               ClosestShape{};
            b3Pos                   ClosestPoint{};
            b3Vec3                  ClosestNormal{};
            float                   ClosestFraction = 0.0f;
        };

        // The ignore list is inline and tiny, so a linear scan beats any set on both branches and cache.
        FORCEINLINE bool IsIgnored(TSpan<const ECS::FEntity> IgnoreEntities, ECS::FEntity Entity)
        {
            for (ECS::FEntity Ignored : IgnoreEntities)
            {
                if (Ignored == Entity)
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

            Result.Target = Scene.MakeBodyTarget(UnpackHandle(UserData));
            Result.Entity = (Entity).Value;
            Result.Start = Start;
            Result.End = End;
            Result.Location = Box3DUtils::FromB3Vec3(Point);
            Result.Normal = Box3DUtils::FromB3Vec3(Normal);
            Result.Fraction = Fraction;
            Result.Distance = Fraction * Length;
            Result.BoneIndex = Scene.ResolveHitBoneIndex(Entity, BodyId);
            Result.InstanceIndex = Box3DUtils::UnpackInstanceIndex(b3Shape_GetUserData(ShapeId));
            Result.bHit = true;
        }

        float ClosestCastCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction,
                                  uint64_t, int, int, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            if (IsIgnored(Query.IgnoreEntities, EntityOfBody(BodyId)))
            {
                return -1.0f;
            }

            Query.ClosestShape    = ShapeId;
            Query.ClosestPoint    = Point;
            Query.ClosestNormal   = Normal;
            Query.ClosestFraction = Fraction;
            Query.bFound          = true;

            // Returning the fraction clips the ray, so the traversal keeps narrowing toward the nearest hit.
            return Fraction;
        }

        bool FinishClosest(FQueryContext& Query)
        {
            if (Query.bFound)
            {
                FillRayResult(*Query.Closest, *Query.Scene, Query.ClosestShape, Query.Start, Query.End, Query.Length,
                              Query.ClosestPoint, Query.ClosestNormal, Query.ClosestFraction);
            }
            return Query.bFound;
        }

        float AllHitsCastCallback(b3ShapeId ShapeId, b3Pos Point, b3Vec3 Normal, float Fraction,
                                  uint64_t, int, int, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            if (IsIgnored(Query.IgnoreEntities, EntityOfBody(BodyId)))
            {
                return -1.0f;
            }

            SRayResult& Result = Query.Hits->emplace_back();
            FillRayResult(Result, *Query.Scene, ShapeId, Query.Start, Query.End, Query.Length, Point, Normal, Fraction);

            return 1.0f;
        }

        float AnyHitCastCallback(b3ShapeId ShapeId, b3Pos, b3Vec3, float, uint64_t, int, int, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);
            if (IsIgnored(Query.IgnoreEntities, EntityOfBody(b3Shape_GetBody(ShapeId))))
            {
                return -1.0f;
            }

            // Zero ends the traversal, since one blocker answers the question.
            Query.bFound = true;
            return 0.0f;
        }

        // Box3D proxies are points plus a radius, so each query shape is a few points around the query position.
        b3ShapeProxy MakeProxy(const SQueryShape& Shape, b3Vec3 (&Points)[8])
        {
            switch (Shape.Type)
            {
            case EQueryShapeType::Box:
                for (int32 i = 0; i < 8; ++i)
                {
                    const FVector3 Sign((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
                    Points[i] = Box3DUtils::ToB3Vec3(Math::Rotate(Shape.Rotation, Sign * Shape.HalfExtents));
                }
                return b3ShapeProxy{ Points, 8, 0.0f };
            case EQueryShapeType::Capsule:
            {
                const FVector3 Axis = Math::Rotate(Shape.Rotation, FVector3(0.0f, Shape.HalfHeight, 0.0f));
                Points[0] = Box3DUtils::ToB3Vec3(Axis);
                Points[1] = Box3DUtils::ToB3Vec3(-Axis);
                return b3ShapeProxy{ Points, 2, Shape.Radius };
            }
            default:
                Points[0] = b3Vec3{ 0.0f, 0.0f, 0.0f };
                return b3ShapeProxy{ Points, 1, Shape.Radius };
            }
        }

        void SortNearToFar(TVector<SRayResult>& Hits)
        {
            Algo::Sort(Hits, [](const SRayResult& A, const SRayResult& B) { return A.Fraction < B.Fraction; });
        }

        // Inline for the common single-shape body, spilling only for a large compound.
        using FBodyShapes = TFixedVector<b3ShapeId, 8>;

        bool GatherShapes(b3BodyId Body, FBodyShapes& Out)
        {
            if (!b3Body_IsValid(Body))
            {
                return false;
            }
            Out.resize((size_t)b3Body_GetShapeCount(Body));
            Out.resize((size_t)b3Body_GetShapes(Body, Out.data(), (int)Out.size()));
            return !Out.empty();
        }

        bool OverlapCallback(b3ShapeId ShapeId, void* Context)
        {
            FQueryContext& Query = *static_cast<FQueryContext*>(Context);

            const b3BodyId BodyId = b3Shape_GetBody(ShapeId);
            void* UserData = b3Body_IsValid(BodyId) ? b3Body_GetUserData(BodyId) : nullptr;

            if (IsIgnored(Query.IgnoreEntities, EntityOfBody(BodyId)))
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
            return Constants::kIndexNone;
        }

        ECS::FRegistry& Registry = World->GetEntityRegistry();
        const SRagdollComponent* RagdollComp = Registry.TryGet<SRagdollComponent>(Entity);
        if (RagdollComp == nullptr || !RagdollComp->Ragdoll)
        {
            return Constants::kIndexNone;
        }

        // Ragdolls are a handful of bodies, so a linear scan beats maintaining a body-to-bone map.
        const FPhysicsRagdollHandle& Handle = *RagdollComp->Ragdoll;
        for (size_t i = 0; i < Handle.Bodies.size(); ++i)
        {
            if (B3_ID_EQUALS(Handle.Bodies[i], BodyId))
            {
                return i < Handle.JointToBone.size() ? Handle.JointToBone[i] : Constants::kIndexNone;
            }
        }

        return Constants::kIndexNone;
    }

    // Queries carry no zone, since gameplay runs thousands a frame on every worker and fiber builds serialize Tracy events.
    TOptional<SRayResult> FBox3DPhysicsScene::CastRay(const SRayCastSettings& Settings)
    {
        TOptional<SRayResult> Hit = CastRayClosest(Settings);
        if (Settings.bDrawDebug)
        {
            DrawRayDebug(Settings, Hit ? TSpan<const SRayResult>(&*Hit, 1) : TSpan<const SRayResult>());
        }
        return Hit;
    }

    TOptional<SRayResult> FBox3DPhysicsScene::CastRayClosest(const SRayCastSettings& Settings)
    {
        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= Math::kSmallNumber)
        {
            return {};
        }

        const b3QueryFilter Filter = MakeLayerFilter(Settings.LayerMask);
        SRayResult Result{};

        if (Settings.IgnoreEntities.empty())
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
        Query.IgnoreEntities = TSpan<const ECS::FEntity>(Settings.IgnoreEntities.data(), Settings.IgnoreEntities.size());
        Query.Closest = &Result;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastRay(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), Box3DUtils::ToB3Vec3(Delta),
                        Filter, &ClosestCastCallback, &Query);

        if (!FinishClosest(Query))
        {
            return {};
        }
        return Result;
    }

    void FBox3DPhysicsScene::DrawRayDebug(const SRayCastSettings& Settings, TSpan<const SRayResult> Hits) const
    {
        FWorldDebugInterface* Debug = World ? World->GetDebugInterface() : nullptr;
        if (Debug == nullptr)
        {
            return;
        }

        const FVector4 HitColor = FColor(Settings.DebugHitColor);
        const FVector4 MissColor = FColor(Settings.DebugMissColor);
        if (Hits.empty())
        {
            Debug->DrawLine(Settings.Start, Settings.End, MissColor, 3.0f, true, Settings.DebugDuration);
            return;
        }

        Debug->DrawLine(Settings.Start, Hits[0].Location, HitColor, 3.0f, true, Settings.DebugDuration);
        Debug->DrawLine(Hits[0].Location, Settings.End, MissColor, 3.0f, true, Settings.DebugDuration);
        for (const SRayResult& Hit : Hits)
        {
            Debug->DrawLine(Hit.Location, Hit.Location + Hit.Normal * 0.5f, FColor::Blue, 3.0f, true, Settings.DebugDuration);
            Debug->DrawBox(Hit.Location, FVector3(0.05f), FQuat(1.0f, 0.0f, 0.0f, 0.0f), FColor::Yellow, 3.0f, true, Settings.DebugDuration);
        }
    }

    void FBox3DPhysicsScene::CastRayAll(const SRayCastSettings& Settings, TVector<SRayResult>& OutHits)
    {
        OutHits.clear();

        const FVector3 Delta = Settings.End - Settings.Start;
        const float Length = Math::Length(Delta);
        if (Length <= Math::kSmallNumber)
        {
            return;
        }

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreEntities = TSpan<const ECS::FEntity>(Settings.IgnoreEntities.data(), Settings.IgnoreEntities.size());
        Query.Hits = &OutHits;
        Query.Start = Settings.Start;
        Query.End = Settings.End;
        Query.Length = Length;

        b3World_CastRay(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), Box3DUtils::ToB3Vec3(Delta),
                        MakeLayerFilter(Settings.LayerMask), &AllHitsCastCallback, &Query);
        SortNearToFar(OutHits);
        if (Settings.bDrawDebug)
        {
            DrawRayDebug(Settings, TSpan<const SRayResult>(OutHits.data(), OutHits.size()));
        }
    }

    bool FBox3DPhysicsScene::CastRayAny(const SRayCastSettings& Settings)
    {
        const FVector3 Delta = Settings.End - Settings.Start;
        if (Math::Length(Delta) <= Math::kSmallNumber)
        {
            return false;
        }

        FQueryContext Query;
        Query.IgnoreEntities = TSpan<const ECS::FEntity>(Settings.IgnoreEntities.data(), Settings.IgnoreEntities.size());
        b3World_CastRay(WorldId, Box3DUtils::ToB3Vec3(Settings.Start), Box3DUtils::ToB3Vec3(Delta),
                        MakeLayerFilter(Settings.LayerMask), &AnyHitCastCallback, &Query);
        return Query.bFound;
    }

    TOptional<SRayResult> FBox3DPhysicsScene::CastRayAgainst(ECS::FEntity Entity, const FVector3& Start, const FVector3& End)
    {
        const FVector3 Delta = End - Start;
        const float Length = Math::Length(Delta);
        FBodyShapes Shapes;
        if (Length <= Math::kSmallNumber || !GatherShapes(ResolveBody(FindEntityBody(Entity)), Shapes))
        {
            return {};
        }

        TOptional<SRayResult> Nearest;
        for (b3ShapeId Shape : Shapes)
        {
            const b3WorldCastOutput Hit = b3Shape_RayCast(Shape, Box3DUtils::ToB3Vec3(Start), Box3DUtils::ToB3Vec3(Delta));
            if (Hit.hit && (!Nearest.has_value() || Hit.fraction < Nearest->Fraction))
            {
                Nearest.emplace();
                FillRayResult(*Nearest, *this, Shape, Start, End, Length, Hit.point, Hit.normal, Hit.fraction);
            }
        }
        return Nearest;
    }

    void FBox3DPhysicsScene::CastShapeAll(const SQueryShape& Shape, const FVector3& Start, const FVector3& End, ECollisionProfiles LayerMask,
                                          TSpan<const ECS::FEntity> IgnoreEntities, TVector<SRayResult>& OutHits)
    {
        OutHits.clear();

        const FVector3 Delta = End - Start;
        const float Length = Math::Length(Delta);
        if (Length <= Math::kSmallNumber)
        {
            return;
        }

        b3Vec3 Points[8];
        const b3ShapeProxy Proxy = MakeProxy(Shape, Points);

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreEntities = IgnoreEntities;
        Query.Hits = &OutHits;
        Query.Start = Start;
        Query.End = End;
        Query.Length = Length;

        b3World_CastShape(WorldId, Box3DUtils::ToB3Vec3(Start), &Proxy, Box3DUtils::ToB3Vec3(Delta),
                          MakeLayerFilter(LayerMask), &AllHitsCastCallback, &Query);
        SortNearToFar(OutHits);
    }

    TOptional<SRayResult> FBox3DPhysicsScene::CastShapeClosest(const SQueryShape& Shape, const FVector3& Start, const FVector3& End,
                                                               ECollisionProfiles LayerMask, TSpan<const ECS::FEntity> IgnoreEntities)
    {
        const FVector3 Delta = End - Start;
        const float Length = Math::Length(Delta);
        if (Length <= Math::kSmallNumber)
        {
            return {};
        }

        b3Vec3 Points[8];
        const b3ShapeProxy Proxy = MakeProxy(Shape, Points);

        SRayResult Result{};
        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreEntities = IgnoreEntities;
        Query.Closest = &Result;
        Query.Start = Start;
        Query.End = End;
        Query.Length = Length;

        b3World_CastShape(WorldId, Box3DUtils::ToB3Vec3(Start), &Proxy, Box3DUtils::ToB3Vec3(Delta),
                          MakeLayerFilter(LayerMask), &ClosestCastCallback, &Query);
        if (!FinishClosest(Query))
        {
            return {};
        }
        return Result;
    }

    int32 FBox3DPhysicsScene::OverlapShape(const SQueryShape& Shape, const FVector3& Center, ECollisionProfiles LayerMask,
                                           TSpan<const ECS::FEntity> IgnoreEntities, TSpan<ECS::FEntity> OutEntities)
    {
        b3Vec3 Points[8];
        const b3ShapeProxy Proxy = MakeProxy(Shape, Points);

        FQueryContext Query;
        Query.Scene = this;
        Query.IgnoreEntities = IgnoreEntities;
        Query.OutEntities = OutEntities;

        b3World_OverlapShape(WorldId, Box3DUtils::ToB3Vec3(Center), &Proxy, MakeLayerFilter(LayerMask), &OverlapCallback, &Query);
        return Query.EntityCount;
    }

    bool FBox3DPhysicsScene::GetClosestPointOnBody(ECS::FEntity Entity, const FVector3& Point, FVector3& OutPoint) const
    {
        FBodyShapes Shapes;
        if (!GatherShapes(ResolveBody(FindEntityBody(Entity)), Shapes))
        {
            return false;
        }

        float NearestDistanceSq = FLT_MAX;
        for (b3ShapeId Shape : Shapes)
        {
            const FVector3 Candidate = Box3DUtils::FromB3Vec3(b3Shape_GetClosestPoint(Shape, Box3DUtils::ToB3Vec3(Point)));
            const float DistanceSq = Math::LengthSquared(Candidate - Point);
            if (DistanceSq < NearestDistanceSq)
            {
                NearestDistanceSq = DistanceSq;
                OutPoint = Candidate;
            }
        }
        return true;
    }

    bool FBox3DPhysicsScene::GetBodyBounds(ECS::FEntity Entity, SAABB& OutBounds) const
    {
        FBodyShapes Shapes;
        if (!GatherShapes(ResolveBody(FindEntityBody(Entity)), Shapes))
        {
            return false;
        }

        OutBounds = SAABB(FVector3(FLT_MAX), FVector3(-FLT_MAX));
        for (b3ShapeId Shape : Shapes)
        {
            const b3AABB Bounds = b3Shape_GetAABB(Shape);
            OutBounds.Min = Math::Min(OutBounds.Min, Box3DUtils::FromB3Vec3(Bounds.lowerBound));
            OutBounds.Max = Math::Max(OutBounds.Max, Box3DUtils::FromB3Vec3(Bounds.upperBound));
        }
        return true;
    }
}
