#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DCharacterHandle.h"
#include "Box3DPhysics.h"
#include "Box3DRagdollHandle.h"
#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Entity/Systems/DebugDrawSystem.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/World.h"

namespace Lumina::Physics
{
    namespace
    {
        // Box3D's native rule needs both sides to accept, so the engine's historical rule lives here instead.
        bool PermissivePairFilter(b3ShapeId ShapeA, b3ShapeId ShapeB, void*)
        {
            const FCollisionProfile ProfileA = Box3DUtils::UnpackProfileUserData(b3Shape_GetUserData(ShapeA));
            const FCollisionProfile ProfileB = Box3DUtils::UnpackProfileUserData(b3Shape_GetUserData(ShapeB));
            return ProfileA.ShouldCollide(ProfileB);
        }

        // The pair takes the stronger of the two modes, so a deliberately sticky surface always wins.
        FORCEINLINE float CombineUnder(EPhysicsMaterialCombineMode Mode, float A, float B)
        {
            switch (Mode)
            {
                case EPhysicsMaterialCombineMode::Min:      return Math::Min(A, B);
                case EPhysicsMaterialCombineMode::Multiply: return A * B;
                case EPhysicsMaterialCombineMode::Max:      return Math::Max(A, B);
                default:                                    return (A + B) * 0.5f;
            }
        }

        FORCEINLINE bool ResolveCombineMode(uint64 IdA, uint64 IdB, uint32 Shift, EPhysicsMaterialCombineMode& OutMode)
        {
            const bool bHasA = (IdA & MaterialIdPresentBit) != 0;
            const bool bHasB = (IdB & MaterialIdPresentBit) != 0;
            if (!bHasA && !bHasB)
            {
                return false;
            }

            const uint8 ModeA = bHasA ? (uint8)((IdA >> Shift) & 0xFF) : 0;
            const uint8 ModeB = bHasB ? (uint8)((IdB >> Shift) & 0xFF) : 0;
            OutMode = (EPhysicsMaterialCombineMode)Math::Max(ModeA, ModeB);
            return true;
        }

        float CombineFriction(float FrictionA, uint64_t IdA, float FrictionB, uint64_t IdB)
        {
            EPhysicsMaterialCombineMode Mode;
            if (!ResolveCombineMode(IdA, IdB, 0, Mode))
            {
                // No authored material on either side, so Box3D's own geometric mean applies.
                return Math::Sqrt(FrictionA * FrictionB);
            }
            return CombineUnder(Mode, FrictionA, FrictionB);
        }

        float CombineRestitution(float RestitutionA, uint64_t IdA, float RestitutionB, uint64_t IdB)
        {
            EPhysicsMaterialCombineMode Mode;
            if (!ResolveCombineMode(IdA, IdB, 8, Mode))
            {
                return Math::Max(RestitutionA, RestitutionB);
            }
            return CombineUnder(Mode, RestitutionA, RestitutionB);
        }
    }

