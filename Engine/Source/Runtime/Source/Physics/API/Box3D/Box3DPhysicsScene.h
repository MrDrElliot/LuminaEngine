#pragma once

#include "World/ECS/Registry.h"


#include <box3d/box3d.h>

#include "Box3DTaskBridge.h"
#include "Containers/HashTable.h"
#include "World/ECS/EntityMap.h"
#include "Containers/Queue.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsScene.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Renderer/SkeletonResource.h"
#include "World/Entity/Components/DirtyComponent.h"

namespace Lumina
{
    struct FDynamicMeshRenderData;
    struct SCharacterMovementComponent;
    struct SCharacterPhysicsComponent;
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
        // A box collider's hull lives here until commit, since Box3D clones it on shape creation and the shared cache's lock serialized parallel builds.
        TUniquePtr<b3BoxHull>   BoxHull;

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
        ECS::FEntity        EntityA;
        ECS::FEntity        EntityB;
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

    // Characters that owe a full collide-and-solve this substep, gathered so the pass can fan out.
    struct FCharacterWork
    {
        SCharacterPhysicsComponent*     Physics;
        SCharacterMovementComponent*    Movement;
    };

    struct FPendingCharacterPush
    {
        b3BodyId    Body;
        b3Vec3      Impulse;
        b3Vec3      Point;
    };

    // One bucket per task-thread slot, cache isolated so neighboring workers do not share a line.
    struct alignas(64) FCharacterPushBucket
    {
        TVector<FPendingCharacterPush> Pushes;
    };

    class RUNTIME_API FBox3DPhysicsScene : public IPhysicsScene
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

        void ActivateBody(ECS::FEntity Entity) override;
        void DeactivateBody(ECS::FEntity Entity) override;
        void ChangeBodyMotionType(ECS::FEntity Entity, EBodyType NewType) override;
        bool IsBodyActive(ECS::FEntity Entity) const override;

        void ApplyDirtyTransforms(float FixedDt, uint32 RemainingSteps, bool bFirstStep);
        void UpdateCharacters(float FixedDt);
        void UpdateVehicles(float FixedDt);
        void PoseVehicleWheels();
        void StepCharacter(const FCharacterWork& Work, float FixedDt, uint32 ThreadSlot);
        void LatchCharacterInput();

        bool GetCharacterNetState(ECS::FEntity Entity, FCharacterNetState& Out) const override;
        bool SetCharacterNetState(ECS::FEntity Entity, const FCharacterNetState& State) override;
        bool SimulateCharacterStep(ECS::FEntity Entity, const FCharacterMoveInput& Input, float FixedDt, bool bReplay) override;
        void SetCharacterNetDrive(ECS::FEntity Entity, ECharacterNetDrive Drive) override;

