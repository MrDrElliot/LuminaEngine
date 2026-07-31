#pragma once

#include <atomic>
#include "Core/Math/Math.h"
#include "Core/Threading/Thread.h"
#include "DirtyComponent.h"
#include "Core/Math/Transform.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "TransformComponent.generated.h"

namespace Lumina
{
    
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
            , CachedMatrix(InTransform.GetMatrix())
        {}
    
        FUNCTION(Script)
        FVector3 GetLocalLocation() const { return LocalTransform.GetLocation(); }

        FUNCTION(Script)
        FQuat GetLocalRotation() const { return LocalTransform.GetRotation(); }

        FUNCTION(Script)
        FVector3 GetLocalScale()    const { return LocalTransform.GetScale(); }

        FUNCTION(Script)
        FVector3 GetLocalRotationAsEuler() const
        {
            return Math::Degrees(Math::EulerAngles(LocalTransform.GetRotation()));
        }

        FUNCTION(Script)
        FVector3 SetLocalLocation(const FVector3& InLocation)
        {
            LocalTransform.SetLocation(InLocation);
            MarkDirty();
            return InLocation;
        }

        FUNCTION(Script)
        FVector3 Translate(const FVector3& Delta)
        {
            LocalTransform.Translate(Delta);
            MarkDirty();
            return LocalTransform.GetLocation();
        }

        FUNCTION(Script)
        FQuat SetLocalRotation(const FQuat& InRotation)
        {
            LocalTransform.SetRotation(InRotation);
            MarkDirty();
            return InRotation;
        }

