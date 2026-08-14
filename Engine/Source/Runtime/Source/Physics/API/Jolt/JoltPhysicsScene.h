#pragma once

// Jolt/Jolt.h must come first -- it sets up the JPH_* macros every other
// Jolt header expects, and was previously force-included via pch.h.
#include <Jolt/Jolt.h>
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/Shape.h"
#include "Jolt/Physics/Constraints/TwoBodyConstraint.h"
#include "Containers/Array.h"
#include <cstring>
#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsScene.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Events/ImpulseEvent.h"
#include "Renderer/SkeletonResource.h"


namespace JPH
{
	class CharacterVsCharacterCollisionSimple;
}

namespace Lumina
{
	struct SImpulseEvent;
	struct SCompoundColliderComponent;
	class CCollisionShape;
	struct STransformComponent;
	class CWorld;
	class CMesh;
}

namespace Lumina::Physics
{
	class FJoltPhysicsScene;
	class FJoltCharacterContactListener;   // CharacterVirtual contact listener, defined in the .cpp.
	class FJoltBodyActivationListener;     // Sleep/wake listener, defined in the .cpp.

	enum class EContactEventType : uint8
	{
		Added,
		Removed,
	};

	// Outcome of TryBuildRigidBodyCreationSettings: drives whether the caller
	// commits the body, retries later, or drops the entity.
	enum class EBodyBuildStatus : uint8
	{
		Success,        // Result populated; safe to CreateBody and add.
		Defer,          // Asset/transform not ready; push to PendingRigidBodyCreations.
		AlreadyExists,  // Component already owns a BodyID; skip.
		NoCollider,     // No collider component attached; nothing to build (caller decides whether to defer).
		Error,          // Shape build failed and was logged; do not retry.
	};

	struct FRigidBodyBuildResult
	{
		JPH::BodyCreationSettings	Settings;
		FVector3					LastBodyPosition = FVector3(0.0f);
		FQuat						LastBodyRotation = FQuat::Identity();
		// Resolved on the parallel build so the serial commit never re-derives it from the shape.
		float						ComputedMass = 0.0f;
		// Resolved from the collider's PhysicsMaterial (if any), copied into BodyMaterials on commit.
		// Cached on the build result so the parallel build never touches the registry post-commit.
		bool						bHasMaterial = false;
		float						MaterialFriction = 0.0f;
		float						MaterialRestitution = 0.0f;
		uint8						MaterialFrictionCombine = 0;
		uint8						MaterialRestitutionCombine = 0;
		// Conveyor surface velocity from an SConveyorComponent (world space; zero = none).
		FVector3					SurfaceLinearVelocity = FVector3(0.0f);
		FVector3					SurfaceAngularVelocity = FVector3(0.0f);
	};

	// Contact snapshot recorded by Jolt's own worker jobs during the step, drained right after it on the
	// game thread; pre-resolves entity ids and velocities so dispatch never touches a (possibly
	// destroyed) Jolt body.
	struct FContactRecord
	{
		EContactEventType	Type;
		entt::entity		EntityA;
		entt::entity		EntityB;
		uint32				BodyIDA;
		uint32				BodyIDB;
		FVector3			Point;          // average manifold point (world space)
		FVector3			Normal;         // contact normal in world space (points from A toward B)
		FVector3			VelocityA;      // pre-step linear velocity of A
		FVector3			VelocityB;      // pre-step linear velocity of B
		float				ImpactSpeed;    // |relative velocity along normal|
		bool				bSensorA;
		bool				bSensorB;
	};

	class FJoltContactListener : public JPH::ContactListener
	{
	public:
		FJoltContactListener(FJoltPhysicsScene* InScene, entt::dispatcher& InDispatcher, const JPH::BodyLockInterfaceNoLock* InBodyLockInterface)
			: Scene(InScene)
			, EventDispatcher(InDispatcher)
			, BodyLockInterface(InBodyLockInterface)
		{ }

