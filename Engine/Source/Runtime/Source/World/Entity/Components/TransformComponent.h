#pragma once

#include "World/ECS/Registry.h"


#include <atomic>
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "DirtyComponent.h"
#include "Core/Math/Transform.h"
#include "World/Entity/EntityUtils.h"
#include "TransformComponent.generated.h"

namespace Lumina
{
    struct FPropertyChangedEvent;

    /**
     * OWNERSHIP AND SYNCHRONIZATION CONTRACT
     *
     * The scheduler, not this component, is what orders transform access. Systems declare Read/Write and
     * are batched so that conflicting ones never run together, batches run in sequence, and the frame's
     * stages (FrameStart -> PrePhysics -> DuringPhysics -> physics step -> PostPhysics -> FrameEnd ->
     * Extract) are strictly ordered on the game thread. Nothing here re-implements that.
     *
     *   LocalTransform  -- authored state. Owned by whichever system declared Write this batch. Safe to
     *                      write from ANY thread, including a bare user job, as long as writers touch
     *                      disjoint entities: every setter mutates only this component and a lock-free
     *                      channel. What it does NOT promise is immediate visibility, see below.
     *
     *   WorldTransform  -- derived state, owned by the resolve. Guaranteed correct at the start of every
     *                      batch, because CWorld::TickSystems resolves before each one. Never write it
     *                      from outside the resolve or the flat fast path in MarkDirty.
     *
     *   The visibility rule -- a write becomes visible to other systems at the NEXT BARRIER, not at the
     *                      instant it happens. That is the whole contract, and it is why writing from an
     *                      arbitrary thread is safe without a lock.
     *
     * Deferral exists for exactly one reason: HIERARCHY. Locals are written in arbitrary order, so a
     * parent may be written after its child and world transforms cannot be composed incrementally. A flat
     * entity has no such dependency -- world IS local -- so its setter resolves itself inline and never
     * touches the queue at all (see bIsFlat). The queue is not a race fix; it is a dependency-ordering
     * mechanism that only hierarchical entities need.
     *
     * Display poses live elsewhere and are never authored here: FRenderTransform holds the interpolated
     * matrix for physics-driven entities, and the render scene keeps its own snapshot taken at Extract.
     */
    // ScriptFastCalls: local accessors are bound SuppressGCTransition. World getters opt out (they resolve).
    REFLECT(Component, HideInComponentList, ScriptFastCalls)
    struct RUNTIME_API CACHE_ALIGN STransformComponent
    {
        GENERATED_BODY()
    
        friend class CWorld;
    
        STransformComponent() = default;
        explicit STransformComponent(const FTransform& InTransform)
            : LocalTransform(InTransform)
            , WorldTransform(InTransform)
        {}
    
        FUNCTION()
        FVector3 GetLocalLocation() const { return LocalTransform.GetLocation(); }

        FUNCTION()
        FQuat GetLocalRotation() const { return LocalTransform.GetRotation(); }

        FUNCTION()
        FVector3 GetLocalScale()    const { return LocalTransform.GetScale(); }

        FUNCTION()
        FVector3 GetLocalRotationAsEuler() const
        {
            return Math::Degrees(Math::EulerAngles(LocalTransform.GetRotation()));
        }

        FUNCTION()
        FVector3 SetLocalLocation(const FVector3& InLocation)
        {
            LocalTransform.SetLocation(InLocation);
            MarkDirty();
            return InLocation;
        }

        FUNCTION()
        FVector3 Translate(const FVector3& Delta)
        {
            LocalTransform.Translate(Delta);
            MarkDirty();
            return LocalTransform.GetLocation();
        }

        FUNCTION()
        FQuat SetLocalRotation(const FQuat& InRotation)
        {
            LocalTransform.SetRotation(InRotation);
            MarkDirty();
            return InRotation;
        }

        FUNCTION()
        FVector3 SetLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.SetRotationFromEuler(EulerDegrees);
            MarkDirty();
            return GetLocalRotationAsEuler();
        }