    FBox3DPhysicsScene::FBox3DPhysicsScene(CWorld* InWorld)
        : World(InWorld)
    {
        const Lumina::SDefaultWorldSettings& InitSettings = World->GetDefaultWorldSettings();
        MaxBodies = Math::Max(InitSettings.MaxPhysicsBodies, 1u);

        b3WorldDef Def = b3DefaultWorldDef();
        Def.workerCount = TaskBridge.GetWorkerCount();
        Def.enqueueTask = &FBox3DTaskBridge::EnqueueTask;
        Def.finishTask = &FBox3DTaskBridge::FinishTask;
        Def.userTaskContext = &TaskBridge;
        Def.userData = this;
        Def.createDebugShape = &FBox3DDebugRenderer::CreateDebugShape;
        Def.destroyDebugShape = &FBox3DDebugRenderer::DestroyDebugShape;
        Def.userDebugShapeContext = this;

        // Pre-sizing avoids the world reallocating its body and contact arrays during a spawn burst.
        Def.capacity.dynamicBodyCount = (int)MaxBodies;
        Def.capacity.staticBodyCount = (int)MaxBodies;
        Def.capacity.dynamicShapeCount = (int)MaxBodies;
        Def.capacity.staticShapeCount = (int)MaxBodies;
        Def.capacity.contactCount = (int)InitSettings.MaxPhysicsContactConstraints;

        WorldId = b3CreateWorld(&Def);

        LOG_DISPLAY("[Box3D] Scene created with {} solver worker(s), capacity {} bodies.",
            TaskBridge.GetWorkerCount(), MaxBodies);

        BodyMaterials.resize(MaxBodies);
        BodyHandles.reserve(MaxBodies);

        ApplyWorldSettings(InitSettings);

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.on_construct<SSphereColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SBoxColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCapsuleColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCylinderColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCapsuleColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCylinderColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SPlaneColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCollisionShapeComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCompoundColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SMeshColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STerrainColliderComponent>().connect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
    }

    FBox3DPhysicsScene::~FBox3DPhysicsScene()
    {
        DestroyAllConstraints();
        DestroyAllStaticBodyGroups();

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.on_construct<SSphereColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SBoxColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCapsuleColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCylinderColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCapsuleColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCylinderColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SPlaneColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCollisionShapeComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SCompoundColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<SMeshColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();
        Registry.on_construct<STerrainColliderComponent>().disconnect<&entt::registry::get_or_emplace<SRigidBodyComponent>>();

        if (b3World_IsValid(WorldId))
        {
            b3DestroyWorld(WorldId);
            WorldId = b3_nullWorldId;
        }

        // Geometry outlives the shapes that reference it, so it is released only once the world is gone.
        DestroyGeometryCaches();
    }

    void FBox3DPhysicsScene::TrackOwnedHull(b3HullData* Hull)
    {
        FScopeLock Lock(OwnedGeometryMutex);
        OwnedHulls.push_back(Hull);
    }

    void FBox3DPhysicsScene::TrackOwnedMesh(b3MeshData* Mesh)
    {
        FScopeLock Lock(OwnedGeometryMutex);
        OwnedMeshes.push_back(Mesh);
    }

    void FBox3DPhysicsScene::TrackOwnedHeightField(b3HeightFieldData* Field)
    {
        FScopeLock Lock(OwnedGeometryMutex);
        OwnedHeightFields.push_back(Field);
    }

    void FBox3DPhysicsScene::DestroyGeometryCaches()
    {
        for (auto& [Key, Hull] : HullCache)
        {
            b3DestroyHull(Hull);
        }
        HullCache.clear();

        for (auto& [Key, Mesh] : MeshCache)
        {
            b3DestroyMesh(Mesh);
        }
        MeshCache.clear();

        for (b3HeightFieldData* Field : OwnedHeightFields)
        {
            b3DestroyHeightField(Field);
        }
        OwnedHeightFields.clear();

        for (b3HullData* Hull : OwnedHulls)
        {
            b3DestroyHull(Hull);
        }
        OwnedHulls.clear();

        for (b3MeshData* Mesh : OwnedMeshes)
        {
            b3DestroyMesh(Mesh);
        }
        OwnedMeshes.clear();
    }