        // Teleports and ground seating, shared by the loop and an isolated step. False means no collide-and-solve is owed.
        bool ResolveCharacterPreStep(ECS::FEntity Entity, SCharacterPhysicsComponent& Physics, SCharacterMovementComponent& Movement);
        void FollowTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, SCharacterPhysicsComponent& Physics,
            SCharacterMovementComponent& Movement, float FixedDt);
        void BuildInterpolatedTransforms(float Alpha);
        void ApplyInterpolatedTransforms();

        EPhysicsBodyStatus GetBodyStatus(ECS::FEntity Entity) const override;
        bool TryGetBodyState(ECS::FEntity Entity, FPhysicsBodyState& Out) const override;
        bool TryGetTargetState(const FPhysicsBodyTarget& Target, FPhysicsBodyState& Out) const override;
        void AddForceAtTarget(const FPhysicsBodyTarget& Target, const FVector3& Force, const FVector3& Point) override;
        FPhysicsBodyTarget MakeBodyTarget(uint32 Handle) const;

        TOptional<SRayResult> CastRay(const SRayCastSettings& Settings) override;
        void CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) override;
        TOptional<SRayResult> CastSphereClosest(const SSphereCastSettings& Settings) override;
        void CastRayAll(const SRayCastSettings& Settings, TVector<SRayResult>& OutHits) override;

        int32 ResolveHitBoneIndex(ECS::FEntity Entity, b3BodyId BodyId) const;
        int32 CollidePoint(const FVector3& Point, TSpan<const ECS::FEntity> IgnoreEntities, TSpan<ECS::FEntity> OutEntities) override;
        int32 OverlapSphere(const FVector3& Center, float Radius, TSpan<const ECS::FEntity> IgnoreEntities, TSpan<ECS::FEntity> OutEntities) override;
        int32 OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, TSpan<const ECS::FEntity> IgnoreEntities, TSpan<ECS::FEntity> OutEntities) override;

        void OnCharacterComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnCharacterComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnRigidBodyComponentUpdated(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnRigidBodyComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnRigidBodyComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnColliderComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnColliderComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void OnConstraintComponentConstructed(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void OnConstraintComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity);

        void AddForce(ECS::FEntity Entity, const FVector3& Force) override;
        void AddImpulse(ECS::FEntity Entity, const FVector3& Impulse) override;
        void AddTorque(ECS::FEntity Entity, const FVector3& Torque) override;
        void AddAngularImpulse(ECS::FEntity Entity, const FVector3& AngularImpulse) override;
        void AddForceAtPosition(ECS::FEntity Entity, const FVector3& Force, const FVector3& Position) override;
        void AddImpulseAtPosition(ECS::FEntity Entity, const FVector3& Impulse, const FVector3& Position) override;
        void SetLinearVelocity(ECS::FEntity Entity, const FVector3& Velocity) override;
        void SetAngularVelocity(ECS::FEntity Entity, const FVector3& AngularVelocity) override;
        void SetGravityFactor(ECS::FEntity Entity, float Factor) override;

        void ApplyBuoyancyImpulse(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime) override;

        FVector3 GetVelocityAtPoint(ECS::FEntity Entity, const FVector3& Point) const override;
        FVector3 GetLinearVelocity(ECS::FEntity Entity) const override;
        FVector3 GetAngularVelocity(ECS::FEntity Entity) const override;
        FVector3 GetCenterOfMass(ECS::FEntity Entity) const override;
        float GetBodyMass(ECS::FEntity Entity) const override;
        FVector3 GetBodyPosition(ECS::FEntity Entity) const override;
        FQuat GetBodyRotation(ECS::FEntity Entity) const override;

        uint32 GetBodyCount() override;

        // Joints alive in the solver, counted here because Box3D's world registry is private to the module that links it.
        uint32 GetLiveJointCount();
        uint32 GetMaxBodyCount() override;


        uint32 CreateStaticBodyGroup(ECS::FEntity Owner, TSpan<const FStaticInstanceDesc> Instances) override;
        void DestroyStaticBodyGroup(uint32 GroupID) override;

        TSharedPtr<FPhysicsRagdollHandle> CreateRagdoll(const FRagdollDesc& Desc) override;
        bool IsRagdollReady(const FPhysicsRagdollHandle& Handle) const override;
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

        void SetSurfaceVelocity(ECS::FEntity Entity, const FVector3& Linear, const FVector3& Angular) override;

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

        uint32 FindEntityBody(ECS::FEntity Entity) const;
        b3BodyId ResolveTarget(const FPhysicsBodyTarget& Target) const;
        bool ReadBodyState(b3BodyId Body, FPhysicsBodyState& Out) const;
        void SynchronizeBodies();
        void SynchronizeBodyGroups();
        void CommitStaticBodyGroup(uint32 GroupID, ECS::FEntity Owner, TSpan<const FRigidBodyBuildResult> Builds);

        struct FPendingStaticGroup
        {
            uint32 GroupID = 0;
            ECS::FEntity Owner = ECS::NullEntity;
            TVector<FRigidBodyBuildResult> Builds;
        };
        TVector<FPendingStaticGroup> PendingStaticGroups;

        struct FPendingRagdoll
        {
            FRagdollDesc Description;
            TSharedPtr<FSkeletonResource> Skeleton;
            TVector<FMatrix4> Globals;
            TVector<SPhysicsBodySetup> Bodies;
            TVector<SPhysicsConstraintSetup> Constraints;
            TSharedPtr<FPhysicsRagdollHandle> Handle;
        };
        TSharedPtr<FPhysicsRagdollHandle> CommitRagdoll(const FPendingRagdoll& Request);
        TVector<FPendingRagdoll> PendingRagdolls;
        struct FOwnedRagdoll
        {
            ECS::FEntity Owner;
            TSharedPtr<FPhysicsRagdollHandle> Handle;
        };
        TVector<FOwnedRagdoll> OwnedRagdolls;
        void DestroyBodyHandle(uint32 Handle);
        void CreateCharacter(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void ApplyBuoyancy(ECS::FEntity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime);
        void ApplySurfaceVelocity(b3BodyId Body, const FVector3& Linear, const FVector3& Angular);
        void DispatchContactEvents();
        void DispatchActivationEvents();
        void DrainStepEvents();

        // Interpolation is driven by Box3D's move events, so a sleeping or static body costs nothing here.
        void DrainMoveEvents(bool bStageForInterp);
        void MarkAuthoredKinematic(uint32 Handle);
        uint32 StageInterpSlot(uint32 BodyHandle, const FVector3& Position, const FQuat& Rotation);
        void ResetInterpStaging();
        void DropCharacterInterpSlots();

        void BulkCreateRigidBodies(ECS::FRegistry& Registry);
        void CreateRigidBodiesBatched(const TVector<ECS::FEntity>& Entities);
        void RebuildStaleDynamicMeshBodies(ECS::FRegistry& Registry);

        EBodyBuildStatus TryBuildRigidBody(ECS::FRegistry& Registry, ECS::FEntity Entity, FRigidBodyBuildResult& OutResult);
        uint32 CommitRigidBody(ECS::FEntity Entity, FRigidBodyBuildResult& Build);

        bool TryCreateComponentConstraint(ECS::FRegistry& Registry, ECS::FEntity Entity);
        void DrainPendingConstraints();
        void SynchronizeConstraints();
        bool CommitConstraint(uint32 Handle, const FConstraintDesc& Desc);
        void ApplyConstraintEnabled(uint32 ConstraintID, bool bEnabled);
        void ApplyConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target);
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
            static size_t operator()(const FHullKey& Key)
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
        // Built analytically and owned here rather than by Box3D, so they are freed with Memory::Delete.
        THashMap<FHullKey, b3BoxHull*, FHullKeyHash>        BoxHullCache;

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

        // A dynamic mesh collider's shape, built on a worker so a streamed-in chunk never welds and partitions on the game thread.
        struct FDynamicMeshCook
        {
            TAtomic<bool>   bDone{ false };
            bool            bFailed = false;
            b3MeshData*     Mesh = nullptr;
            b3HullData*     Hull = nullptr;

            ~FDynamicMeshCook();
        };

        struct FDynamicMeshCookEntry
        {
            TSharedPtr<FDynamicMeshCook> Cook;
            // Held, not just compared, so a freed snapshot's address cannot be mistaken for the current one.
            TSharedPtr<FDynamicMeshRenderData> RenderData;
            FVector3                     Scale = FVector3(1.0f);
            bool                         bConvex = false;
        };

        // Written only by the serial pass before the parallel build, so the build reads it without a lock.
        THashMap<ECS::FEntity, FDynamicMeshCookEntry> DynamicMeshCooks;

        struct FPendingDynamicMeshCook
        {
            TSharedPtr<FDynamicMeshCook> Cook;
            TVector<b3Vec3>              Positions;
            TVector<int32>               Indices;
            bool                         bConvex = false;
        };
        static void RunDynamicMeshCook(FPendingDynamicMeshCook& Pending);

        void StartDynamicMeshCooks(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Entities);
        // Set while the level's bodies are created in bulk, so nothing spawns over ground that has no collision yet.
        bool bCookDynamicMeshesInline = false;
        bool IsDynamicMeshCookPending(ECS::FEntity Entity) const;

        struct FDeferredBodyUpdate
        {
            ECS::FEntity            Entity;
            FNeedsPhysicsBodyUpdate Update;
        };
        TVector<FDeferredBodyUpdate>            RetryBodyUpdates;

        struct FBodyRecord
        {
            uint32 Handle = ~0u;
            uint64 Revision = 0;
            EPhysicsBodyStatus Status = EPhysicsBodyStatus::Pending;
            FVector3 LastBodyPosition = FVector3(0.0f);
            FQuat LastBodyRotation = FQuat::Identity();
            bool bRebuild = false;
        };

        ECS::TEntityMap<FBodyRecord> RigidBodies;
        ECS::TEntityMap<FBodyRecord> CharacterBodies;

        const FBodyRecord* FindBodyRecord(ECS::FEntity Entity) const;
        uint64 NextBindingRevision = 1;
        TVector<ECS::FEntity> PendingRigidBodies;
        TVector<ECS::FEntity> PendingCharacters;
        TVector<uint32> PendingBodyDestructions;
        TVector<uint32> BodyGenerations;
        uint64 SceneIdentity = 0;
        bool bStaticTreeDirty = false;

        enum class EBodyCommand : uint8
        {
            Impulse, Force, Torque, AngularImpulse, LinearVelocity, AngularVelocity,
            ImpulseAtPosition, ForceAtPosition, Gravity, Activate, Deactivate, MotionType,
            SurfaceVelocity, Buoyancy, TargetForce
        };

        struct FBodyCommand
        {
            EBodyCommand Type;
            ECS::FEntity Entity = ECS::NullEntity;
            FPhysicsBodyTarget Target;
            FVector3 Value = FVector3(0.0f);
            FVector3 Point = FVector3(0.0f);
            FVector3 Secondary = FVector3(0.0f);
            FVector4 Parameters = FVector4(0.0f);

            // The body binding the command was aimed at, so a removed and re-added component does not inherit it. Zero when none existed yet.
            uint64 Revision = 0;
        };

        enum class EBodyCommandResult : uint8 { Applied, Dropped, Waiting };

        void QueueBodyCommand(const FBodyCommand& Command);
        void ApplyBodyCommands();
        EBodyCommandResult ApplyBodyCommand(const FBodyCommand& Command);

        // One queue per job thread slot, so callers on any worker push without a lock.
        TVector<TVector<FBodyCommand>> ThreadBodyCommands;
        FMutex BodyCommandMutex;
        TVector<FBodyCommand> OverflowBodyCommands;
        TVector<FBodyCommand> WaitingBodyCommands;
        TVector<FBodyCommand> BodyCommandScratch;
        TVector<FBodyCommand> DrainBodyCommands;

        // Offsets the re-validation phase of resting characters so a settled crowd does not poll on one step.
        uint64                                  CharacterStepCounter = 0;

        TVector<FCharacterWork>                 CharacterWorkScratch;
        TVector<FCharacterPushBucket>           CharacterPushScratch;


        TVector<FRigidBodyBuildResult>          BatchBuildScratch;
        TVector<EBodyBuildStatus>               BatchStatusScratch;
        TVector<ECS::FEntity>                   PendingDrainScratch;

        FBox3DTaskBridge                        TaskBridge;
        b3WorldId                               WorldId{};
        CWorld*                                 World = nullptr;

        // Dense handle table so the engine's uint32 body id survives Box3D's generational ids.
        TVector<b3BodyId>                       BodyHandles;
        TVector<uint32>                         FreeBodyHandles;

        TVector<FContactRecord>                 ContactDrainScratch;

        struct FActivationRecord
        {
            ECS::FEntity    Entity;
            bool            bActivated;
        };
        TVector<FActivationRecord>              ActivationDrainScratch;

        TVector<FBodyMaterialEntry>             BodyMaterials;

        float                                   Accumulator = 0.0f;
        uint32                                  CollisionSteps = 0;
        uint32                                  MaxBodies = 0;

        static constexpr uint32                 kInitialBodyReservation = 4096;
        static constexpr uint32                 kInitialContactReservation = 8192;

        // Hash of the settings last pushed, so an unedited frame skips the setter calls entirely.
        uint64                                  WorldSettingsHash = 0;

        uint32                                  NextRagdollGroupID = 1;

        THashMap<uint32, TVector<uint32>>       StaticBodyGroups;
        uint32                                  NextStaticBodyGroupID = 1;

        struct FBox3DConstraint
        {
            b3JointId               JointId{};
            FConstraintDesc         Description;
            uint64                  RevisionA = 0;
            uint64                  RevisionB = 0;
            bool                    bDestroy = false;
            bool                    bSettingsDirty = false;
            bool                    bMotorSet = false;
            EConstraintMotorMode    MotorMode = EConstraintMotorMode::Off;
            float                   MotorTarget = 0.0f;
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
        TVector<ECS::FEntity>                   PendingConstraintCreations;

        enum class EInterpFlag : uint8 { Interpolate = 0, Skip = 1, BelowKill = 2 };

        // Grow-only SoA holding only the bodies that actually moved this frame. Rotations are deinterleaved
        // to x/y/z/w so the nlerp vectorizes.
        struct FInterpStaging
        {
            TVector<ECS::FEntity>   Entities;
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

            void Resize(size_t Count)
            {
                Truncate(Count);
            }

            void EnsureLerpCapacity()
            {
                const size_t N = Entities.size();
                LerpPos.resize(N);
                LerpQx.resize(N); LerpQy.resize(N); LerpQz.resize(N); LerpQw.resize(N);
            }

            void Truncate(size_t Count)
            {
                Entities.resize(Count); Flags.resize(Count);
                PrevPos.resize(Count);  CurrPos.resize(Count);
                PrevQx.resize(Count); PrevQy.resize(Count); PrevQz.resize(Count); PrevQw.resize(Count);
                CurrQx.resize(Count); CurrQy.resize(Count); CurrQz.resize(Count); CurrQw.resize(Count);
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

        // Characters are restaged on every blend, so their slots trail the bodies and are dropped before the next one.
        uint32                                  InterpBodySlots = 0;
        bool                                    bInterpCharacterTail = false;
        TVector<uint32>                         InterpApplied;
        TVector<uint32>                         InterpAppliedParented;
        TVector<uint8>                          InterpCategory;

        // Parented poses written in parallel, queued for the resolve in one bulk call per frame.
        struct CACHE_ALIGN FInterpDeferBucket
        {
            TVector<ECS::FEntity> Entities;
        };
        TVector<FInterpDeferBucket>             InterpDeferred;

        // Body handle to staging slot for this frame, reset through StagedBodyHandles so it stays O(moved).
        TVector<uint32>                         BodyStagingSlot;
        TVector<uint32>                         StagedBodyHandles;

        // Dense wake state per body handle, diffed against the move events to raise OnWake / OnSleep.
        TVector<uint8>                          BodyAwake;

        // Kinematic bodies gameplay placed this update, which draw at that placement rather than the stepped pose.
        TVector<uint8>                          BodyAuthoredKinematic;
        TVector<uint32>                         AuthoredKinematicHandles;
        TVector<uint32>                         PreviousAuthoredKinematicHandles;
    };
}
