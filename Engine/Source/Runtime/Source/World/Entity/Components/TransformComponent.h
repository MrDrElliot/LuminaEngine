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

    // Every accessor names its space, Local relative to the parent and World in the world. Only LocalTransform is stored, so GetWorld* computes the current value while GetWorld*Cached lags a moved child by a frame.
    REFLECT(Component, HideInComponentList, ScriptFastCalls)
    struct RUNTIME_API CACHE_ALIGN STransformComponent
    {
        GENERATED_BODY()
    
        friend class CWorld;

        // World reads and writes walk the parent chain, so a query filter never proves two transform writers apart.
        static constexpr bool ReachesOtherEntities = true;
    
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
        void SetLocalLocation(const FVector3& InLocation)
        {
            LocalTransform.SetLocation(InLocation);
            MarkDirty();
        }

        // Both at once, so a caller placing an entity marks it dirty once rather than twice.
        void SetLocalLocationAndRotation(const FVector3& InLocation, const FQuat& InRotation)
        {
            LocalTransform.SetLocation(InLocation);
            LocalTransform.SetRotation(InRotation);
            MarkDirty();
        }

        FUNCTION()
        void AddLocalLocation(const FVector3& Delta)
        {
            LocalTransform.Translate(Delta);
            MarkDirty();
        }

        FUNCTION()
        void SetLocalRotation(const FQuat& InRotation)
        {
            LocalTransform.SetRotation(InRotation);
            MarkDirty();
        }

        FUNCTION()
        void SetLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.SetRotationFromEuler(EulerDegrees);
            MarkDirty();
        }

        FUNCTION()
        void AddLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.Rotate(EulerDegrees);
            MarkDirty();
        }

        FUNCTION()
        void AddYaw(float Degrees)
        {
            LocalTransform.AddYawRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        // Clamps the pitch it ends at, measured nose down from the horizon the way a positive Degrees turns it.
        FUNCTION()
        void AddPitch(float Degrees, float ClampMin = -89.9f, float ClampMax = 89.9f)
        {
            const float Current = -Math::Degrees(Math::Asin(Math::Clamp(LocalTransform.GetForward().y, -1.0f, 1.0f)));
            const float Target = Math::Clamp(Current + Degrees, ClampMin, ClampMax);
            LocalTransform.AddPitchRadians(Math::Radians(Target - Current));
            MarkDirty();
        }

        FUNCTION()
        void AddRoll(float Degrees)
        {
            LocalTransform.AddRollRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        FUNCTION()
        void SetLocalScale(const FVector3& InScale)
        {
            LocalTransform.SetScale(InScale);
            MarkDirty();
        }

        // Derived, not stored: bIsFlat means world == local, so WorldTransform is left stale for those.
        FORCEINLINE const FTransform& GetWorldTransformCached() const
        {
            return bIsFlat ? LocalTransform : WorldTransform;
        }

        // Writes nothing and takes no lock, so any thread may read a world transform between propagations.
        FTransform ComputeWorldTransform() const
        {
            // A transform never bound to a registry has no hierarchy, so its world transform is its local one.
            if (bIsFlat || Registry == nullptr)
            {
                return LocalTransform;
            }

            // Nothing has moved since the last propagation in the common case, so the cached value is exact.
            if (DirtyState != nullptr && !DirtyState->bAnyDirty.load(std::memory_order_acquire))
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

        // Composed on demand because storing the matrix cost 64 bytes on every entity, paid by every pass striding this pool.
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

        // The largest world scale axis, which is what physics sizes a sphere, capsule or cylinder by.
        float GetWorldMaxScaleCached() const
        {
            const FVector3 S = GetWorldScaleCached();
            return Math::Max(S.x, Math::Max(S.y, S.z));
        }

        // A transform with no parent is its own world transform, so only a parented one pays for the parent's inverse.
        FUNCTION(NoSuppressGCTransition)
        void SetWorldTransform(const FTransform& InTransform)
        {
            if (bIsFlat || Registry == nullptr)
            {
                SetLocalTransform(InTransform);
                return;
            }
            ECS::Utils::SetEntityWorldTransform(*Registry, Entity, InTransform);
        }

        FUNCTION(NoSuppressGCTransition)
        void SetWorldLocation(const FVector3& InLocation)
        {
            FTransform World = ComputeWorldTransform();
            World.SetLocation(InLocation);
            SetWorldTransform(World);
        }

        // Both at once, so a caller placing an entity resolves its parent once rather than twice.
        void SetWorldLocationAndRotation(const FVector3& InLocation, const FQuat& InRotation)
        {
            FTransform World = ComputeWorldTransform();
            World.SetLocation(InLocation);
            World.SetRotation(InRotation);
            SetWorldTransform(World);
        }

        FUNCTION(NoSuppressGCTransition)
        void SetWorldRotation(const FQuat& InRotation)
        {
            FTransform World = ComputeWorldTransform();
            World.SetRotation(InRotation);
            SetWorldTransform(World);
        }

        FUNCTION(NoSuppressGCTransition)
        void SetWorldRotationFromEuler(const FVector3& EulerDegrees)
        {
            FTransform World = ComputeWorldTransform();
            World.SetRotationFromEuler(EulerDegrees);
            SetWorldTransform(World);
        }

        FUNCTION(NoSuppressGCTransition)
        void SetWorldScale(const FVector3& InScale)
        {
            FTransform World = ComputeWorldTransform();
            World.SetScale(InScale);
            SetWorldTransform(World);
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
    
        // Relative to the parent, like every Local getter; GetWorldForward is the direction in the world.
        FUNCTION()
        FVector3 GetLocalForward() const
        {
            return LocalTransform.GetForward();
        }

        FUNCTION()
        FVector3 GetLocalRight() const
        {
            return LocalTransform.GetRight();
        }

        FUNCTION()
        FVector3 GetLocalUp() const
        {
            return LocalTransform.GetUp();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldForward() const
        {
            return ComputeWorldTransform().GetForward();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldRight() const
        {
            return ComputeWorldTransform().GetRight();
        }

        FUNCTION(NoSuppressGCTransition)
        FVector3 GetWorldUp() const
        {
            return ComputeWorldTransform().GetUp();
        }
    
        FUNCTION()
        float GetLocalMaxScale() const
        {
            const FVector3 S = LocalTransform.GetScale();
            return Math::Max(S.x, Math::Max(S.y, S.z));
        }

        // Rewires the self-referential pointers after duplication or post-load.
        void Bind(ECS::FRegistry& InRegistry, ECS::FEntity InEntity)
        {
            Registry = &InRegistry;
            Entity = InEntity;
            DirtyState = ECS::Utils::EnsureTransformDirtyGate(InRegistry);

            // Re-derived because a bit-copied flag describes the source entity's hierarchy.
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

        // Cached "not linked in the registry hierarchy", which lets a setter resolve itself. Only true is a claim.
        bool bIsFlat = false;

        // Publish-once-per-frame guard for the flat path
        mutable uint32 LastPublishEpoch = 0;

        // Clear both dirty guards without touching the queues. 
        void ResetDirtyState()
        {
            bWorldDirty      = false;
            bBodyDirtyQueued = false;
            LastPublishEpoch = 0;
        }

    private:
        
        void MarkDirty()
        {
            // An entity in a hierarchy may carry bodied descendants, which have to follow it.
            MarkMoved((bHasPhysicsBody || !bIsFlat) && !bBodyDirtyQueued);
        }

        void MarkMoved(bool bQueueBody)
        {
            // Flat fast path, since deferring world = local to the resolve cost more than doing it here.
            if (bIsFlat)
            {
                // No world write: GetWorldTransformCached derives it from LocalTransform while bIsFlat holds.
                const uint32 Epoch   = (DirtyState != nullptr)
                                     ? DirtyState->PublishEpoch.load(std::memory_order_relaxed) : 0u;
                const bool bPublish  = (LastPublishEpoch != Epoch);
                LastPublishEpoch     = Epoch;
                bBodyDirtyQueued    |= bQueueBody;

                // Offered on every move, not just the first of the frame, so a writer keeps the latest pose.
                const bool bTaken = ECS::Utils::TakeFlatMove(DirtyState, Entity, LocalTransform);

                // Nothing is owed, so bWorldDirty and the dirty signal both stay down and the next propagation can skip.
                if (bPublish || bQueueBody)
                {
                    ECS::Utils::PublishFlatMove(DirtyState, Entity, bPublish, bQueueBody, bTaken);
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

    // CACHE_ALIGN stops the parallel resolve false-sharing, and the size is quantized to 64 so only a cut across a boundary helps (next is 128 to 64).
    static_assert(sizeof(STransformComponent) == 128,
        "STransformComponent changed size. It is quantized to 64B; see the note above before adjusting this.");
}
