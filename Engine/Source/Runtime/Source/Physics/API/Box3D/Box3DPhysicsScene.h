#pragma once

#include <box3d/box3d.h>

#include "Box3DTaskBridge.h"
#include "Containers/HashTable.h"
#include "Containers/Queue.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsScene.h"
#include "Renderer/SkeletonResource.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Events/ImpulseEvent.h"

namespace Lumina
{
    struct SCompoundColliderComponent;
    struct SDefaultWorldSettings;
    struct STransformComponent;
    class CCollisionShape;
    class CMesh;
    class CWorld;
}

namespace Lumina::Physics
{
    enum class EContactEventType : uint8
    {
        Added,
        Removed,
    };

    // Outcome of the body build, deciding whether the caller commits, retries later, or drops the entity.
    enum class EBodyBuildStatus : uint8
    {
        Success,
        Defer,
        AlreadyExists,
        NoCollider,
        Error,
    };

    // One shape attached to a pending body, in body-local space.
    struct FPendingShape
    {
        b3ShapeType             Type = b3_sphereShape;
        b3Sphere                Sphere{};
        b3Capsule               Capsule{};
        const b3HullData*       Hull = nullptr;
        const b3MeshData*       Mesh = nullptr;
        const b3HeightFieldData* HeightField = nullptr;
        b3Transform             Transform{ { 0.0f, 0.0f, 0.0f }, { { 0.0f, 0.0f, 0.0f }, 1.0f } };
        b3Vec3                  Scale{ 1.0f, 1.0f, 1.0f };
    };

    struct FRigidBodyBuildResult
    {
        b3BodyDef               BodyDef{};
        b3ShapeDef              ShapeDef{};
        TVector<FPendingShape>  Shapes;

        FVector3                LastBodyPosition = FVector3(0.0f);
        FQuat                   LastBodyRotation = FQuat::Identity();
        float                   ComputedMass = 0.0f;
        bool                    bOverrideMass = false;
        FVector3                InertiaTensor = FVector3(0.0f);
        bool                    bOverrideInertia = false;
        FVector3                CenterOfMassOffset = FVector3(0.0f);

        bool                    bHasMaterial = false;
        float                   MaterialFriction = 0.0f;
        float                   MaterialRestitution = 0.0f;
        uint8                   MaterialFrictionCombine = 0;
        uint8                   MaterialRestitutionCombine = 0;

        FVector3                SurfaceLinearVelocity = FVector3(0.0f);
        FVector3                SurfaceAngularVelocity = FVector3(0.0f);
    };

    // Contact snapshot resolved from Box3D's post-step event arrays and dispatched on the game thread.
    struct FContactRecord
    {
        EContactEventType   Type;
        entt::entity        EntityA;
        entt::entity        EntityB;
        uint32              BodyIDA;
        uint32              BodyIDB;
        FVector3            Point;
        FVector3            Normal;
        FVector3            VelocityA;
        FVector3            VelocityB;
        float               ImpactSpeed;
        bool                bSensorA;
        bool                bSensorB;
    };

    class FBox3DPhysicsScene : public IPhysicsScene
    {
    public:

        explicit FBox3DPhysicsScene(CWorld* InWorld);
        ~FBox3DPhysicsScene() override;

        LE_NO_COPYMOVE(FBox3DPhysicsScene);

        void PreUpdate() override;
        void Update(double DeltaTime) override;
        void PostUpdate() override;
        void Simulate() override;
        void StopSimulate() override;

        void DispatchPendingEvents() override;

        void ActivateBody(uint32 BodyID) override;
        void DeactivateBody(uint32 BodyID) override;
        void ChangeBodyMotionType(uint32 BodyID, EBodyType NewType) override;
        bool IsBodyActive(uint32 BodyID) override;

        void ApplyDirtyTransforms(float FixedDt);
        void UpdateCharacters(float FixedDt);
        void LatchCharacterInput();
        void BuildInterpolatedTransforms(float Alpha);
        void ApplyInterpolatedTransforms();

        uint32 GetEntityBodyID(entt::entity Entity) override;

        TOptional<SRayResult> CastRay(const SRayCastSettings& Settings) override;
        void CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) override;
        TOptional<SRayResult> CastSphereClosest(const SSphereCastSettings& Settings) override;
        void CastRayAll(const SRayCastSettings& Settings, TVector<SRayResult>& OutHits) override;