    bool FBox3DPhysicsScene::WorldSettingsChanged(const Lumina::SDefaultWorldSettings& Settings)
    {
        uint64 Hash = 1469598103934665603ull;
        auto Mix = [&Hash](float Value)
        {
            uint32 Bits;
            std::memcpy(&Bits, &Value, sizeof(Bits));
            Hash = (Hash ^ Bits) * 1099511628211ull;
        };

        Mix(Settings.GravityDirection.x); Mix(Settings.GravityDirection.y); Mix(Settings.GravityDirection.z);
        Mix(Settings.GravityScale);
        Mix(Settings.ContactHertz); Mix(Settings.ContactDampingRatio); Mix(Settings.ContactPushSpeed);
        Mix(Settings.RestitutionThreshold); Mix(Settings.HitEventThreshold); Mix(Settings.MaxLinearSpeed);
        Mix(Settings.bAllowSleeping ? 1.0f : 0.0f);
        Mix(Settings.bEnableContinuousCollision ? 1.0f : 0.0f);
        Mix(Settings.bEnableSpeculativeContacts ? 1.0f : 0.0f);
        Mix(Settings.bConstraintWarmStart ? 1.0f : 0.0f);

        if (Hash == WorldSettingsHash)
        {
            return false;
        }

        WorldSettingsHash = Hash;
        return true;
    }

    void FBox3DPhysicsScene::ApplyWorldSettings(const Lumina::SDefaultWorldSettings& Settings)
    {
        const FVector3 GravityDir = Math::LengthSquared(Settings.GravityDirection) > 0.0f
            ? Math::Normalize(Settings.GravityDirection)
            : FVector3(0.0f, -1.0f, 0.0f);

        b3World_SetGravity(WorldId, Box3DUtils::ToB3Vec3(GravityDir * Math::Abs(GEarthGravity) * Settings.GravityScale));

        b3World_EnableSleeping(WorldId, Settings.bAllowSleeping);
        b3World_EnableContinuous(WorldId, Settings.bEnableContinuousCollision);
        b3World_EnableSpeculative(WorldId, Settings.bEnableSpeculativeContacts);
        b3World_EnableWarmStarting(WorldId, Settings.bConstraintWarmStart);
        b3World_SetContactTuning(WorldId, Settings.ContactHertz, Settings.ContactDampingRatio, Settings.ContactPushSpeed);
        b3World_SetRestitutionThreshold(WorldId, Settings.RestitutionThreshold);
        b3World_SetHitEventThreshold(WorldId, Settings.HitEventThreshold);
        b3World_SetMaximumLinearSpeed(WorldId, Settings.MaxLinearSpeed);

        // Per-material combine modes only exist through these, so they are installed with the rest of the tuning.
        b3World_SetFrictionCallback(WorldId, &CombineFriction);
        b3World_SetRestitutionCallback(WorldId, &CombineRestitution);

        b3World_SetCustomFilterCallback(WorldId,
            Box3DUtils::UsesPermissiveCollisionFilter() ? &PermissivePairFilter : nullptr, this);
    }

    b3BodyId FBox3DPhysicsScene::ResolveBody(uint32 BodyID) const
    {
        if (BodyID >= BodyHandles.size())
        {
            return b3_nullBodyId;
        }
        return BodyHandles[BodyID];
    }

    uint32 FBox3DPhysicsScene::RegisterBody(b3BodyId BodyId)
    {
        if (!FreeBodyHandles.empty())
        {
            const uint32 Handle = FreeBodyHandles.back();
            FreeBodyHandles.pop_back();
            BodyHandles[Handle] = BodyId;
            return Handle;
        }

        BodyHandles.push_back(BodyId);
        return (uint32)(BodyHandles.size() - 1);
    }

    void FBox3DPhysicsScene::UnregisterBody(uint32 BodyID)
    {
        if (BodyID >= BodyHandles.size())
        {
            return;
        }

        BodyHandles[BodyID] = b3_nullBodyId;
        FreeBodyHandles.push_back(BodyID);

        // The handle can be reused this same frame, so it must not inherit the dead body's staging slot.
        if (BodyID < BodyStagingSlot.size())
        {
            BodyStagingSlot[BodyID] = InvalidBodyHandle;
        }
    }

    void FBox3DPhysicsScene::PreUpdate()
    {
    }

    void FBox3DPhysicsScene::PostUpdate()
    {
    }