		// Jolt: reject a contact pair before it's added; called with all bodies locked, so no locking.
		virtual JPH::ValidateResult	OnContactValidate(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::RVec3Arg inBaseOffset, const JPH::CollideShapeResult& inCollisionResult) override { return JPH::ValidateResult::AcceptAllContactsForThisBodyPair; }

		// Jolt: new contact point; called with all bodies locked, so no locking. Velocities are pre-solve.
		virtual void OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

		// Jolt: contact also present last update; called with all bodies locked, so no locking.
		virtual void OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings) override;

		// Jolt: contact gone since last update; uses BodyIDs (bodies may be removed). Called locked, so no locking.
		virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override;
    	
	private:
		void OverrideFrictionAndRestitution(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings);
		void GetFrictionAndRestitution(const JPH::Body& inBody, const JPH::SubShapeID& inSubShapeID, float& outFriction, float& outRestitution) const;
		// Conveyor belts / moving surfaces: feed the per-body surface velocities into the contact constraint.
		void ApplySurfaceVelocity(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::ContactSettings& ioSettings);

	private:

		FJoltPhysicsScene* Scene = nullptr;
		entt::dispatcher& EventDispatcher;
		const JPH::BodyLockInterfaceNoLock* BodyLockInterface = nullptr;
	};
    
    class FJoltPhysicsScene : public IPhysicsScene
    {
    public:

        FJoltPhysicsScene(CWorld* InWorld);
        ~FJoltPhysicsScene() override;
    	
    	LE_NO_COPYMOVE(FJoltPhysicsScene);


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

    	// Push game-thread transform edits into Jolt bodies once per frame,
    	// before the fixed-step loop.
    	void ApplyDirtyTransforms(float FixedDt);

    	// Step every CharacterVirtual on the fixed timestep, alongside the
    	// Jolt rigid-body update.
    	void UpdateCharacters(float FixedDt);

    	// Latch controller input into SCharacterMovementComponent once per Update() so all substeps see it.
    	void LatchCharacterInput();
    	
    	// Runs inside the step (may be on a worker): stages interpolated transforms in
    	// InterpolatedTransforms; does NOT touch the registry.
    	void BuildInterpolatedTransforms(float Alpha);

    	// Game thread: write staged transforms into the ECS, resolve hierarchy, process kill-height destroys.
    	// Sole writer of physics-driven transforms, so it must run on the game thread.
    	void ApplyInterpolatedTransforms();
    	
    	uint32 GetEntityBodyID(entt::entity Entity) override;
    	
    	TOptional<SRayResult> CastRay(const SRayCastSettings& Settings) override;
		TVector<SRayResult> CastSphere(const SSphereCastSettings& Settings) override;
		TVector<SRayResult> CastRayAll(const SRayCastSettings& Settings) override;

		// Skeleton bone index behind a hit body: ragdoll per-bone bodies map through the entity's
		// ragdoll handle; every other body returns INDEX_NONE.
		int32 ResolveHitBoneIndex(entt::entity Entity, JPH::BodyID BodyID) const;
		void CollidePoint(const FVector3& Point, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities) override;
		void OverlapSphere(const FVector3& Center, float Radius, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities) override;
		void OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities) override;

		// Shared CollideShape overlap: gathers distinct entities into OutEntities. Shape is owned by the caller.
		void OverlapShapeInternal(const JPH::Shape* Shape, const JPH::RMat44& Transform, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities);
    	
    	// Lazily stands up the per-worker character-substep allocator pool on the first character;
    	// character-free worlds never allocate it.
    	void EnsureCharacterAllocators();
    	void OnCharacterComponentConstructed(entt::registry& Registry, entt::entity Entity);
    	void OnCharacterComponentDestroyed(entt::registry& Registry, entt::entity Entity);
    	
    	void OnRigidBodyComponentUpdated(entt::registry& Registry, entt::entity Entity);
    	void OnRigidBodyComponentConstructed(entt::registry& Registry, entt::entity Entity);
    	void OnRigidBodyComponentDestroyed(entt::registry& Registry, entt::entity Entity);
    	void OnColliderComponentAdded(entt::registry& Registry, entt::entity Entity);
    	void OnColliderComponentRemoved(entt::registry& Registry, entt::entity Entity);

    	// SPhysicsConstraintComponent lifecycle: construct queues creation (deferred until both bodies exist),
    	// destroy tears down the live joint.
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

    	TSharedPtr<FJoltRagdollHandle> CreateRagdoll(const FRagdollDesc& Desc) override;
    	void ReadRagdollPose(const FJoltRagdollHandle& Handle, const FMatrix4& WorldToEntity, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms) override;
    	void DestroyRagdoll(const TSharedPtr<FJoltRagdollHandle>& Handle) override;
    	void GetRagdollRootTransform(const FJoltRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation) override;
    	uint32 AllocateRagdollGroupID() override { return NextRagdollGroupID++; }

    	uint32 CreateConstraint(const FConstraintDesc& Desc) override;
    	void DestroyConstraint(uint32 ConstraintID) override;
    	void SetConstraintEnabled(uint32 ConstraintID, bool bEnabled) override;
    	void SetConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target) override;
    	bool IsConstraintBroken(uint32 ConstraintID) override;
    	float GetConstraintValue(uint32 ConstraintID) override;

    	JPH::PhysicsSystem* GetPhysicsSystem() const { return JoltSystem.get(); }
    	
    	void EnqueueContactRecord(const FContactRecord& Record);

    	// Called by the body-activation listener, which Jolt raises from its worker jobs, to stage a
    	// sleep/wake transition for the game-thread script dispatch. bActivated == true is a wake.
    	void EnqueueActivation(entt::entity Entity, bool bActivated);

    	// Shared collision shape, built on first request so N identical bodies share one JPH::Shape.
    	// Thread-safe: called from the parallel body build.
    	JPH::ShapeRefC GetOrCreateSphereShape(float Radius);
    	JPH::ShapeRefC GetOrCreateBoxShape(const FVector3& HalfExtent);
    	JPH::ShapeRefC GetOrCreateCapsuleShape(float Radius, float HalfHeight);
    	JPH::ShapeRefC GetOrCreateCylinderShape(float Radius, float HalfHeight, float CapRadius);
    	JPH::ShapeRefC GetOrCreateTaperedCapsuleShape(float HalfHeight, float TopRadius, float BottomRadius);
    	JPH::ShapeRefC GetOrCreateTaperedCylinderShape(float HalfHeight, float TopRadius, float BottomRadius, float ConvexRadius);

    	// Build a StaticCompoundShape from an SCompoundColliderComponent's child primitives (each child reuses
    	// the cached primitive shapes). Thread-safe (called from the parallel body build). Null on failure.
    	JPH::ShapeRefC BuildCompoundShape(const SCompoundColliderComponent& Comp, const STransformComponent& Transform);

    	// Compounds an authored collision asset's primitives, or builds its baked triangle mesh. Hull pieces
    	// are uncached: a hull point set belongs to one asset, so there is nothing for a shared cache to hit.
    	JPH::ShapeRefC BuildCollisionShapeAsset(const CCollisionShape& Asset, const FVector3& Scale);

    	// Mesh-collider shape cached by mesh+scale+convexity so shards reuse one hull instead of re-running
    	// QuickHull; builds outside the lock so parallel distinct-mesh builds don't serialize.
    	JPH::ShapeRefC GetOrCreateMeshShape(const CMesh* Mesh, const FVector3& Scale, bool bConvex);

    	// Per-body material data consumed by the contact listener's OverrideFrictionAndRestitution.
    	struct FBodyMaterialEntry
    	{
    		float	Friction            = 0.0f;
    		float	Restitution         = 0.0f;
    		uint8	FrictionCombine     = 0;    // EPhysicsMaterialCombineMode
    		uint8	RestitutionCombine  = 0;
    		bool	bHasMaterial        = false;
    	};

    	// Side-table accessors for the contact listener. GetBodyMaterial is hot-path (called per
    	// contact pair during the physics step) and never resizes; Store/Clear run game-thread only.
    	void StoreBodyMaterial(JPH::BodyID BodyID, const FRigidBodyBuildResult& Build);
    	void ClearBodyMaterial(JPH::BodyID BodyID);
    	const FBodyMaterialEntry* GetBodyMaterial(JPH::BodyID BodyID) const;

    	// Per-body surface velocity (conveyor belts / moving floors). World-space; the contact listener feeds
    	// the body2-minus-body1 difference into the constraint each step. Same threading model as BodyMaterials
    	// (lock-free read on the step, game-thread writes between steps). bActive gates the per-contact work.
    	struct FBodySurfaceVelocity
    	{
    		FVector3	Linear  = FVector3(0.0f);
    		FVector3	Angular = FVector3(0.0f);
    		bool		bActive = false;
    	};
    	void StoreBodySurfaceVelocity(JPH::BodyID BodyID, const FVector3& Linear, const FVector3& Angular);
    	void ClearBodySurfaceVelocity(JPH::BodyID BodyID);
    	const FBodySurfaceVelocity* GetBodySurfaceVelocity(JPH::BodyID BodyID) const;

    	// Set a body's conveyor surface velocity at runtime (world space); zero clears it. Game thread only.
    	void SetSurfaceVelocity(entt::entity Entity, const FVector3& Linear, const FVector3& Angular) override;

    private:
    	
    	void DispatchContactEvents();
    	void DispatchActivationEvents();
    	
    	void SnapshotBodyStates();
    	
    	void BulkCreateRigidBodies(entt::registry& Registry);

    	// Build the given entities' bodies in parallel, then insert them all with a single
    	// AddBodiesPrepare/Finalize. Game-thread only (outside JoltSystem->Update()).
    	void CreateRigidBodiesBatched(const TVector<entt::entity>& Entities);

    	// Build + add a single body; outside Update(). on_construct enqueues, the drain calls this.
    	void CreateRigidBodyImmediate(entt::registry& Registry, entt::entity Entity);


    private:

    	// Shared-shape cache keyed by kind + dimensions (bit-exact float compare).
    	struct FShapeKey
    	{
    		uint8	Kind;       // 0 sphere, 1 box, 2 capsule, 3 cylinder, 4 tapered capsule, 5 tapered cylinder
    		float	X, Y, Z;
    		float	W = 0.0f;   // 4th param (e.g. tapered-cylinder convex radius); 0 for the others.
    		bool operator==(const FShapeKey& Other) const
    		{
    			return Kind == Other.Kind && X == Other.X && Y == Other.Y && Z == Other.Z && W == Other.W;
    		}
    	};
    	struct FShapeKeyHash
    	{
    		size_t operator()(const FShapeKey& Key) const
    		{
    			size_t Hash = Key.Kind;
    			auto Mix = [&Hash](float Value)
    			{
    				uint32 Bits;
    				std::memcpy(&Bits, &Value, sizeof(Bits));
    				Hash = (Hash * 1099511628211ull) ^ Bits;
    			};
    			Mix(Key.X); Mix(Key.Y); Mix(Key.Z); Mix(Key.W);
    			return Hash;
    		}
    	};
    	mutable FSharedMutex								ShapeCacheMutex;
    	THashMap<FShapeKey, JPH::ShapeRefC, FShapeKeyHash>	ShapeCache;

    	// Shared-shape cache halves. Lookup takes a read lock only, so a parallel body build of N identical
    	// primitives no longer serializes on one exclusive lock; the shape is built between the two calls,
    	// outside any lock. Publish returns the winner of a same-key race, not necessarily InShape.
    	JPH::ShapeRefC FindCachedShape(const FShapeKey& Key) const;
    	JPH::ShapeRefC PublishCachedShape(const FShapeKey& Key, const JPH::ShapeRefC& InShape);


    	struct FMeshShapeKey
    	{
    		const void*	Mesh;
    		uint8		Convex;
    		bool operator==(const FMeshShapeKey& Other) const
    		{
    			return Mesh == Other.Mesh && Convex == Other.Convex;
    		}
    	};
    	struct FMeshShapeKeyHash
    	{
    		size_t operator()(const FMeshShapeKey& Key) const
    		{
    			size_t Hash = 1469598103934665603ull;
    			auto Mix = [&Hash](uint64 Value) { Hash = (Hash ^ Value) * 1099511628211ull; };
    			Mix(reinterpret_cast<uint64>(Key.Mesh));
    			Mix(Key.Convex);
    			return Hash;
    		}
    	};
    	mutable FSharedMutex										MeshShapeCacheMutex;
    	THashMap<FMeshShapeKey, JPH::ShapeRefC, FMeshShapeKeyHash>	MeshShapeCache;

    	// Body-move requests ApplyDirtyTransforms could not apply because the body did not exist yet; re-tagged
    	// after the consume so the next step retries them. Game/physics-step thread only, reused across frames.
    	struct FDeferredBodyUpdate
    	{
    		entt::entity				Entity;
    		FNeedsPhysicsBodyUpdate		Update;
    	};
    	TVector<FDeferredBodyUpdate>				RetryBodyUpdates;

    	FMutex										PendingRigidBodyMutex;
    	TQueue<entt::entity>						PendingRigidBodyCreations;

    	// True while Update() is stepping this scene. Jolt forbids body creation during a step, so
    	// on_construct defers instead. Per-scene (not thread_local) so it stays correct when the
    	// physics step runs as a migrating fiber job rather than a dedicated thread.
    	TAtomic<bool>								bStepInProgress{false};

    	// Non-zero between BeginBodyBatch/EndBodyBatch on the game thread: rigid-body constructions are
    	// collected and created together instead of one at a time. A depth, not a flag, because the API is
    	// script-facing now and a scoped batch can nest inside another (e.g. fracture during a spawn batch).
    	int32										BodyBatchDepth = 0;
    	TVector<entt::entity>						BatchedBodyCreations;

    	// Characters defer through the same batch. Not for the bulk-insert win (there is no batched
    	// CharacterVirtual create) but for the POSE: the controller is built from the entity's transform and
    	// owns it from then on, so anything that emplaces the component before writing the final transform --
    	// a prefab spawn -- would otherwise bake the pre-spawn pose in and stamp it back over the entity.
    	TVector<entt::entity>						BatchedCharacterCreations;

    	// Reused by CreateRigidBodiesBatched so a repeated bulk spawn doesn't reallocate ~350 bytes/body
    	// every call. Grow-only and game-thread only; entries past the current count are stale.
    	TVector<FRigidBodyBuildResult>				BatchBuildScratch;
    	TVector<EBodyBuildStatus>					BatchStatusScratch;
    	TVector<JPH::BodyID>						BatchBodyIDScratch;
    	TVector<entt::entity>						PendingDrainScratch;

    	// Base buffer covers typical steps with no per-frame alloc; heavy frames fall back to malloc.
    	JPH::TempAllocatorImplWithMallocFallback	Allocator;
    	TVector<TUniquePtr<JPH::TempAllocatorImpl>>	CharacterAllocators;
    	TUniquePtr<FJoltContactListener>			ContactListener;
    	// Shared CharacterVirtual contact listener; every character points at it (see OnCharacterComponentConstructed).
    	TUniquePtr<FJoltCharacterContactListener>	CharacterContactListener;
    	// Sleep/wake listener; records transitions for the game-thread script dispatch.
    	TUniquePtr<FJoltBodyActivationListener>		ActivationListener;
    	
    	TUniquePtr<JPH::CharacterVsCharacterCollisionSimple>	CharacterVsCharacter;
    	THashSet<uint32>							CharacterProxyBodies;

        TUniquePtr<JPH::PhysicsSystem>				JoltSystem;
        CWorld*										World = nullptr;
    	
    	TConcurrentQueue<FContactRecord>			PendingContacts;
    	TVector<FContactRecord>						ContactDrainScratch;   // game thread, reused across frames

    	// Sleep/wake transitions staged by the activation listener (Jolt worker job), drained game-thread.
    	struct FActivationRecord
    	{
    		entt::entity	Entity;
    		bool			bActivated;     // true = woke / spawned active, false = went to sleep
    	};
    	TConcurrentQueue<FActivationRecord>			PendingActivations;
    	TVector<FActivationRecord>					ActivationDrainScratch;

    	// Material side table indexed by BodyID, sized once to MaxPhysicsBodies so the listener reads lock-free;
    	// writes are game-thread-only between steps.
    	TVector<FBodyMaterialEntry>					BodyMaterials;

    	// Conveyor surface-velocity side table; same sizing/threading as BodyMaterials.
    	TVector<FBodySurfaceVelocity>				BodySurfaceVelocities;

    	float										Accumulator = 0.0f;
    	uint32										CollisionSteps = 0;

    	// Monotonic source for per-ragdoll self-collision group ids.
    	uint32										NextRagdollGroupID = 1;

    	// Active joints, keyed by the opaque handle CreateConstraint hands back. Guarded because the
    	// breakable monitor reads/disables them on the physics step while Create/Destroy mutate the map on
    	// the game thread (the two windows don't overlap, but the lock keeps it provably safe).
    	struct FJoltConstraint
    	{
    		JPH::Ref<JPH::TwoBodyConstraint>	Constraint;
    		EPhysicsConstraintType				Type        = EPhysicsConstraintType::Point;
    		float								BreakForce  = 0.0f;     // N; 0 = unbreakable.
    		bool								bBroken     = false;
    	};
    	FMutex										ConstraintsMutex;
    	THashMap<uint32, FJoltConstraint>			Constraints;
    	uint32										NextConstraintID = 1;

    	// Physics-thread: after each step, disable any breakable joint whose applied force exceeded BreakForce.
    	void MonitorBreakableConstraints(float Dt);
    	// Remove + release every joint; called on teardown before JoltSystem is destroyed.
    	void DestroyAllConstraints();

    	// Editor/component-authored joints: try to build one entity's joint (true once handled, false to retry
    	// next frame while its bodies aren't ready yet). DrainPendingConstraints runs the queue pre-step.
    	bool TryCreateComponentConstraint(entt::registry& Registry, entt::entity Entity);
    	void DrainPendingConstraints();
    	FMutex										PendingConstraintMutex;
    	TVector<entt::entity>						PendingConstraintCreations;

    	// Interp transforms staged during the step, read game-thread after it returns.
    	// SoA, grow-only: positions are flat float3, rotations deinterleaved to x/y/z/w so nlerp vectorizes.
    	enum class EInterpFlag : uint8 { Interpolate = 0, Skip = 1, BelowKill = 2 };
    	struct FInterpStaging
    	{
    		TVector<entt::entity>	Entities;
    		TVector<EInterpFlag>	Flags;

    		// Curr* is the simulated pose at the last completed step and stays untouched by the interp pass:
    		// it is the authoritative value written back into the ECS. Lerp* is the display-time blend, which
    		// is render-only and must never feed back into the body.
    		TVector<FVector3>		PrevPos;
    		TVector<FVector3>		CurrPos;
    		TVector<FVector3>		LerpPos;

    		TVector<float>			PrevQx, PrevQy, PrevQz, PrevQw;
    		TVector<float>			CurrQx, CurrQy, CurrQz, CurrQw;
    		TVector<float>			LerpQx, LerpQy, LerpQz, LerpQw;

    		void Resize(uint32 N)
    		{
    			Entities.resize(N); Flags.resize(N);
    			PrevPos.resize(N);  CurrPos.resize(N);  LerpPos.resize(N);
    			PrevQx.resize(N); PrevQy.resize(N); PrevQz.resize(N); PrevQw.resize(N);
    			CurrQx.resize(N); CurrQy.resize(N); CurrQz.resize(N); CurrQw.resize(N);
    			LerpQx.resize(N); LerpQy.resize(N); LerpQz.resize(N); LerpQw.resize(N);
    		}
    	};
    	FInterpStaging								InterpStaging;

    	// Staging slots the writeback actually applied this frame; the render-matrix pass revisits them
    	// after the transform resolve. Game thread only, reused across frames.
    	TVector<uint32>								InterpApplied;
    };
}