        int32 ResolveHitBoneIndex(entt::entity Entity, b3BodyId BodyId) const;
        int32 CollidePoint(const FVector3& Point, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) override;
        int32 OverlapSphere(const FVector3& Center, float Radius, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) override;
        int32 OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) override;

        void OnCharacterComponentConstructed(entt::registry& Registry, entt::entity Entity);
        void OnCharacterComponentDestroyed(entt::registry& Registry, entt::entity Entity);

        void OnRigidBodyComponentUpdated(entt::registry& Registry, entt::entity Entity);
        void OnRigidBodyComponentConstructed(entt::registry& Registry, entt::entity Entity);
        void OnRigidBodyComponentDestroyed(entt::registry& Registry, entt::entity Entity);
        void OnColliderComponentAdded(entt::registry& Registry, entt::entity Entity);
        void OnColliderComponentRemoved(entt::registry& Registry, entt::entity Entity);

        void OnConstraintComponentConstructed(entt::registry& Registry, entt::entity Entity);
        void OnConstraintComponentDestroyed(entt::registry& Registry, entt::entity Entity);

        void OnImpulseEvent(const SImpulseEvent& Impulse) override;
        void OnForceEvent(const SForceEvent& Force) override;
        void OnTorqueEvent(const STorqueEvent& Torque) override;
        void OnAngularImpulseEvent(const SAngularImpulseEvent& AngularImpulse) override;
        void OnSetVelocityEvent(const SSetVelocityEvent& Velocity) override;
        void OnSetAngularVelocityEvent(const SSetAngularVelocityEvent& AngularVelocity) override;
        void OnAddImpulseAtPositionEvent(const SAddImpulseAtPositionEvent& Event) override;
        void OnAddForceAtPositionEvent(const SAddForceAtPositionEvent& Event) override;
        void OnSetGravityFactorEvent(const SSetGravityFactorEvent& Event) override;

        void ApplyBuoyancyImpulse(entt::entity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime) override;

        FVector3 GetVelocityAtPoint(uint32 BodyID, const FVector3& Point) override;
        FVector3 GetLinearVelocity(uint32 BodyID) override;
        FVector3 GetAngularVelocity(uint32 BodyID) override;
        FVector3 GetCenterOfMass(uint32 BodyID) override;
        float GetBodyMass(uint32 BodyID) override;
        FVector3 GetBodyPosition(uint32 BodyID) override;
        FQuat GetBodyRotation(uint32 BodyID) override;

        uint32 GetBodyCount() override;
        uint32 GetMaxBodyCount() override;

        void BeginBodyBatch() override;
        void EndBodyBatch() override;

        uint32 CreateStaticBodyGroup(entt::entity Owner, TSpan<const FStaticInstanceDesc> Instances) override;
        void DestroyStaticBodyGroup(uint32 GroupID) override;

        TSharedPtr<FPhysicsRagdollHandle> CreateRagdoll(const FRagdollDesc& Desc) override;
        void ReadRagdollPose(const FPhysicsRagdollHandle& Handle, const FMatrix4& WorldToEntity, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms) override;
        void DestroyRagdoll(const TSharedPtr<FPhysicsRagdollHandle>& Handle) override;
        void GetRagdollRootTransform(const FPhysicsRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation) override;
        uint32 AllocateRagdollGroupID() override { return NextRagdollGroupID++; }

        uint32 CreateConstraint(const FConstraintDesc& Desc) override;
        void DestroyConstraint(uint32 ConstraintID) override;
        void SetConstraintEnabled(uint32 ConstraintID, bool bEnabled) override;
        void SetConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target) override;
        bool IsConstraintBroken(uint32 ConstraintID) override;
        float GetConstraintValue(uint32 ConstraintID) override;

        void SetSurfaceVelocity(entt::entity Entity, const FVector3& Linear, const FVector3& Angular) override;

        b3WorldId GetWorldId() const { return WorldId; }

        // Handle table translating the engine's opaque uint32 body id to Box3D's generational b3BodyId.
        b3BodyId ResolveBody(uint32 BodyID) const;
        uint32 RegisterBody(b3BodyId BodyId);
        void UnregisterBody(uint32 BodyID);

        const b3HullData* GetOrCreateBoxHull(const FVector3& HalfExtent);
        const b3HullData* GetOrCreateCylinderHull(float Radius, float HalfHeight);
        const b3HullData* GetOrCreateTaperedCylinderHull(float HalfHeight, float TopRadius, float BottomRadius);
        const b3HullData* GetOrCreateMeshHull(const CMesh* Mesh);
        const b3MeshData* GetOrCreateTriangleMesh(const CMesh* Mesh);

        bool BuildCompoundShapes(const SCompoundColliderComponent& Comp, const STransformComponent& Transform, TVector<FPendingShape>& OutShapes);
        bool BuildCollisionShapeAsset(const CCollisionShape& Asset, const FVector3& Scale, TVector<FPendingShape>& OutShapes);

        struct FBodyMaterialEntry
        {
            float   Friction = 0.0f;
            float   Restitution = 0.0f;
            uint8   FrictionCombine = 0;
            uint8   RestitutionCombine = 0;
            bool    bHasMaterial = false;
        };

        void StoreBodyMaterial(uint32 BodyID, const FRigidBodyBuildResult& Build);
        void ClearBodyMaterial(uint32 BodyID);

    private:

        void DispatchContactEvents();
        void DispatchActivationEvents();
        void DrainStepEvents();

        // Interpolation is driven by Box3D's move events, so a sleeping or static body costs nothing here.
        void DrainMoveEvents(bool bStageForInterp);
        uint32 StageInterpSlot(uint32 BodyHandle, const FVector3& Position, const FQuat& Rotation);
        void ResetInterpStaging();

        void BulkCreateRigidBodies(entt::registry& Registry);
        void CreateRigidBodiesBatched(const TVector<entt::entity>& Entities);
        void CreateRigidBodyImmediate(entt::registry& Registry, entt::entity Entity);
        void RebuildStaleDynamicMeshBodies(entt::registry& Registry);

        EBodyBuildStatus TryBuildRigidBody(entt::registry& Registry, entt::entity Entity, FRigidBodyBuildResult& OutResult);
        uint32 CommitRigidBody(entt::entity Entity, FRigidBodyBuildResult& Build);

        bool TryCreateComponentConstraint(entt::registry& Registry, entt::entity Entity);
        void DrainPendingConstraints();
        void MonitorBreakableConstraints(float Dt);
        void DestroyAllConstraints();
        void DestroyAllStaticBodyGroups();
        void DestroyGeometryCaches();
        void ApplyWorldSettings(const Lumina::SDefaultWorldSettings& Settings);
        bool WorldSettingsChanged(const Lumina::SDefaultWorldSettings& Settings);

    private:

        struct FHullKey
        {
            uint8   Kind = 0;
            float   X = 0.0f, Y = 0.0f, Z = 0.0f;
            const void* Source = nullptr;

            bool operator==(const FHullKey& Other) const
            {
                return Kind == Other.Kind && X == Other.X && Y == Other.Y && Z == Other.Z && Source == Other.Source;
            }
        };

        struct FHullKeyHash
        {
            size_t operator()(const FHullKey& Key) const
            {
                size_t Hash = Key.Kind;
                auto Mix = [&Hash](uint32 Bits) { Hash = (Hash * 1099511628211ull) ^ Bits; };
                auto MixFloat = [&Mix](float Value)
                {
                    uint32 Bits;
                    std::memcpy(&Bits, &Value, sizeof(Bits));
                    Mix(Bits);
                };
                MixFloat(Key.X); MixFloat(Key.Y); MixFloat(Key.Z);
                Mix((uint32)(reinterpret_cast<uintptr_t>(Key.Source) & 0xFFFFFFFFu));
                Mix((uint32)(reinterpret_cast<uintptr_t>(Key.Source) >> 32));
                return Hash;
            }
        };

        mutable FSharedMutex                                HullCacheMutex;
        THashMap<FHullKey, b3HullData*, FHullKeyHash>       HullCache;

        mutable FSharedMutex                                MeshCacheMutex;
        THashMap<const void*, b3MeshData*>                  MeshCache;

        TVector<b3HeightFieldData*>                         OwnedHeightFields;

        void TrackOwnedHull(b3HullData* Hull);
        void TrackOwnedMesh(b3MeshData* Mesh);
        void TrackOwnedHeightField(b3HeightFieldData* Field);

        // Geometry unique to one asset or component, so a shared cache would never hit; freed with the scene.
        // Guarded because the parallel body build appends to them.
        mutable FMutex                                      OwnedGeometryMutex;
        TVector<b3HullData*>                                OwnedHulls;
        TVector<b3MeshData*>                                OwnedMeshes;

        struct FDeferredBodyUpdate
        {
            entt::entity            Entity;
            FNeedsPhysicsBodyUpdate Update;
        };
        TVector<FDeferredBodyUpdate>            RetryBodyUpdates;

        FMutex                                  PendingRigidBodyMutex;
        TQueue<entt::entity>                    PendingRigidBodyCreations;

        TAtomic<bool>                           bStepInProgress{ false };

        int32                                   BodyBatchDepth = 0;
        TVector<entt::entity>                   BatchedBodyCreations;
        TVector<entt::entity>                   BatchedCharacterCreations;

        TVector<FRigidBodyBuildResult>          BatchBuildScratch;
        TVector<EBodyBuildStatus>               BatchStatusScratch;
        TVector<entt::entity>                   PendingDrainScratch;

        FBox3DTaskBridge                        TaskBridge;
        b3WorldId                               WorldId{};
        CWorld*                                 World = nullptr;

        // Dense handle table so the engine's uint32 body id survives Box3D's generational ids.
        TVector<b3BodyId>                       BodyHandles;
        TVector<uint32>                         FreeBodyHandles;

        TVector<FContactRecord>                 ContactDrainScratch;

        struct FActivationRecord
        {
            entt::entity    Entity;
            bool            bActivated;
        };
        TVector<FActivationRecord>              ActivationDrainScratch;

        TVector<FBodyMaterialEntry>             BodyMaterials;

        float                                   Accumulator = 0.0f;
        uint32                                  CollisionSteps = 0;
        uint32                                  MaxBodies = 0;

        // Hash of the settings last pushed, so an unedited frame skips the setter calls entirely.
        uint64                                  WorldSettingsHash = 0;

        uint32                                  NextRagdollGroupID = 1;

        THashMap<uint32, TVector<uint32>>       StaticBodyGroups;
        uint32                                  NextStaticBodyGroupID = 1;

        struct FBox3DConstraint
        {
            b3JointId               JointId{};
            EPhysicsConstraintType  Type = EPhysicsConstraintType::Point;
            float                   BreakForce = 0.0f;
            float                   MotorForceLimit = 0.0f;
            float                   MotorTorqueLimit = 0.0f;
            bool                    bBroken = false;
            bool                    bEnabled = true;
        };
        FMutex                                  ConstraintsMutex;
        THashMap<uint32, FBox3DConstraint>      Constraints;
        uint32                                  NextConstraintID = 1;

        FMutex                                  PendingConstraintMutex;
        TVector<entt::entity>                   PendingConstraintCreations;

        enum class EInterpFlag : uint8 { Interpolate = 0, Skip = 1, BelowKill = 2 };

        // Grow-only SoA holding only the bodies that actually moved this frame. Rotations are deinterleaved
        // to x/y/z/w so the nlerp vectorizes.
        struct FInterpStaging
        {
            TVector<entt::entity>   Entities;
            TVector<EInterpFlag>    Flags;

            TVector<FVector3>       PrevPos;
            TVector<FVector3>       CurrPos;
            TVector<FVector3>       LerpPos;

            TVector<float>          PrevQx, PrevQy, PrevQz, PrevQw;
            TVector<float>          CurrQx, CurrQy, CurrQz, CurrQw;
            TVector<float>          LerpQx, LerpQy, LerpQz, LerpQw;

            void PushBack()
            {
                Entities.emplace_back();
                Flags.emplace_back();
                PrevPos.emplace_back();
                CurrPos.emplace_back();
                PrevQx.emplace_back(); PrevQy.emplace_back(); PrevQz.emplace_back(); PrevQw.emplace_back();
                CurrQx.emplace_back(); CurrQy.emplace_back(); CurrQz.emplace_back(); CurrQw.emplace_back();
            }

            void EnsureLerpCapacity()
            {
                const size_t N = Entities.size();
                LerpPos.resize(N);
                LerpQx.resize(N); LerpQy.resize(N); LerpQz.resize(N); LerpQw.resize(N);
            }

            void Clear()
            {
                Entities.clear(); Flags.clear();
                PrevPos.clear();  CurrPos.clear();  LerpPos.clear();
                PrevQx.clear(); PrevQy.clear(); PrevQz.clear(); PrevQw.clear();
                CurrQx.clear(); CurrQy.clear(); CurrQz.clear(); CurrQw.clear();
                LerpQx.clear(); LerpQy.clear(); LerpQz.clear(); LerpQw.clear();
            }
        };
        FInterpStaging                          InterpStaging;
        TVector<uint32>                         InterpApplied;

        // Body handle to staging slot for this frame, reset through StagedBodyHandles so it stays O(moved).
        TVector<uint32>                         BodyStagingSlot;
        TVector<uint32>                         StagedBodyHandles;

        // Dense wake state per body handle, diffed against the move events to raise OnWake / OnSleep.
        TVector<uint8>                          BodyAwake;
    };
}
