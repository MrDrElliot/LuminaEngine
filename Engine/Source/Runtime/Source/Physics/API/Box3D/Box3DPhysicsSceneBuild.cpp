#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DPhysics.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Log/Log.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Box3D height fields index [row * countX + column] with the sample grid on the XZ plane.
        b3HeightFieldData* BuildTerrainHeightField(const STerrainComponent& Terrain)
        {
            const int32 Res = Terrain.Resolution;
            if (Res < 2 || (int64)Terrain.Heightmap.size() < (int64)Res * (int64)Res)
            {
                return nullptr;
            }

            TVector<float> Heights;
            Heights.resize((size_t)Res * (size_t)Res);

            float MinHeight = FLT_MAX;
            float MaxHeight = -FLT_MAX;
            for (size_t i = 0; i < Heights.size(); ++i)
            {
                const float H = Terrain.Heightmap[i] * Terrain.MaxHeight;
                Heights[i] = H;
                MinHeight = Math::Min(MinHeight, H);
                MaxHeight = Math::Max(MaxHeight, H);
            }

            const float Stride = Terrain.TileWorldSize / float(Res - 1);

            b3HeightFieldDef Def = b3HeightFieldDef{};
            Def.heights = Heights.data();
            Def.countX = Res;
            Def.countZ = Res;
            Def.scale = b3Vec3{ Stride, 1.0f, Stride };
            Def.globalMinimumHeight = MinHeight;
            Def.globalMaximumHeight = Math::Max(MaxHeight, MinHeight + B3_LINEAR_SLOP);

            return b3CreateHeightField(&Def);
        }
    }

    EBodyBuildStatus FBox3DPhysicsScene::TryBuildRigidBody(ECS::FRegistry& Registry, ECS::FEntity Entity, FRigidBodyBuildResult& Out)
    {
        LUMINA_PROFILE_SCOPE();

        const SRigidBodyComponent* RigidBody = Registry.TryGet<SRigidBodyComponent>(Entity);
        if (RigidBody == nullptr)
        {
            return EBodyBuildStatus::Error;
        }

        if (RigidBody->BodyID != InvalidBodyHandle)
        {
            return EBodyBuildStatus::AlreadyExists;
        }

        const STransformComponent* Transform = Registry.TryGet<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return EBodyBuildStatus::Defer;
        }

        Out.Shapes.clear();

        FVector3 ColliderTranslationOffset(0.0f);
        FVector3 ColliderRotationOffset(0.0f);
        bool bMustBeStatic = false;
        bool bColliderIsTrigger = false;
        const CPhysicsMaterial* ResolvedMaterial = nullptr;

        const FVector3 Scale = Transform->GetScale();
        const float UniformScale = Transform->MaxScale();

        if (const SBoxColliderComponent* BC = Registry.TryGet<SBoxColliderComponent>(Entity))
        {
            ColliderTranslationOffset = BC->TranslationOffset;
            ColliderRotationOffset = BC->RotationOffset;
            bColliderIsTrigger = BC->bIsTrigger;
            ResolvedMaterial = BC->PhysicsMaterial.Get();

            const b3HullData* Hull = GetOrCreateBoxHull(BC->HalfExtent * Scale);
            if (Hull == nullptr)
            {
                LOG_ERROR("Failed to create BoxCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity()));
        }
        else if (const SSphereColliderComponent* SC = Registry.TryGet<SSphereColliderComponent>(Entity))
        {
            ColliderTranslationOffset = SC->TranslationOffset;
            bColliderIsTrigger = SC->bIsTrigger;
            ResolvedMaterial = SC->PhysicsMaterial.Get();

            Out.Shapes.push_back(MakeSphereShape(FVector3(0.0f), SC->Radius * UniformScale));
        }
        else if (const SCapsuleColliderComponent* CC = Registry.TryGet<SCapsuleColliderComponent>(Entity))
        {
            ColliderTranslationOffset = CC->TranslationOffset;
            ColliderRotationOffset = CC->RotationOffset;
            bColliderIsTrigger = CC->bIsTrigger;
            ResolvedMaterial = CC->PhysicsMaterial.Get();

            Out.Shapes.push_back(MakeCapsuleShape(FVector3(0.0f), FQuat::Identity(), CC->Radius * UniformScale, CC->HalfHeight * UniformScale));
        }
        else if (const SCylinderColliderComponent* CyC = Registry.TryGet<SCylinderColliderComponent>(Entity))
        {
            ColliderTranslationOffset = CyC->TranslationOffset;
            ColliderRotationOffset = CyC->RotationOffset;
            bColliderIsTrigger = CyC->bIsTrigger;
            ResolvedMaterial = CyC->PhysicsMaterial.Get();

            const b3HullData* Hull = GetOrCreateCylinderHull(CyC->Radius * UniformScale, CyC->HalfHeight * UniformScale);
            if (Hull == nullptr)
            {
                LOG_ERROR("Failed to create CylinderCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity()));
        }
        else if (const STaperedCapsuleColliderComponent* TCC = Registry.TryGet<STaperedCapsuleColliderComponent>(Entity))
        {
            ColliderTranslationOffset = TCC->TranslationOffset;
            ColliderRotationOffset = TCC->RotationOffset;
            bColliderIsTrigger = TCC->bIsTrigger;
            ResolvedMaterial = TCC->PhysicsMaterial.Get();

            WarnTaperedCapsuleOnce();

            const b3HullData* Hull = GetOrCreateTaperedCylinderHull(TCC->HalfHeight * UniformScale, TCC->TopRadius * UniformScale, TCC->BottomRadius * UniformScale);
            if (Hull == nullptr)
            {
                LOG_ERROR("Failed to create TaperedCapsuleCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity()));
        }
        else if (const STaperedCylinderColliderComponent* TCyC = Registry.TryGet<STaperedCylinderColliderComponent>(Entity))
        {
            ColliderTranslationOffset = TCyC->TranslationOffset;
            ColliderRotationOffset = TCyC->RotationOffset;
            bColliderIsTrigger = TCyC->bIsTrigger;
            ResolvedMaterial = TCyC->PhysicsMaterial.Get();

            const b3HullData* Hull = GetOrCreateTaperedCylinderHull(TCyC->HalfHeight * UniformScale, TCyC->TopRadius * UniformScale, TCyC->BottomRadius * UniformScale);
            if (Hull == nullptr)
            {
                LOG_ERROR("Failed to create TaperedCylinderCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity()));
        }
        else if (const SPlaneColliderComponent* PC = Registry.TryGet<SPlaneColliderComponent>(Entity))
        {
            bColliderIsTrigger = PC->bIsTrigger;
            ResolvedMaterial = PC->PhysicsMaterial.Get();

            // Box3D has no plane primitive, so the ground becomes a wide, thin static box.
            const float Extent = Math::Max(PC->HalfExtent, 1.0f);
            constexpr float PlaneThickness = 0.05f;
            const b3HullData* Hull = GetOrCreateBoxHull(FVector3(Extent, PlaneThickness, Extent));
            if (Hull == nullptr)
            {
                LOG_ERROR("Failed to create PlaneCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f, -PlaneThickness, 0.0f), FQuat::Identity()));
            bMustBeStatic = true;
        }
        else if (const SCollisionShapeComponent* CSC = Registry.TryGet<SCollisionShapeComponent>(Entity))
        {
            ColliderTranslationOffset = CSC->TranslationOffset;
            ColliderRotationOffset = CSC->RotationOffset;
            bColliderIsTrigger = CSC->bIsTrigger;

            const CCollisionShape* Asset = CSC->CollisionShape.Get();
            if (Asset == nullptr || !Asset->HasCollision())
            {
                LOG_WARN("CollisionShape on Entity {} is missing or empty; no body was built.", (Entity).Value);
                return EBodyBuildStatus::Error;
            }

            ResolvedMaterial = CSC->PhysicsMaterial.IsValid() ? CSC->PhysicsMaterial.Get() : Asset->PhysicsMaterial.Get();

            if (!BuildCollisionShapeAsset(*Asset, Scale, Out.Shapes))
            {
                LOG_ERROR("Failed to build collision shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }

            bMustBeStatic = Asset->IsConcave();
        }
        else if (const SCompoundColliderComponent* CompC = Registry.TryGet<SCompoundColliderComponent>(Entity))
        {
            bColliderIsTrigger = CompC->bIsTrigger;
            ResolvedMaterial = CompC->PhysicsMaterial.Get();

            if (!BuildCompoundShapes(*CompC, *Transform, Out.Shapes))
            {
                LOG_ERROR("Failed to create CompoundCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SMeshColliderComponent* MC = Registry.TryGet<SMeshColliderComponent>(Entity))
        {
            ColliderTranslationOffset = MC->TranslationOffset;
            ColliderRotationOffset = MC->RotationOffset;
            bColliderIsTrigger = MC->bIsTrigger;
            ResolvedMaterial = MC->PhysicsMaterial.Get();

            CStaticMesh* Mesh = MC->Mesh.Get();
            if (Mesh == nullptr)
            {
                if (const SStaticMeshComponent* SMC = Registry.TryGet<SStaticMeshComponent>(Entity))
                {
                    Mesh = SMC->StaticMesh.Get();
                }
            }

            if (Mesh == nullptr || Mesh->HasAnyFlag(OF_NeedsLoad) || Mesh->GetMeshResource().MeshletData.IsEmpty())
            {
                return EBodyBuildStatus::Defer;
            }

            if (MC->bConvex)
            {
                const b3HullData* Hull = GetOrCreateMeshHull(Mesh);
                if (Hull == nullptr)
                {
                    LOG_ERROR("Failed to create convex MeshCollider shape for Entity: {}", (Entity).Value);
                    return EBodyBuildStatus::Error;
                }

                FPendingShape Shape = MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity());
                Shape.Scale = Box3DUtils::ToB3Vec3(Scale);
                Out.Shapes.push_back(Shape);
            }
            else
            {
                const b3MeshData* MeshData = GetOrCreateTriangleMesh(Mesh);
                if (MeshData == nullptr)
                {
                    LOG_ERROR("Failed to create MeshCollider shape for Entity: {}", (Entity).Value);
                    return EBodyBuildStatus::Error;
                }

                FPendingShape Shape;
                Shape.Type = b3_meshShape;
                Shape.Mesh = MeshData;
                Shape.Scale = Box3DUtils::ToB3Vec3(Scale);
                Out.Shapes.push_back(Shape);
                bMustBeStatic = true;
            }
        }
        else if (const SDynamicMeshColliderComponent* DMC = Registry.TryGet<SDynamicMeshColliderComponent>(Entity))
        {
            bColliderIsTrigger = DMC->bIsTrigger;
            ResolvedMaterial = DMC->PhysicsMaterial.Get();

            const SDynamicMeshComponent* DM = Registry.TryGet<SDynamicMeshComponent>(Entity);
            const TSharedPtr<FDynamicMeshRenderData> MeshData = DM ? DM->LoadRenderData() : nullptr;
            if (!MeshData || MeshData->Resource.MeshletData.IsEmpty())
            {
                if (MeshData && MeshData->MeshletHeaderSlot != 0)
                {
                    LOG_ERROR("Entity {} needs the dynamic mesh CPU meshlet data, which was dropped after upload. "
                              "Set bKeepCPUMeshletData on the SDynamicMeshComponent.", (Entity).Value);
                    return EBodyBuildStatus::Error;
                }
                return EBodyBuildStatus::Defer;
            }

            TVector<b3Vec3> Positions;
            TVector<int32> Indices;
            if (!GatherMeshResourceGeometry(MeshData->Resource, Scale, Positions, DMC->bConvex ? nullptr : &Indices))
            {
                LOG_ERROR("Failed to create DynamicMeshCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }

            // A dynamic mesh is unique per component, so the shared caches have nothing to hit.
            if (DMC->bConvex)
            {
                b3HullData* Hull = b3CreateHull(Positions.data(), (int)Positions.size(), B3_MAX_HULL_VERTICES);
                if (Hull == nullptr)
                {
                    return EBodyBuildStatus::Error;
                }
                TrackOwnedHull(Hull);
                Out.Shapes.push_back(MakeHullShape(Hull, FVector3(0.0f), FQuat::Identity()));
            }
            else
            {
                b3MeshDef Def = b3MeshDef{};
                Def.vertices = Positions.data();
                Def.vertexCount = (int)Positions.size();
                Def.indices = Indices.data();
                Def.triangleCount = (int)(Indices.size() / 3);
                Def.weldVertices = true;
                Def.identifyEdges = true;

                b3MeshData* Built = b3CreateMesh(&Def, nullptr, 0);
                if (Built == nullptr)
                {
                    return EBodyBuildStatus::Error;
                }
                TrackOwnedMesh(Built);

                FPendingShape Shape;
                Shape.Type = b3_meshShape;
                Shape.Mesh = Built;
                Out.Shapes.push_back(Shape);
                bMustBeStatic = true;
            }
        }
        else if (const STerrainColliderComponent* TC = Registry.TryGet<STerrainColliderComponent>(Entity))
        {
            bColliderIsTrigger = TC->bIsTrigger;
            ResolvedMaterial = TC->PhysicsMaterial.Get();

            const STerrainComponent* Terrain = Registry.TryGet<STerrainComponent>(Entity);
            if (Terrain == nullptr || Terrain->Heightmap.empty())
            {
                return EBodyBuildStatus::Defer;
            }

            b3HeightFieldData* Field = BuildTerrainHeightField(*Terrain);
            if (Field == nullptr)
            {
                LOG_ERROR("Failed to create TerrainCollider shape for Entity: {}", (Entity).Value);
                return EBodyBuildStatus::Error;
            }
            TrackOwnedHeightField(Field);

            FPendingShape Shape;
            Shape.Type = b3_heightShape;
            Shape.HeightField = Field;
            // Box3D indexes the grid from a corner, so it is recentered onto the tile.
            Shape.Transform.p = b3Vec3{ Terrain->TileWorldSize * -0.5f, 0.0f, Terrain->TileWorldSize * -0.5f };
            Out.Shapes.push_back(Shape);
            bMustBeStatic = true;
        }
        else
        {
            return EBodyBuildStatus::NoCollider;
        }

        EBodyType BodyType = RigidBody->BodyType;
        if (bMustBeStatic && BodyType == EBodyType::Dynamic)
        {
            LOG_WARN("Collider on Entity {} is concave; forcing motion type to Static.", (Entity).Value);
            BodyType = EBodyType::Static;
        }

        // Folding the collider offset into each shape here costs nothing at run time.
        const bool bHasColliderOffset = Math::LengthSquared(ColliderTranslationOffset) > LE_SMALL_NUMBER
                                     || Math::LengthSquared(ColliderRotationOffset) > LE_SMALL_NUMBER;
        if (bHasColliderOffset)
        {
            const FQuat OffsetRotation(ColliderRotationOffset);
            const b3Transform Offset = Box3DUtils::ToB3Transform(ColliderTranslationOffset, OffsetRotation);

            for (FPendingShape& Shape : Out.Shapes)
            {
                switch (Shape.Type)
                {
                    case b3_sphereShape:
                        Shape.Sphere.center = b3TransformPoint(Offset, Shape.Sphere.center);
                        break;
                    case b3_capsuleShape:
                        Shape.Capsule.center1 = b3TransformPoint(Offset, Shape.Capsule.center1);
                        Shape.Capsule.center2 = b3TransformPoint(Offset, Shape.Capsule.center2);
                        break;
                    default:
                        Shape.Transform = b3MulTransforms(Offset, Shape.Transform);
                        break;
                }
            }
        }

        const FVector3 Position = Transform->GetLocation();
        const FQuat Rotation = Transform->GetRotation();

        Out.BodyDef = b3DefaultBodyDef();
        Out.BodyDef.type = Box3DUtils::ToBox3DBodyType(BodyType);
        Out.BodyDef.position = Box3DUtils::ToB3Vec3(Position);
        Out.BodyDef.rotation = Box3DUtils::ToB3Quat(Rotation);
        Out.BodyDef.linearDamping = RigidBody->LinearDamping;
        Out.BodyDef.angularDamping = RigidBody->AngularDamping;
        Out.BodyDef.gravityScale = RigidBody->bUseGravity ? 1.0f : 0.0f;
        Out.BodyDef.enableSleep = RigidBody->bAllowSleeping;
        Out.BodyDef.isBullet = RigidBody->bUseContinuousCollision;

        Out.BodyDef.motionLocks.linearX = RigidBody->bLockTranslationX;
        Out.BodyDef.motionLocks.linearY = RigidBody->bLockTranslationY;
        Out.BodyDef.motionLocks.linearZ = RigidBody->bLockTranslationZ;
        Out.BodyDef.motionLocks.angularX = RigidBody->bLockRotationX;
        Out.BodyDef.motionLocks.angularY = RigidBody->bLockRotationY;
        Out.BodyDef.motionLocks.angularZ = RigidBody->bLockRotationZ;

        Out.ShapeDef = b3DefaultShapeDef();
        Out.ShapeDef.filter = Box3DUtils::MakeShapeFilter(RigidBody->CollisionProfile);
        Out.ShapeDef.userData = Box3DUtils::PackProfileUserData(RigidBody->CollisionProfile);
        Out.ShapeDef.enableCustomFiltering = Box3DUtils::UsesPermissiveCollisionFilter();
        Out.ShapeDef.isSensor = RigidBody->bIsSensor || bColliderIsTrigger;
        Out.ShapeDef.enableSensorEvents = Out.ShapeDef.isSensor;
        Out.ShapeDef.enableContactEvents = true;
        Out.ShapeDef.enableHitEvents = true;

        // Scanning the world on creation is the dominant cost of a bulk static spawn.
        Out.ShapeDef.invokeContactCreation = BodyType != EBodyType::Static;

        if (ResolvedMaterial != nullptr)
        {
            Out.ShapeDef.baseMaterial.friction = ResolvedMaterial->Friction;
            Out.ShapeDef.baseMaterial.restitution = ResolvedMaterial->Restitution;
            Out.bHasMaterial = true;
            Out.MaterialFriction = ResolvedMaterial->Friction;
            Out.MaterialRestitution = ResolvedMaterial->Restitution;
            Out.MaterialFrictionCombine = (uint8)ResolvedMaterial->FrictionCombine;
            Out.MaterialRestitutionCombine = (uint8)ResolvedMaterial->RestitutionCombine;
        }
        else
        {
            Out.ShapeDef.baseMaterial.friction = RigidBody->FrictionOverride;
            Out.ShapeDef.baseMaterial.restitution = RigidBody->RestitutionOverride;
        }

        if (const SConveyorComponent* Conveyor = Registry.TryGet<SConveyorComponent>(Entity))
        {
            Out.SurfaceLinearVelocity = Conveyor->SurfaceVelocity;
            Out.SurfaceAngularVelocity = Conveyor->AngularSurfaceVelocity;
            Out.ShapeDef.baseMaterial.tangentVelocity = Box3DUtils::ToB3Vec3(Conveyor->SurfaceVelocity);
        }

        Out.ShapeDef.baseMaterial.userMaterialId = Out.bHasMaterial
            ? PackMaterialId(Out.MaterialFrictionCombine, Out.MaterialRestitutionCombine)
            : 0;

        Out.bOverrideMass = RigidBody->bOverrideMass || RigidBody->bOverrideInertia;
        Out.bOverrideInertia = RigidBody->bOverrideInertia;

        // Body local space is unscaled, since the entity scale is baked into the shapes themselves.
        Out.CenterOfMassOffset = RigidBody->CenterOfMassOffset * Scale;
        Out.InertiaTensor = RigidBody->InertiaTensor;
        Out.ComputedMass = RigidBody->Mass;
        Out.LastBodyPosition = Position;
        Out.LastBodyRotation = Rotation;

        return EBodyBuildStatus::Success;
    }

    uint32 FBox3DPhysicsScene::CommitRigidBody(ECS::FEntity Entity, FRigidBodyBuildResult& Build)
    {
        LUMINA_PROFILE_SCOPE();

        // b3CreateHeightFieldShape takes no transform and indexes the grid from the body origin, so a
        // height field's local offset has to be folded into the body before it is created.
        for (const FPendingShape& Shape : Build.Shapes)
        {
            if (Shape.Type != b3_heightShape || b3LengthSquared(Shape.Transform.p) <= 0.0f)
            {
                continue;
            }

            const b3Vec3 WorldOffset = b3RotateVector(Build.BodyDef.rotation, Shape.Transform.p);
            Build.BodyDef.position = b3Add(Build.BodyDef.position, WorldOffset);
            Build.LastBodyPosition = Box3DUtils::FromB3Vec3(Build.BodyDef.position);
            break;
        }

        const b3BodyId BodyId = b3CreateBody(WorldId, &Build.BodyDef);
        if (!b3Body_IsValid(BodyId))
        {
            return InvalidBodyHandle;
        }

        const uint32 Handle = RegisterBody(BodyId);
        b3Body_SetUserData(BodyId, PackBodyUserData(Entity, Handle));

        // Mass is derived once after every shape lands rather than re-integrated on each attach.
        Build.ShapeDef.updateBodyMass = false;

        for (const FPendingShape& Shape : Build.Shapes)
        {
            switch (Shape.Type)
            {
                case b3_sphereShape:
                    b3CreateSphereShape(BodyId, &Build.ShapeDef, &Shape.Sphere);
                    break;
                case b3_capsuleShape:
                    b3CreateCapsuleShape(BodyId, &Build.ShapeDef, &Shape.Capsule);
                    break;
                case b3_hullShape:
                    b3CreateTransformedHullShape(BodyId, &Build.ShapeDef, Shape.Hull, Shape.Transform, Shape.Scale);
                    break;
                case b3_meshShape:
                    b3CreateMeshShape(BodyId, &Build.ShapeDef, Shape.Mesh, Shape.Scale);
                    break;
                case b3_heightShape:
                    b3CreateHeightFieldShape(BodyId, &Build.ShapeDef, Shape.HeightField);
                    break;
                default:
                    break;
            }
        }

        b3Body_ApplyMassFromShapes(BodyId);

        const bool bShiftCenterOfMass = Math::LengthSquared(Build.CenterOfMassOffset) > LE_SMALL_NUMBER;

        if (Build.BodyDef.type == b3_dynamicBody && (Build.bOverrideMass || bShiftCenterOfMass))
        {
            b3MassData MassData = b3Body_GetMassData(BodyId);
            MassData.center = b3Add(MassData.center, Box3DUtils::ToB3Vec3(Build.CenterOfMassOffset));

            if (Build.bOverrideInertia)
            {
                MassData.inertia = b3Matrix3{
                    b3Vec3{ Build.InertiaTensor.x, 0.0f, 0.0f },
                    b3Vec3{ 0.0f, Build.InertiaTensor.y, 0.0f },
                    b3Vec3{ 0.0f, 0.0f, Build.InertiaTensor.z } };
            }
            else
            {
                // Keeps the shape-derived inertia distribution and rescales it onto the authored mass.
                const float Ratio = MassData.mass > 0.0f ? Build.ComputedMass / MassData.mass : 1.0f;
                MassData.inertia.cx = b3MulSV(Ratio, MassData.inertia.cx);
                MassData.inertia.cy = b3MulSV(Ratio, MassData.inertia.cy);
                MassData.inertia.cz = b3MulSV(Ratio, MassData.inertia.cz);
            }

            if (Build.bOverrideMass)
            {
                MassData.mass = Build.ComputedMass;
            }
            b3Body_SetMassData(BodyId, MassData);
        }

        StoreBodyMaterial(Handle, Build);

        return Handle;
    }
}