    uint32 FBox3DPhysicsScene::GetEntityBodyID(entt::entity Entity)
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        if (const SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Entity))
        {
            return Body->BodyID;
        }

        if (const SCharacterPhysicsComponent* Character = Registry.try_get<SCharacterPhysicsComponent>(Entity))
        {
            if (Character->Character)
            {
                return Character->Character->ProxyBodyHandle;
            }
        }

        return InvalidBodyHandle;
    }

    uint32 FBox3DPhysicsScene::GetBodyCount()
    {
        return (uint32)(BodyHandles.size() - FreeBodyHandles.size());
    }

    uint32 FBox3DPhysicsScene::GetMaxBodyCount()
    {
        return MaxBodies;
    }

    bool FBox3DPhysicsScene::IsBodyActive(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) && b3Body_IsAwake(Body);
    }

    void FBox3DPhysicsScene::ActivateBody(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetAwake(Body, true);
        }
    }

    void FBox3DPhysicsScene::DeactivateBody(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetAwake(Body, false);
        }
    }

    void FBox3DPhysicsScene::ChangeBodyMotionType(uint32 BodyID, EBodyType NewType)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetType(Body, Box3DUtils::ToBox3DBodyType(NewType));
        }
    }

    FVector3 FBox3DPhysicsScene::GetLinearVelocity(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? Box3DUtils::FromB3Vec3(b3Body_GetLinearVelocity(Body)) : FVector3(0.0f);
    }

    FVector3 FBox3DPhysicsScene::GetAngularVelocity(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? Box3DUtils::FromB3Vec3(b3Body_GetAngularVelocity(Body)) : FVector3(0.0f);
    }

    FVector3 FBox3DPhysicsScene::GetVelocityAtPoint(uint32 BodyID, const FVector3& Point)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body)
            ? Box3DUtils::FromB3Vec3(b3Body_GetWorldPointVelocity(Body, Box3DUtils::ToB3Vec3(Point)))
            : FVector3(0.0f);
    }

    FVector3 FBox3DPhysicsScene::GetCenterOfMass(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? Box3DUtils::FromB3Vec3(b3Body_GetWorldCenter(Body)) : FVector3(0.0f);
    }

    float FBox3DPhysicsScene::GetBodyMass(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? b3Body_GetMass(Body) : 0.0f;
    }

    FVector3 FBox3DPhysicsScene::GetBodyPosition(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? Box3DUtils::FromB3Vec3(b3Body_GetPosition(Body)) : FVector3(0.0f);
    }

    FQuat FBox3DPhysicsScene::GetBodyRotation(uint32 BodyID)
    {
        const b3BodyId Body = ResolveBody(BodyID);
        return b3Body_IsValid(Body) ? Box3DUtils::FromB3Quat(b3Body_GetRotation(Body)) : FQuat::Identity();
    }

    void FBox3DPhysicsScene::OnImpulseEvent(const SImpulseEvent& Impulse)
    {
        const b3BodyId Body = ResolveBody(Impulse.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyLinearImpulseToCenter(Body, Box3DUtils::ToB3Vec3(Impulse.Impulse), true);
        }
    }

    void FBox3DPhysicsScene::OnForceEvent(const SForceEvent& Force)
    {
        const b3BodyId Body = ResolveBody(Force.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyForceToCenter(Body, Box3DUtils::ToB3Vec3(Force.Force), true);
        }
    }

    void FBox3DPhysicsScene::OnTorqueEvent(const STorqueEvent& Torque)
    {
        const b3BodyId Body = ResolveBody(Torque.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyTorque(Body, Box3DUtils::ToB3Vec3(Torque.Torque), true);
        }
    }

    void FBox3DPhysicsScene::OnAngularImpulseEvent(const SAngularImpulseEvent& AngularImpulse)
    {
        const b3BodyId Body = ResolveBody(AngularImpulse.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyAngularImpulse(Body, Box3DUtils::ToB3Vec3(AngularImpulse.AngularImpulse), true);
        }
    }

    void FBox3DPhysicsScene::OnSetVelocityEvent(const SSetVelocityEvent& Velocity)
    {
        const b3BodyId Body = ResolveBody(Velocity.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetLinearVelocity(Body, Box3DUtils::ToB3Vec3(Velocity.Velocity));
            b3Body_SetAwake(Body, true);
        }
    }

    void FBox3DPhysicsScene::OnSetAngularVelocityEvent(const SSetAngularVelocityEvent& AngularVelocity)
    {
        const b3BodyId Body = ResolveBody(AngularVelocity.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetAngularVelocity(Body, Box3DUtils::ToB3Vec3(AngularVelocity.AngularVelocity));
            b3Body_SetAwake(Body, true);
        }
    }

    void FBox3DPhysicsScene::OnAddImpulseAtPositionEvent(const SAddImpulseAtPositionEvent& Event)
    {
        const b3BodyId Body = ResolveBody(Event.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyLinearImpulse(Body, Box3DUtils::ToB3Vec3(Event.Impulse), Box3DUtils::ToB3Vec3(Event.Position), true);
        }
    }

    void FBox3DPhysicsScene::OnAddForceAtPositionEvent(const SAddForceAtPositionEvent& Event)
    {
        const b3BodyId Body = ResolveBody(Event.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_ApplyForce(Body, Box3DUtils::ToB3Vec3(Event.Force), Box3DUtils::ToB3Vec3(Event.Position), true);
        }
    }

    void FBox3DPhysicsScene::OnSetGravityFactorEvent(const SSetGravityFactorEvent& Event)
    {
        const b3BodyId Body = ResolveBody(Event.BodyID);
        if (b3Body_IsValid(Body))
        {
            b3Body_SetGravityScale(Body, Event.GravityFactor);
        }
    }

    void FBox3DPhysicsScene::StoreBodyMaterial(uint32 BodyID, const FRigidBodyBuildResult& Build)
    {
        if (BodyID >= BodyMaterials.size())
        {
            BodyMaterials.resize(BodyID + 1);
        }

        FBodyMaterialEntry& Entry = BodyMaterials[BodyID];
        Entry.bHasMaterial = Build.bHasMaterial;
        Entry.Friction = Build.MaterialFriction;
        Entry.Restitution = Build.MaterialRestitution;
        Entry.FrictionCombine = Build.MaterialFrictionCombine;
        Entry.RestitutionCombine = Build.MaterialRestitutionCombine;
    }

    void FBox3DPhysicsScene::ClearBodyMaterial(uint32 BodyID)
    {
        if (BodyID < BodyMaterials.size())
        {
            BodyMaterials[BodyID] = FBodyMaterialEntry{};
        }
    }

    void FBox3DPhysicsScene::SetSurfaceVelocity(entt::entity Entity, const FVector3& Linear, const FVector3& Angular)
    {
        const b3BodyId Body = ResolveBody(GetEntityBodyID(Entity));
        if (!b3Body_IsValid(Body))
        {
            return;
        }

        // Box3D carries conveyor motion on the surface material, so it is set per shape rather than per body.
        b3ShapeId Shapes[16];
        const int32 Count = b3Body_GetShapes(Body, Shapes, 16);
        for (int32 i = 0; i < Count; ++i)
        {
            b3SurfaceMaterial Material = b3Shape_GetSurfaceMaterial(Shapes[i]);
            Material.tangentVelocity = Box3DUtils::ToB3Vec3(Linear);
            b3Shape_SetSurfaceMaterial(Shapes[i], Material);
        }

        if (Math::LengthSquared(Angular) > 0.0f)
        {
            // Box3D has no angular surface drive, so a rotating platform needs a kinematic body instead.
            static bool bWarned = false;
            if (!bWarned)
            {
                bWarned = true;
                LOG_WARN("Angular surface velocity is not supported by Box3D; use a kinematic body for rotating platforms.");
            }
        }
    }
}