        FUNCTION(Script)
        FVector3 SetLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.SetRotationFromEuler(EulerDegrees);
            MarkDirty();
            return GetLocalRotationAsEuler();
        }

        FUNCTION(Script)
        FVector3 AddLocalRotationFromEuler(const FVector3& EulerDegrees)
        {
            LocalTransform.Rotate(EulerDegrees);
            MarkDirty();
            return GetLocalRotationAsEuler();
        }

        FUNCTION(Script)
        void AddYaw(float Degrees)
        {
            LocalTransform.AddYawRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        FUNCTION(Script)
        void AddPitch(float Degrees, float ClampMin = -89.9f, float ClampMax = 89.9f)
        {
            LocalTransform.AddPitchRadians(Math::Radians(Math::Clamp(Degrees, ClampMin, ClampMax)));
            MarkDirty();
        }

        FUNCTION(Script)
        void AddRoll(float Degrees)
        {
            LocalTransform.AddRollRadians(Math::Radians(Degrees));
            MarkDirty();
        }

        FUNCTION(Script)
        FVector3 SetLocalScale(const FVector3& InScale)
        {
            LocalTransform.SetScale(InScale);
            MarkDirty();
            return InScale;
        }

        // World getters opt out of SuppressGCTransition: a dirty-chain resolve can exceed the ~1us budget.
        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetWorldLocation() const
        {
            ResolveIfDirty();
            return WorldTransform.GetLocation();
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FQuat GetWorldRotation() const
        {
            ResolveIfDirty();
            return WorldTransform.GetRotation();
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetWorldScale() const
        {
            ResolveIfDirty();
            return WorldTransform.GetScale();
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetWorldRotationAsEuler() const
        {
            ResolveIfDirty();
            return Math::Degrees(Math::EulerAngles(WorldTransform.GetRotation()));
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FMatrix4 GetWorldMatrix() const
        {
            ResolveIfDirty();
            return CachedMatrix;
        }

        FUNCTION(Script, NoSuppressGCTransition)
        const FTransform& GetWorldTransform() const
        {
            ResolveIfDirty();
            return WorldTransform;
        }

        // Cached world transform WITHOUT a resolve. By value (the SIMD transform has no scalar member to
        // reference); CachedMatrix is still returned by const ref.
        FVector3        GetWorldLocationCached() const { return WorldTransform.GetLocation(); }
        FQuat           GetWorldRotationCached() const { return WorldTransform.GetRotation(); }
        FVector3        GetWorldScaleCached()    const { return WorldTransform.GetScale(); }
        const FMatrix4& GetWorldMatrixCached()   const { return CachedMatrix; }

        FUNCTION(Script, NoSuppressGCTransition)
        void SetWorldTransform(const FTransform& InTransform)
        {
            if (Registry)
            {
                ECS::Utils::SetEntityWorldTransform(*Registry, Entity, InTransform);
            }
        }
        
        FUNCTION(Script)
        void SetLocalTransform(const FTransform& InTransform)
        {
            LocalTransform = InTransform;
            MarkDirty();
        }
    
        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetForward() const
        {
            ResolveIfDirty();
            return LocalTransform.GetForward();
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetRight()   const
        {
            ResolveIfDirty();
            return LocalTransform.GetRight();
        }

        FUNCTION(Script, NoSuppressGCTransition)
        FVector3 GetUp()      const
        {
            ResolveIfDirty();
            return LocalTransform.GetUp();
        }
    
        FUNCTION(Script)
        float MaxScale() const
        {
            const FVector3 S = LocalTransform.GetScale();
            return Math::Max(S.x, Math::Max(S.y, S.z));
        }
    
        FUNCTION(Script)
        FVector3 GetLocation() const { return GetLocalLocation(); }
    
        FUNCTION(Script)
        FVector3 GetPosition() const { return GetLocalLocation(); }
    
        FUNCTION(Script)
        FQuat GetRotation() const { return GetLocalRotation(); }
    
        FUNCTION(Script)
        FVector3 GetScale()    const { return GetLocalScale(); }
    
        FUNCTION(Script)
        FVector3 SetLocation(const FVector3& L)    { return SetLocalLocation(L); }
    
        FUNCTION(Script)
        FQuat SetRotation(const FQuat& R)    { return SetLocalRotation(R); }
    
        FUNCTION(Script)
        FVector3 SetScale(const FVector3& S)       { return SetLocalScale(S); }
    
        FUNCTION(Script)
        FVector3 SetRotationFromEuler(const FVector3& E)  { return SetLocalRotationFromEuler(E); }
    
        FUNCTION(Script)
        FVector3 AddRotationFromEuler(const FVector3& E)  { return AddLocalRotationFromEuler(E); }
    
        FUNCTION(Script)
        FVector3 GetRotationAsEuler() const { return GetLocalRotationAsEuler(); }

        // Bind this component to its owning entity. Called after duplication or post-load to rewire the
        // self-referential pointers used by MarkDirty/ResolveIfDirty. World init also does this directly via friend access.
        void Bind(FEntityRegistry& InRegistry, entt::entity InEntity)
        {
            Registry = &InRegistry;
            Entity = InEntity;
            DirtyState = ECS::Utils::EnsureTransformDirtyState(InRegistry);
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

        FTransform WorldTransform;
        FMatrix4  CachedMatrix = FMatrix4(1.f);

        // Set by setters, cleared by the resolver. Component-local so writes are ParallelFor-safe.
        mutable bool bWorldDirty = false;

        // Maintained on the game thread by the physics on_construct/on_destroy hooks.
        bool bHasPhysicsBody = false;
        void SetHasPhysicsBody(bool bInHasBody) { bHasPhysicsBody = bInHasBody; }

        // Dedup guard for the DirtyBodies queue
        mutable bool bBodyDirtyQueued = false;

        // Clear both dirty guards without touching the queues. For a freshly bit-copied component, whose flags
        // describe the *source* entity's queue state and would otherwise suppress the copy's own enqueues.
        void ResetDirtyState()
        {
            bWorldDirty      = false;
            bBodyDirtyQueued = false;
        }

    private:
        
        void MarkDirty() const
        {
            const bool bQueueTransform = !bWorldDirty;
            const bool bQueueBody      = bHasPhysicsBody && !bBodyDirtyQueued;

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
            if (Registry)
            {
                ECS::Utils::ResolveTransformChain(*Registry, Entity);
            }
        }

        FEntityRegistry*              Registry = nullptr;
        entt::entity                  Entity   = entt::null;
        ECS::Utils::FTransformDirtyState* DirtyState = nullptr;
    };

    // Display-time world matrix, present only on entities the physics step interpolates. Render reads it in
    // preference to STransformComponent::CachedMatrix; nothing else may. STransformComponent holds the
    // simulated pose, so gameplay, queries and the body re-sync all agree with what Jolt believes, while the
    // visual is free to sit between two fixed steps. Not reflected: pure per-frame render state.
    struct FRenderTransform
    {
        FMatrix4 Matrix = FMatrix4(1.0f);
    };
    
}
