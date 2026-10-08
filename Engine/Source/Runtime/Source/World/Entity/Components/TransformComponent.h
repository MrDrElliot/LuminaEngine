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

    // Only LocalTransform is written, and WorldTransform follows at the frame's propagation, so GetWorld* computes the current value while GetWorld*Cached lags a moved child by a frame.
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

        // Both at once, so a caller placing an entity marks it dirty once rather than twice.
        void SetLocalLocationAndRotation(const FVector3& InLocation, const FQuat& InRotation)
        {
            LocalTransform.SetLocation(InLocation);
            LocalTransform.SetRotation(InRotation);
            MarkDirty();
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

        // Writes nothing and takes no lock, so any thread may read a world transform between propagations.
        FTransform ComputeWorldTransform() const
        {
            if (bIsFlat)
            {
                return LocalTransform;
            }

            // Nothing has moved since the last propagation in the common case, so the cached value is exact.
            if (Registry == nullptr || (DirtyState != nullptr && !DirtyState->bAnyDirty.load(std::memory_order_acquire)))
            {
                return WorldTransform;
            }

            return ECS::Utils::ComputeWorldTransform(*Registry, Entity);
        }

        // World getters opt out of SuppressGCTransition: a dirty-chain resolve can exceed the ~1us budget.
        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldLocation() const
        {
            return ComputeWorldTransform().GetLocation();
        }

        FUNCTION(NoSuppressGCTransition)
        FQuat GetWorldRotation() const
        {
            return ComputeWorldTransform().GetRotation();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldScale() const
        {
            return ComputeWorldTransform().GetScale();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldRotationAsEuler() const
        {
            return Math::Degrees(Math::EulerAngles(ComputeWorldTransform().GetRotation()));
        }

        // Composed on demand rather than cached: the matrix is ~20 SIMD instructions out of a
        // WorldTransform that is already in cache here, where storing it cost 64 bytes -- a third of the
        // component -- on every entity, paid by every pass that strides this pool for anything else.
        FUNCTION(NoSuppressGCTransition)
        FMatrix4 GetWorldMatrix() const
        {
            return ComputeWorldTransform().GetMatrix();
        }

        FUNCTION(NoSuppressGCTransition)
        FTransform GetWorldTransform() const
        {
            return ComputeWorldTransform();
        }

        // The world transform as of the last propagation, which a child moved since then has not caught up with.
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

        // World rotation against the parent's current world rotation, computed without a lock so parallel writers of disjoint entities may call it.
        FUNCTION()
        void SetWorldRotationCached(const FQuat& InRotation)
        {
            if (Registry)
            {
                ECS::Utils::SetEntityWorldRotationCached(*Registry, Entity, InRotation);
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
    
        FUNCTION()
        FVector3 GetForward() const
        {
            return LocalTransform.GetForward();
        }

        FUNCTION()
        FVector3 GetRight()   const
        {
            return LocalTransform.GetRight();
        }

        FUNCTION()
        FVector3 GetUp()      const
        {
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
        // self-referential pointers used by MarkDirty and ComputeWorldTransform. World init also does this directly via friend access.
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

        // The simulated pose coming back from the solver. The body is already there, so it is not re-queued.
        void SetFromPhysics(const FVector3& Location, const FQuat& Rotation)
        {
            LocalTransform.SetLocation(Location);
            LocalTransform.SetRotation(Rotation);
            MarkMoved(false);
        }

        // For a parallel writer that batches the dirty enqueue itself; true when this entity still has to be queued.
        bool SetFromPhysicsUnqueued(const FVector3& Location, const FQuat& Rotation)
        {
            if (bIsFlat)
            {
                SetFromPhysics(Location, Rotation);
                return false;
            }
            LocalTransform.SetLocation(Location);
            LocalTransform.SetRotation(Rotation);
            const bool bNeedsQueue = !bWorldDirty;
            bWorldDirty = true;
            return bNeedsQueue;
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
            MarkMoved(bHasPhysicsBody && !bBodyDirtyQueued);
        }

        void MarkMoved(bool bQueueBody)
        {
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

                // Nothing is owed, so bWorldDirty and the dirty signal both stay down and the next propagation can skip.
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
}