        FUNCTION()
        FVector3 AddLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.Rotate(EulerDegrees);
            MarkDirty();
            return GetLocalRotationAsEuler();
        }

        FUNCTION()
        void AddYaw(float Degrees)
        {
            LocalTransform.AddYawRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        FUNCTION()
        void AddPitch(float Degrees, float ClampMin = -89.9f, float ClampMax = 89.9f)
        {
            LocalTransform.AddPitchRadians(Math::Radians(Math::Clamp(Degrees, ClampMin, ClampMax)));
            MarkDirty();
        }

        FUNCTION()
        void AddRoll(float Degrees)
        {
            LocalTransform.AddRollRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        FUNCTION()
        FVector3 SetLocalScale(const FVector3& InScale)
        {
            LocalTransform.SetScale(InScale);
            MarkDirty();
            return InScale;
        }

        // Derived, not stored: bIsFlat means world == local, so WorldTransform is left stale for those.
        FORCEINLINE const FTransform& GetWorldTransformCached() const
        {
            return bIsFlat ? LocalTransform : WorldTransform;
        }

        // World getters opt out of SuppressGCTransition: a dirty-chain resolve can exceed the ~1us budget.
        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldLocation() const
        {
            ResolveIfDirty();
            return GetWorldTransformCached().GetLocation();
        }

        FUNCTION(NoSuppressGCTransition)
        FQuat GetWorldRotation() const
        {
            ResolveIfDirty();
            return GetWorldTransformCached().GetRotation();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldScale() const
        {
            ResolveIfDirty();
            return GetWorldTransformCached().GetScale();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldRotationAsEuler() const
        {
            ResolveIfDirty();
            return Math::Degrees(Math::EulerAngles(GetWorldTransformCached().GetRotation()));
        }

        // Composed on demand rather than cached: the matrix is ~20 SIMD instructions out of a
        // WorldTransform that is already in cache here, where storing it cost 64 bytes -- a third of the
        // component -- on every entity, paid by every pass that strides this pool for anything else.
        FUNCTION(NoSuppressGCTransition)
        FMatrix4 GetWorldMatrix() const
        {
            ResolveIfDirty();
            return GetWorldTransformCached().GetMatrix();
        }

        FUNCTION(NoSuppressGCTransition)
        const FTransform& GetWorldTransform() const
        {
            ResolveIfDirty();
            return GetWorldTransformCached();
        }

        // Cached world transform WITHOUT a resolve. All by value; the SIMD transform has no scalar member
        // to reference and the matrix is composed on the spot.
        FVector3 GetWorldLocationCached() const { return GetWorldTransformCached().GetLocation(); }
        FQuat    GetWorldRotationCached() const { return GetWorldTransformCached().GetRotation(); }
        FVector3 GetWorldScaleCached()    const { return GetWorldTransformCached().GetScale(); }
        FMatrix4 GetWorldMatrixCached()   const { return GetWorldTransformCached().GetMatrix(); }

        FUNCTION(NoSuppressGCTransition)
        void SetWorldTransform(const FTransform& InTransform)
        {
            if (Registry)
            {
                ECS::Utils::SetEntityWorldTransform(*Registry, Entity, InTransform);
            }
        }
        
        FUNCTION()
        void SetLocalTransform(const FTransform& InTransform)
        {
            LocalTransform = InTransform;
            MarkDirty();
        }

        // A property editor stores into LocalTransform's bytes directly, so no setter runs to raise the dirty signal.
        void PostEditChange(const FPropertyChangedEvent& Event)
        {
            if (DirtyState != nullptr)
            {
                MarkDirty();
            }
        }
    
        FUNCTION(NoSuppressGCTransition)
        FVector3 GetForward() const
        {
            ResolveIfDirty();
            return LocalTransform.GetForward();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetRight()   const
        {
            ResolveIfDirty();
            return LocalTransform.GetRight();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetUp()      const
        {
            ResolveIfDirty();
            return LocalTransform.GetUp();
        }
    
        FUNCTION()
        float MaxScale() const
        {
            const FVector3 S = LocalTransform.GetScale();
            return Math::Max(S.x, Math::Max(S.y, S.z));
        }
    
        FUNCTION()
        FVector3 GetLocation() const { return GetLocalLocation(); }
    
        FUNCTION()
        FVector3 GetPosition() const { return GetLocalLocation(); }
    
        FUNCTION()
        FQuat GetRotation() const { return GetLocalRotation(); }
    
        FUNCTION()
        FVector3 GetScale()    const { return GetLocalScale(); }
    
        FUNCTION()
        FVector3 SetLocation(const FVector3& L)    { return SetLocalLocation(L); }
    
        FUNCTION()
        FQuat SetRotation(const FQuat& R)    { return SetLocalRotation(R); }
    
        FUNCTION()
        FVector3 SetScale(const FVector3& S)       { return SetLocalScale(S); }
    
        FUNCTION()
        FVector3 SetRotationFromEuler(const FVector3& E)  { return SetLocalRotationFromEuler(E); }
    
        FUNCTION()
        FVector3 AddRotationFromEuler(const FVector3& E)  { return AddLocalRotationFromEuler(E); }
    
        FUNCTION()
        FVector3 GetRotationAsEuler() const { return GetLocalRotationAsEuler(); }

        // Bind this component to its owning entity. Called after duplication or post-load to rewire the
        // self-referential pointers used by MarkDirty/ResolveIfDirty. World init also does this directly via friend access.
        void Bind(ECS::FRegistry& InRegistry, ECS::FEntity InEntity)
        {
            Registry = &InRegistry;
            Entity = InEntity;
            DirtyState = ECS::Utils::EnsureTransformDirtyGate(InRegistry);

            // Re-derived rather than carried over: a bit-copied component's flag describes the SOURCE
            // entity's hierarchy, and this copy may be parented differently.
            bIsFlat = ECS::Utils::IsEntityTransformFlat(InRegistry, InEntity);
        }
        
        void SetRaw(const FVector3& Location, const FQuat& Rotation)
        {
            LocalTransform.SetLocation(Location);
            LocalTransform.SetRotation(Rotation);
        }

        void SetRaw(const FVector3& Location, const FQuat& Rotation, const FVector3& Scale)
        {
            LocalTransform.SetLocation(Location);
            LocalTransform.SetRotation(Rotation);
            LocalTransform.SetScale(Scale);
        }

    public:

        /** Local-space transform relative to the entity's parent (or world if no parent). */
        PROPERTY(Editable, Category = "Transform")
        FTransform LocalTransform;

        // Never read directly; a flat entity leaves it stale. See GetWorldTransformCached.
        FTransform WorldTransform;

        // Do not move the fields below onto LocalTransform's cache line; measured at +7ns/setter if you do.

        // Set by setters, cleared by the resolver. Component-local so writes are ParallelFor-safe.
        mutable bool bWorldDirty = false;

        // Maintained on the game thread by the physics on_construct/on_destroy hooks.
        bool bHasPhysicsBody = false;
        void SetHasPhysicsBody(bool bInHasBody) { bHasPhysicsBody = bInHasBody; }

        // Dedup guard for the DirtyBodies queue
        mutable bool bBodyDirtyQueued = false;

        /**
         * Cached "this entity has no FRelationshipComponent", which is exactly what the resolve tests to
         * take its flat path. When set, a setter resolves itself (world == local) instead of queueing --
         * see MarkDirty.
         *
         * FALSE IS ALWAYS SAFE; only true is a claim. A missed update costs the slow path, never a wrong
         * transform, which is what makes maintaining it out of band acceptable. Kept current by
         * CWorld::OnTransformComponentConstruct, the on_construct<FRelationshipComponent> hook, and Bind
         * (duplication / post-load). Lands in existing padding, so it costs no bytes.
         */
        bool bIsFlat = false;

        // Publish-once-per-frame guard for the flat path, compared against FTransformDirtyGate::PublishEpoch.
        // Also lands in existing padding. Wraps after 2^32 drains, where the only consequence is one entity
        // skipping one frame's render notification.
        mutable uint32 LastPublishEpoch = 0;

        // Clear both dirty guards without touching the queues. For a freshly bit-copied component, whose flags
        // describe the *source* entity's queue state and would otherwise suppress the copy's own enqueues.
        void ResetDirtyState()
        {
            bWorldDirty      = false;
            bBodyDirtyQueued = false;

            // Same hazard, one step removed: a copied epoch is a claim about the SOURCE registry's drain
            // count, and if it happens to match the destination's current epoch the flat path suppresses
            // the copy's first move notification. Zero can still collide, once every 2^32 drains, at the
            // documented cost of one skipped notification - the same odds the wrap already carries.
            LastPublishEpoch = 0;
        }

    private:
        
        void MarkDirty()
        {
            const bool bQueueBody = bHasPhysicsBody && !bBodyDirtyQueued;

            // Flat fast path. For an entity with no relationship component the resolve's whole job is
            // "world = local", so doing it here -- with the transform still in registers -- skips the dirty
            // enqueue, the drain, and the random-access get the resolve would have paid to come back for it.
            // The deferral cost more than the work it deferred.
            if (bIsFlat)
            {
                // No world write: GetWorldTransformCached derives it from LocalTransform while bIsFlat holds.
                const uint32 Epoch   = (DirtyState != nullptr)
                                     ? DirtyState->PublishEpoch.load(std::memory_order_relaxed) : 0u;
                const bool bPublish  = (LastPublishEpoch != Epoch);
                LastPublishEpoch     = Epoch;
                bBodyDirtyQueued    |= bQueueBody;

                // bWorldDirty stays clear: nothing is owed. The dirty signal stays down too, so the next
                // barrier can still skip, and later GetWorld* reads take the guard-free path and find the
                // value already correct.
                if (bPublish || bQueueBody)
                {
                    ECS::Utils::PublishFlatMove(DirtyState, Entity, bPublish, bQueueBody);
                }
                return;
            }

            const bool bQueueTransform = !bWorldDirty;

            if (!bQueueTransform && !bQueueBody)
            {
                return;
            }

            bWorldDirty       = true;
            bBodyDirtyQueued |= bQueueBody;

            ECS::Utils::QueueDirtyTransform(DirtyState, Entity, bQueueTransform, bQueueBody);
        }

        void ResolveIfDirty() const
        {
            // The whole point of the pre-batch barrier is that this is the common case: nothing has moved
            // since the last resolve, so the read is already correct. Answered here with one relaxed load
            // through a pointer cached at Bind. ResolveTransformChain would make the same decision, but only
            // after re-deriving the dirty state from the registry context -- a type-keyed hash lookup on
            // every world read in the engine.
            if (DirtyState != nullptr && !DirtyState->bAnyDirty.load(std::memory_order_acquire))
            {
                return;
            }

            if (Registry)
            {
                ECS::Utils::ResolveTransformChain(*Registry, Entity);
            }
        }

        ECS::FRegistry*                 Registry = nullptr;
        ECS::FEntity                     Entity   = ECS::NullEntity;
        ECS::Utils::FTransformDirtyGate* DirtyState = nullptr;
    };

    // CACHE_ALIGN is load-bearing, not decoration: the parallel resolve writes WorldTransform and the
    // dirty flag for arbitrary entities across worker threads, and neighbors in the dense pool land on
    // different threads, so anything that lets two components share a line false-shares.
    //
    // The corollary is that this size is QUANTIZED to 64. Shaving a few bytes off the tail buys nothing --
    // it pads straight back up -- and only a cut that crosses a boundary changes what a pass fetches. This
    // caught the 192 -> 128 step (deleting a cached world matrix); the next one is 128 -> 64, which needs
    // WorldTransform to move to its own pool.
    static_assert(sizeof(STransformComponent) == 128,
        "STransformComponent changed size. It is quantized to 64B; see the note above before adjusting this.");

    // Display-time world matrix, present only on entities the physics step interpolates. Render reads it in
    // preference to STransformComponent's own world pose; nothing else may. STransformComponent holds the
    // simulated pose, so gameplay, queries and the body re-sync all agree with the simulated pose, while the
    // visual is free to sit between two fixed steps. Not reflected: pure per-frame render state.
    struct FRenderTransform
    {
        FMatrix4 Matrix = FMatrix4(1.0f);
    };
    
}
