#include "RuntimePCH.h"
#include "JoltPhysicsScene.h"
#include "JoltCharacterHandle.h"
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include "Physics/Ray/RayCast.h"
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#if JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRendererSimple.h>
#include "Core/Utils/Defer.h"
#endif
#include <algorithm>
#include "Core/Math/Math.h"
#include "Core/Math/SIMD/SIMD.h"

#include "JoltPhysics.h"
#include "JoltUtils.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Profiler/Profile.h"
#include "Core/Threading/Thread.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/TaperedCapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/TaperedCylinderShape.h"
#include "Jolt/Physics/Collision/Shape/PlaneShape.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/CollidePointResult.h"
#include "Jolt/Physics/Collision/Shape/MeshShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/ScaledShape.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#include "Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "JoltRagdollHandle.h"
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Animation/Pose.h"
#include "Renderer/MeshData.h"
#include "Renderer/RendererUtils.h"
#include "World/World.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/DirtyComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/RagdollComponent.h"
#include "World/Entity/Components/CSharpScriptComponent.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "world/entity/components/velocitycomponent.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "World/Entity/Events/ImpulseEvent.h"
#include "World/Subsystems/WorldSettings.h"
#include "Log/Log.h"

using namespace JPH::literals;

namespace Lumina::Physics
{
    namespace BroadPhaseLayers
    {
        static constexpr JPH::BroadPhaseLayer STATIC(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr uint32 NUM_LAYERS(2);
    };
    
    class FLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
    {
    public:
        virtual uint32 GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
        {
            ECollisionProfiles LayerBits = (ECollisionProfiles)(uint16)(inLayer & 0xFFFF);

            if ((LayerBits & ECollisionProfiles::Static) != (ECollisionProfiles)0)
            {
                return BroadPhaseLayers::STATIC;
            }

            return BroadPhaseLayers::MOVING;
        }

        #if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
        {
            switch ((JPH::BroadPhaseLayer::Type)inLayer)
            {
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::STATIC:      return "STATIC";
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:      return "MOVING";
            default: JPH_ASSERT(false); return "INVALID";
            }
        }
        #endif
    };
    
    constexpr JPH::EMotionType ToJoltMotionType(EBodyType BodyType)
    {
        switch (BodyType)
        {
            case EBodyType::Static:     return JPH::EMotionType::Static;
            case EBodyType::Kinematic:  return JPH::EMotionType::Kinematic;
            case EBodyType::Dynamic:    return JPH::EMotionType::Dynamic;
        }

        UNREACHABLE();
    }
    
    static void CheckJoltError(JPH::EPhysicsUpdateError Error)
    {
        // EPhysicsUpdateError is a bit field; multiple errors can be set in one update.
        if (Error == JPH::EPhysicsUpdateError::None)
        {
            return;
        }

        if ((Error & JPH::EPhysicsUpdateError::ManifoldCacheFull) != JPH::EPhysicsUpdateError::None)
        {
            LOG_ERROR("[Jolt Error] - Manifold Cache is full; increase MaxPhysicsContactConstraints. Some contacts were dropped.");
        }
        if ((Error & JPH::EPhysicsUpdateError::BodyPairCacheFull) != JPH::EPhysicsUpdateError::None)
        {
            LOG_ERROR("[Jolt Error] - Body-Pair Cache is full; increase MaxPhysicsBodyPairs. Some contacts were dropped.");
        }
        if ((Error & JPH::EPhysicsUpdateError::ContactConstraintsFull) != JPH::EPhysicsUpdateError::None)
        {
            LOG_ERROR("[Jolt Error] - Contact Constraints buffer is full; increase MaxPhysicsContactConstraints. Some contacts were dropped.");
        }
    }
    
    class FObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        
        bool ShouldCollide(JPH::ObjectLayer LayerA, JPH::BroadPhaseLayer LayerB) const override
        {
            ECollisionProfiles LayerBits = (ECollisionProfiles)(uint16)(LayerA & 0xFFFF);

            if ((LayerBits & ECollisionProfiles::Static) != (ECollisionProfiles)0)
            {
                return LayerB == BroadPhaseLayers::STATIC;
            }

            return true;
        }
    };

    class FObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
    {
    public:
        
        bool ShouldCollide(JPH::ObjectLayer ObjectA, JPH::ObjectLayer ObjectB) const override
        {
            ECollisionProfiles LayerA = (ECollisionProfiles)(uint16)(ObjectA & 0xFFFF);
            ECollisionProfiles MaskA  = (ECollisionProfiles)(uint16)(ObjectA >> 16);
            ECollisionProfiles LayerB = (ECollisionProfiles)(uint16)(ObjectB & 0xFFFF);
            ECollisionProfiles MaskB  = (ECollisionProfiles)(uint16)(ObjectB >> 16);

            return (MaskA & LayerB) != (ECollisionProfiles)0 || (MaskB & LayerA) != (ECollisionProfiles)0;
        }
    };
    
    class FIgnoreFilter : public JPH::BodyFilter
    {
    public:
        
        FIgnoreFilter(TSpan<const uint32> InIgnoreBodies)
            : IgnoreBodies(InIgnoreBodies)
        {}
        
        bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            const uint32 Key = inBodyID.GetIndexAndSequenceNumber();
            for (std::size_t i = 0; i < IgnoreBodies.size(); i++)
            {
                if (IgnoreBodies[i] == Key)
                {
                    return false;
                }
            }
            return true;
        }
    
        TSpan<const uint32> IgnoreBodies;
    };

    // Rejects character inner bodies so a character's MOVEMENT collision ignores other characters' proxies;
    // char-vs-char is then handled solely by CharacterVsCharacterCollision (no double-collision). Only used on
    // the serial character update, so reading the set is race-free.
    class FCharacterProxyFilter : public JPH::BodyFilter
    {
    public:
        explicit FCharacterProxyFilter(const THashSet<uint32>& InProxies) : Proxies(InProxies) {}

        bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            return Proxies.find(inBodyID.GetIndexAndSequenceNumber()) == Proxies.end();
        }

        const THashSet<uint32>& Proxies;
    };

    static FLayerInterfaceImpl                  GJoltLayerInterface;
    static FObjectLayerPairFilterImpl           GObjectVsObjectLayerFilter;
    static FObjectVsBroadPhaseLayerFilterImpl   GObjectVsBroadPhaseLayerFilter;


    namespace
    {
        // Contact callbacks run with all bodies locked; velocities are pre-step.
        FContactRecord BuildContactRecord(EContactEventType Type,
                                          const JPH::Body& InBody1,
                                          const JPH::Body& InBody2,
                                          const JPH::ContactManifold& InManifold)
        {
            FContactRecord Record;
            Record.Type      = Type;
            Record.EntityA   = static_cast<entt::entity>(InBody1.GetUserData());
            Record.EntityB   = static_cast<entt::entity>(InBody2.GetUserData());
            Record.BodyIDA   = InBody1.GetID().GetIndexAndSequenceNumber();
            Record.BodyIDB   = InBody2.GetID().GetIndexAndSequenceNumber();
            Record.bSensorA  = InBody1.IsSensor();
            Record.bSensorB  = InBody2.IsSensor();

            JPH::Vec3 Sum = JPH::Vec3::sZero();
            const uint32 N = InManifold.mRelativeContactPointsOn1.size();
            if (N > 0)
            {
                for (uint32 i = 0; i < N; ++i)
                {
                    Sum += InManifold.mRelativeContactPointsOn1[i];
                }
                Sum /= float(N);
            }
            const JPH::RVec3 WorldPoint = InManifold.mBaseOffset + Sum;
            Record.Point  = JoltUtils::FromJPHVec3(WorldPoint);
            Record.Normal = JoltUtils::FromJPHVec3(InManifold.mWorldSpaceNormal);

            const JPH::Vec3 V1 = InBody1.GetLinearVelocity();
            const JPH::Vec3 V2 = InBody2.GetLinearVelocity();
            Record.VelocityA = JoltUtils::FromJPHVec3(V1);
            Record.VelocityB = JoltUtils::FromJPHVec3(V2);

            const JPH::Vec3 RelVel = V2 - V1;
            Record.ImpactSpeed = std::abs(RelVel.Dot(InManifold.mWorldSpaceNormal));
            return Record;
        }
    }

    void FJoltContactListener::ApplySurfaceVelocity(const JPH::Body& inBody1, const JPH::Body& inBody2, JPH::ContactSettings& ioSettings)
    {
        if (Scene == nullptr)
        {
            return;
        }

        const auto* SV1 = Scene->GetBodySurfaceVelocity(inBody1.GetID());
        const auto* SV2 = Scene->GetBodySurfaceVelocity(inBody2.GetID());

        const bool bActive1 = SV1 != nullptr && SV1->bActive;
        const bool bActive2 = SV2 != nullptr && SV2->bActive;
        if (!bActive1 && !bActive2)
        {
            return;
        }

        // Jolt wants the relative surface velocity = body2 - body1 (world space). The angular term is taken
        // about body1's center of mass (exact for a conveyor that is body1; an approximation if body2 spins).
        const FVector3 Lin1 = bActive1 ? SV1->Linear  : FVector3(0.0f);
        const FVector3 Lin2 = bActive2 ? SV2->Linear  : FVector3(0.0f);
        const FVector3 Ang1 = bActive1 ? SV1->Angular : FVector3(0.0f);
        const FVector3 Ang2 = bActive2 ? SV2->Angular : FVector3(0.0f);

        ioSettings.mRelativeLinearSurfaceVelocity  = JoltUtils::ToJPHVec3(Lin2 - Lin1);
        ioSettings.mRelativeAngularSurfaceVelocity = JoltUtils::ToJPHVec3(Ang2 - Ang1);
    }

    void FJoltContactListener::OnContactAdded(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings)
    {
        // Apply per-material combine first so the constraint uses the right friction/restitution
        // (Jolt invokes this once per contact pair per step before solving).
        OverrideFrictionAndRestitution(inBody1, inBody2, inManifold, ioSettings);
        ApplySurfaceVelocity(inBody1, inBody2, ioSettings);

        if (Scene)
        {
            Scene->EnqueueContactRecord(BuildContactRecord(EContactEventType::Added, inBody1, inBody2, inManifold));
        }
    }

    void FJoltContactListener::OnContactPersisted(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings)
    {
        // Don't dispatch to the bus (Enter/Exit cover scripts), but material combine + surface velocity must
        // still run each step or resting contacts revert to Jolt's defaults (and the conveyor stops dragging).
        OverrideFrictionAndRestitution(inBody1, inBody2, inManifold, ioSettings);
        ApplySurfaceVelocity(inBody1, inBody2, ioSettings);
    }

    void FJoltContactListener::OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair)
    {
        if (Scene == nullptr || BodyLockInterface == nullptr)
        {
            return;
        }

        // Bodies may have been removed by the time this fires; skip if either side is already gone.
        const JPH::Body* B1 = BodyLockInterface->TryGetBody(inSubShapePair.GetBody1ID());
        const JPH::Body* B2 = BodyLockInterface->TryGetBody(inSubShapePair.GetBody2ID());
        if (B1 == nullptr || B2 == nullptr)
        {
            return;
        }

        // OnContactRemoved: Jolt denies velocity read access here; GetLinearVelocity asserts (UB in release).
        FContactRecord Record;
        Record.Type      = EContactEventType::Removed;
        Record.EntityA   = static_cast<entt::entity>(B1->GetUserData());
        Record.EntityB   = static_cast<entt::entity>(B2->GetUserData());
        Record.BodyIDA   = B1->GetID().GetIndexAndSequenceNumber();
        Record.BodyIDB   = B2->GetID().GetIndexAndSequenceNumber();
        Record.bSensorA  = B1->IsSensor();
        Record.bSensorB  = B2->IsSensor();
        Record.Point     = FVector3(0.0f);
        Record.Normal    = FVector3(0.0f, 1.0f, 0.0f);
        Record.VelocityA = FVector3(0.0f);
        Record.VelocityB = FVector3(0.0f);
        Record.ImpactSpeed = 0.0f;

        Scene->EnqueueContactRecord(Record);
    }

    void FJoltPhysicsScene::EnqueueContactRecord(const FContactRecord& Record)
    {
        PendingContacts.enqueue(Record);
    }

    void FJoltPhysicsScene::EnqueueActivation(entt::entity Entity, bool bActivated)
    {
        PendingActivations.enqueue(FActivationRecord{ Entity, bActivated });
    }

    // Bulk-drain a staging queue into a reusable game-thread vector. Records are POD, so the stack batch
    // costs nothing beyond the copy the queue would do anyway.
    template <typename TRecord, typename TQueueType>
    static bool DrainRecords(TQueueType& Queue, TVector<TRecord>& Out)
    {
        Out.clear();

        TRecord     Batch[128];
        std::size_t Count;
        while ((Count = Queue.try_dequeue_bulk(Batch, 128)) != 0)
        {
            Out.insert(Out.end(), Batch, Batch + Count);
        }

        return !Out.empty();
    }

    namespace
    {
        // Orient a contact record for one receiving side; POD fill, no per-event table built downstream.
        SCollisionEvent BuildCollisionEvent(entt::entity SelfEntity,
                                            entt::entity OtherEntity,
                                            uint32 SelfBodyID,
                                            uint32 OtherBodyID,
                                            const FContactRecord& Record,
                                            bool bFlipNormal)
        {
            SCollisionEvent Event;
            Event.Entity      = SelfEntity;
            Event.Other       = OtherEntity;
            Event.BodyID      = SelfBodyID;
            Event.OtherBodyID = OtherBodyID;
            Event.Point       = Record.Point;

            // Convention: Normal points outward from self toward other so scripts
            // can react with `-Normal` to bounce away from the impact.
            Event.Normal           = bFlipNormal ? -Record.Normal : Record.Normal;
            Event.Velocity         = bFlipNormal ? Record.VelocityB : Record.VelocityA;
            Event.OtherVelocity    = bFlipNormal ? Record.VelocityA : Record.VelocityB;
            Event.RelativeVelocity = Event.OtherVelocity - Event.Velocity;
            Event.ImpactSpeed      = Record.ImpactSpeed;

            // bIsTrigger reports whether the OTHER side was a sensor; useful when the
            // same handler covers both kinds.
            Event.bIsTrigger = bFlipNormal ? Record.bSensorA : Record.bSensorB;
            return Event;
        }
    }

    void FJoltPhysicsScene::DispatchContactEvents()
    {
        if (!DrainRecords(PendingContacts, ContactDrainScratch))
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        // Contact is a solid impact, Overlap is a sensor/trigger side.
        auto Deliver = [&](entt::entity Self, entt::entity Other, uint32 SelfBody, uint32 OtherBody,
                           const FContactRecord& Record, bool bFlipNormal, bool bIsAdded, bool bIsOverlap)
        {
            if (Self == entt::null || !Registry.valid(Self))
            {
                return;
            }

            SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Self);
            if (Body == nullptr)
            {
                return;
            }

            TScriptDelegate<SCollisionEvent>& Delegate = bIsOverlap
                ? (bIsAdded ? Body->OnOverlapBegin : Body->OnOverlapEnd)
                : (bIsAdded ? Body->OnContactBegin : Body->OnContactEnd);

            if (!Delegate.IsBound())
            {
                return;
            }

            const SCollisionEvent Event = BuildCollisionEvent(Self, Other, SelfBody, OtherBody, Record, bFlipNormal);
            Delegate.Broadcast(Event);
        };

        for (const FContactRecord& Record : ContactDrainScratch)
        {
            const bool bAdded = (Record.Type == EContactEventType::Added);
            const bool bOverlap = Record.bSensorA || Record.bSensorB;

            Deliver(Record.EntityA, Record.EntityB, Record.BodyIDA, Record.BodyIDB,
                    Record, /*bFlipNormal*/ false, bAdded, bOverlap);
            Deliver(Record.EntityB, Record.EntityA, Record.BodyIDB, Record.BodyIDA,
                    Record, /*bFlipNormal*/ true,  bAdded, bOverlap);
        }
    }

    void FJoltPhysicsScene::DispatchActivationEvents()
    {
        if (!DrainRecords(PendingActivations, ActivationDrainScratch))
        {
            return;
        }

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
        for (const FActivationRecord& Record : ActivationDrainScratch)
        {
            if (Record.Entity == entt::null || !Registry.valid(Record.Entity))
            {
                continue;
            }

            SRigidBodyComponent* Body = Registry.try_get<SRigidBodyComponent>(Record.Entity);
            if (Body == nullptr)
            {
                continue;
            }

            // OnWake also fires on spawn (the body becomes active).
            FScriptDelegate& Delegate = Record.bActivated ? Body->OnWake : Body->OnSleep;
            Delegate.Broadcast();
        }
    }

    namespace
    {
        // Combine a pair of values under a combine mode. The pair's effective mode is the max of
        // each side's mode (Max > Min > Multiply > Average) so a "sticky" surface always wins.
        FORCEINLINE float CombineUnder(EPhysicsMaterialCombineMode Mode, float A, float B)
        {
            switch (Mode)
            {
            case EPhysicsMaterialCombineMode::Min:      return Math::Min(A, B);
            case EPhysicsMaterialCombineMode::Multiply: return A * B;
            case EPhysicsMaterialCombineMode::Max:      return Math::Max(A, B);
            case EPhysicsMaterialCombineMode::Average:
            default:                                    return (A + B) * 0.5f;
            }
        }

        FORCEINLINE EPhysicsMaterialCombineMode PairMode(uint8 A, uint8 B)
        {
            // Enum order (Average=0, Min=1, Multiply=2, Max=3) chosen so raw max yields the
            // documented precedence.
            return (EPhysicsMaterialCombineMode)Math::Max<uint8>(A, B);
        }
    }

    void FJoltContactListener::OverrideFrictionAndRestitution(const JPH::Body& inBody1, const JPH::Body& inBody2, const JPH::ContactManifold& inManifold, JPH::ContactSettings& ioSettings)
    {
        const auto* M1 = Scene->GetBodyMaterial(inBody1.GetID());
        const auto* M2 = Scene->GetBodyMaterial(inBody2.GetID());

        // No materials on either side: leave Jolt's defaults (already populated from the body's
        // mFriction / mRestitution by the contact solver).
        if ((M1 == nullptr || !M1->bHasMaterial) && (M2 == nullptr || !M2->bHasMaterial))
        {
            return;
        }

        const float F1 = (M1 && M1->bHasMaterial) ? M1->Friction       : inBody1.GetFriction();
        const float R1 = (M1 && M1->bHasMaterial) ? M1->Restitution    : inBody1.GetRestitution();
        const float F2 = (M2 && M2->bHasMaterial) ? M2->Friction       : inBody2.GetFriction();
        const float R2 = (M2 && M2->bHasMaterial) ? M2->Restitution    : inBody2.GetRestitution();

        const uint8 F1Mode = (M1 && M1->bHasMaterial) ? M1->FrictionCombine    : (uint8)EPhysicsMaterialCombineMode::Average;
        const uint8 F2Mode = (M2 && M2->bHasMaterial) ? M2->FrictionCombine    : (uint8)EPhysicsMaterialCombineMode::Average;
        const uint8 R1Mode = (M1 && M1->bHasMaterial) ? M1->RestitutionCombine : (uint8)EPhysicsMaterialCombineMode::Average;
        const uint8 R2Mode = (M2 && M2->bHasMaterial) ? M2->RestitutionCombine : (uint8)EPhysicsMaterialCombineMode::Average;

        ioSettings.mCombinedFriction    = CombineUnder(PairMode(F1Mode, F2Mode), F1, F2);
        ioSettings.mCombinedRestitution = CombineUnder(PairMode(R1Mode, R2Mode), R1, R2);
    }

    void FJoltContactListener::GetFrictionAndRestitution(const JPH::Body& inBody, const JPH::SubShapeID& inSubShapeID, float& outFriction, float& outRestitution) const
    {
        const auto* M = Scene->GetBodyMaterial(inBody.GetID());
        if (M != nullptr && M->bHasMaterial)
        {
            outFriction    = M->Friction;
            outRestitution = M->Restitution;
            return;
        }
        outFriction    = inBody.GetFriction();
        outRestitution = inBody.GetRestitution();
    }
    
    // CharacterVirtual contact listener. Reports the capsule's own contacts -- the ones the kinematic inner
    // body doesn't generate, i.e. STATIC world geometry (walls, floors, terrain). Dynamic/kinematic contacts
    // already reach the character's script through the inner body via the rigid FJoltContactListener (the
    // inner body shares the capsule shape + carries the entity user data), so dispatching them here too would
    // double-report; we skip them. Runs on the (parallel) character substep, so it only appends to the
    // thread-safe contact queue, draining game-thread as the character's OnContactBegin.
    class FJoltCharacterContactListener : public JPH::CharacterContactListener
    {
    public:
        FJoltCharacterContactListener(FJoltPhysicsScene* InScene, const JPH::BodyLockInterfaceNoLock* InLock)
            : Scene(InScene)
            , BodyLockInterface(InLock)
        { }

        virtual void OnContactAdded(const JPH::CharacterVirtual* inCharacter, const JPH::BodyID& inBodyID2,
            const JPH::SubShapeID& /*inSubShapeID2*/, JPH::RVec3Arg inContactPosition, JPH::Vec3Arg inContactNormal,
            JPH::CharacterContactSettings& /*ioSettings*/) override
        {
            const entt::entity CharacterEntity = static_cast<entt::entity>(static_cast<uint32>(inCharacter->GetUserData()));

            entt::entity OtherEntity   = entt::null;
            bool         bOtherStatic  = false;
            if (const JPH::Body* OtherBody = BodyLockInterface->TryGetBody(inBodyID2))
            {
                bOtherStatic = OtherBody->IsStatic();
                OtherEntity  = static_cast<entt::entity>(static_cast<uint32>(OtherBody->GetUserData()));
            }
            if (!bOtherStatic)
            {
                return;     // Dynamic/kinematic contacts arrive via the inner body's rigid contact.
            }

            FContactRecord Record{};
            Record.Type        = EContactEventType::Added;
            Record.EntityA     = CharacterEntity;
            Record.EntityB     = OtherEntity;
            Record.BodyIDA     = 0xFFFFFFFFu;
            Record.BodyIDB     = inBodyID2.GetIndexAndSequenceNumber();
            Record.Point       = FVector3(JoltUtils::FromJPHRVec3(inContactPosition));
            Record.Normal      = JoltUtils::FromJPHVec3(inContactNormal);
            Record.VelocityA   = FVector3(0.0f);
            Record.VelocityB   = FVector3(0.0f);
            Record.ImpactSpeed = 0.0f;
            Record.bSensorA    = false;
            Record.bSensorB    = false;
            Scene->EnqueueContactRecord(Record);
        }

    private:
        FJoltPhysicsScene*                  Scene = nullptr;
        const JPH::BodyLockInterfaceNoLock* BodyLockInterface = nullptr;
    };

    // Body sleep/wake listener. Jolt calls these during the step (any worker thread, body locked) with the
    // body's user data = entity id; we just stage the transition for the game-thread script dispatch.
    class FJoltBodyActivationListener : public JPH::BodyActivationListener
    {
    public:
        explicit FJoltBodyActivationListener(FJoltPhysicsScene* InScene) : Scene(InScene) { }

        virtual void OnBodyActivated(const JPH::BodyID& /*inBodyID*/, JPH::uint64 inBodyUserData) override
        {
            Scene->EnqueueActivation(static_cast<entt::entity>(static_cast<uint32>(inBodyUserData)), true);
        }

        virtual void OnBodyDeactivated(const JPH::BodyID& /*inBodyID*/, JPH::uint64 inBodyUserData) override
        {
            Scene->EnqueueActivation(static_cast<entt::entity>(static_cast<uint32>(inBodyUserData)), false);
        }

    private:
        FJoltPhysicsScene* Scene = nullptr;
    };

    FJoltPhysicsScene::FJoltPhysicsScene(CWorld* InWorld)
        : Allocator(32ull * 1024 * 1024)
        , World(InWorld)
    {
        JoltSystem = MakeUnique<JPH::PhysicsSystem>();

        const SDefaultWorldSettings& InitSettings = World->GetDefaultWorldSettings();
        
        JoltSystem->Init(InitSettings.MaxPhysicsBodies, 0, InitSettings.MaxPhysicsBodyPairs,
            InitSettings.MaxPhysicsContactConstraints, GJoltLayerInterface, GObjectVsBroadPhaseLayerFilter, GObjectVsObjectLayerFilter);

        // Sized once to MaxPhysicsBodies; JPH::BodyID::GetIndex() is bounded by this so the
        // contact-listener lookup is a bare array index with no resize race.
        BodyMaterials.resize(InitSettings.MaxPhysicsBodies);
        BodySurfaceVelocities.resize(InitSettings.MaxPhysicsBodies);

        FVector3 GravityDir = Math::LengthSquared(InitSettings.GravityDirection) > 0.0f
            ? Math::Normalize(InitSettings.GravityDirection) : FVector3(0.0f, -1.0f, 0.0f);
        JoltSystem->SetGravity(JoltUtils::ToJPHVec3(GravityDir * Math::Abs(GEarthGravity) * InitSettings.GravityScale));

        JPH::PhysicsSettings JoltSettings;
        JoltSettings.mDeterministicSimulation                   = InitSettings.bDeterministicSimulation;
        JoltSettings.mNumVelocitySteps                          = InitSettings.NumVelocitySteps;
        JoltSettings.mNumPositionSteps                          = InitSettings.NumPositionSteps;
        JoltSettings.mBaumgarte                                 = InitSettings.BaumgarteStabilizationFactor;
        JoltSettings.mSpeculativeContactDistance                = InitSettings.SpeculativeContactDistance;
        JoltSettings.mPenetrationSlop                           = InitSettings.PenetrationSlop;
        JoltSettings.mMaxPenetrationDistance                    = InitSettings.MaxPenetrationDistance;
        JoltSettings.mLinearCastThreshold                       = InitSettings.LinearCastThreshold;
        JoltSettings.mLinearCastMaxPenetration                  = InitSettings.LinearCastMaxPenetration;
        JoltSettings.mManifoldTolerance                         = InitSettings.ManifoldTolerance;
        JoltSettings.mMinVelocityForRestitution                 = InitSettings.MinVelocityForRestitution;
        JoltSettings.mTimeBeforeSleep                           = InitSettings.TimeBeforeSleep;
        JoltSettings.mPointVelocitySleepThreshold               = InitSettings.SleepVelocityThreshold;
        JoltSettings.mBodyPairCacheMaxDeltaPositionSq           = InitSettings.BodyPairCacheMaxDeltaPositionSq;
        JoltSettings.mBodyPairCacheCosMaxDeltaRotationDiv2      = InitSettings.BodyPairCacheCosMaxDeltaRotationDiv2;
        JoltSettings.mContactNormalCosMaxDeltaRotation          = InitSettings.ContactNormalCosMaxDeltaRotation;
        JoltSettings.mContactPointPreserveLambdaMaxDistSq       = InitSettings.ContactPointPreserveLambdaMaxDistSq;
        JoltSettings.mMaxInFlightBodyPairs                      = InitSettings.MaxInFlightBodyPairs;
        JoltSettings.mStepListenersBatchSize                    = InitSettings.StepListenersBatchSize;
        JoltSettings.mStepListenerBatchesPerJob                 = InitSettings.StepListenerBatchesPerJob;
        JoltSettings.mConstraintWarmStart                       = InitSettings.bConstraintWarmStart;
        JoltSettings.mUseBodyPairContactCache                   = InitSettings.bUseBodyPairContactCache;
        JoltSettings.mUseManifoldReduction                      = InitSettings.bUseManifoldReduction;
        JoltSettings.mUseLargeIslandSplitter                    = InitSettings.bUseLargeIslandSplitter;
        JoltSettings.mAllowSleeping                             = InitSettings.bAllowSleeping;
        JoltSettings.mCheckActiveEdges                          = InitSettings.bCheckActiveEdges;
        JoltSystem->SetPhysicsSettings(JoltSettings);
        
        entt::dispatcher& Dispatcher = World->GetSingleton<entt::dispatcher&>();
        ContactListener = MakeUnique<FJoltContactListener>(this, Dispatcher, &JoltSystem->GetBodyLockInterfaceNoLock());
        JoltSystem->SetContactListener(ContactListener.get());

        // Shared across every CharacterVirtual; assigned per-character in OnCharacterComponentConstructed.
        CharacterContactListener = MakeUnique<FJoltCharacterContactListener>(this, &JoltSystem->GetBodyLockInterfaceNoLock());

        // Sleep/wake transitions -> EntityScript OnWake/OnSleep (drained game-thread).
        ActivationListener = MakeUnique<FJoltBodyActivationListener>(this);
        JoltSystem->SetBodyActivationListener(ActivationListener.get());

        // Shared registry for character-vs-character (mutual pushing); characters opt in at construction.
        CharacterVsCharacter = MakeUnique<JPH::CharacterVsCharacterCollisionSimple>();
        
        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.on_construct<SSphereColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SBoxColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCapsuleColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCylinderColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCapsuleColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCylinderColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SPlaneColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCompoundColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SMeshColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STerrainColliderComponent>().connect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
    }

    FJoltPhysicsScene::~FJoltPhysicsScene()
    {
        // Remove joints before JoltSystem is torn down (constraints reference live bodies/the manager).
        DestroyAllConstraints();

        FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);

        Registry.on_construct<SSphereColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SBoxColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCapsuleColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCylinderColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCapsuleColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STaperedCylinderColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SPlaneColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SCompoundColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<SMeshColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
        Registry.on_construct<STerrainColliderComponent>().disconnect<&entt::registry::emplace_or_replace<SRigidBodyComponent>>();
    }

    void FJoltPhysicsScene::PreUpdate()
    {

    }

    void FJoltPhysicsScene::PostUpdate()
    {
    }

    void FJoltPhysicsScene::DispatchPendingEvents()
    {
        LUMINA_PROFILE_SCOPE();
        // Game thread, post-join: write the physics step's interpolated transforms
        // into the ECS before contact callbacks (scripts read fresh transforms).
        ApplyInterpolatedTransforms();
        DispatchContactEvents();
        DispatchActivationEvents();
    }

    // Remove the display-time override so the entity renders off its resolved world matrix again.
    // Erase only: this also runs from on_destroy hooks, where dirtying a dying entity would outlive it.
    static void DropRenderTransform(entt::registry& Registry, entt::entity Entity)
    {
        if (!Registry.valid(Entity))
        {
            return;
        }

        auto& Storage = Registry.storage<FRenderTransform>();
        if (Storage.contains(Entity))
        {
            Storage.erase(Entity);
        }
    }

    void FJoltPhysicsScene::ApplyDirtyTransforms(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        
        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        const JPH::BodyLockInterfaceNoLock& LockInterface = JoltSystem->GetBodyLockInterfaceNoLock();
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();

        
        auto BodySyncView = Registry.view<SRigidBodyComponent, FNeedsPhysicsBodyUpdate>();
        auto Handle = BodySyncView.handle();
        auto&& Storage = Registry.storage<STransformComponent>();
        Task::ParallelFor(Handle->size(), [&] (uint32 Index)
        {
            entt::entity Entity = (*Handle)[Index];
            if (!BodySyncView.contains(Entity))
            {
                return;
            }
         
            const auto& BodyComponent       = BodySyncView.get<SRigidBodyComponent>(Entity);
            const auto& TransformComponent  = Storage.get(Entity);
            const auto& Update              = BodySyncView.get<FNeedsPhysicsBodyUpdate>(Entity);

            JPH::BodyID BodyID = JPH::BodyID(BodyComponent.BodyID);
            
            const JPH::Body* Body = LockInterface.TryGetBody(BodyID);
            if (Body == nullptr)
            {
                return;
            }
            
            JPH::RVec3 Location = JoltUtils::ToJPHRVec3(TransformComponent.GetLocation());
            JPH::Quat  Rotation = JoltUtils::ToJPHQuat(TransformComponent.GetRotation());
            JPH::EActivation Activation = Update.bActivate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

            if (Body->IsStatic())
            {
                BodyInterface.SetPositionAndRotationWhenChanged(BodyID, Location, Rotation, Activation);
            }
            else if (Body->IsKinematic())
            {
                BodyInterface.MoveKinematic(BodyID, Location, Rotation, FixedDt);
            }
            else if (Body->IsDynamic())
            {
                switch (Update.MoveMode)
                {
                    case EMoveMode::Teleport:
                    {
                        BodyInterface.SetPositionAndRotation(BodyID, Location, Rotation, Activation);
                        break;
                    }
                    case EMoveMode::MoveKinematic:
                    {
                        JPH::RVec3 CurrentPos = Body->GetPosition();
                        JPH::Quat  CurrentRot = Body->GetRotation();

                        JPH::Vec3 LinearVel = (Location - CurrentPos) / FixedDt;
                        BodyInterface.SetLinearVelocity(BodyID, LinearVel);

                        JPH::Quat DeltaRot = Rotation * CurrentRot.Conjugated();
                        JPH::Vec3 Axis;
                        float Angle;
                        DeltaRot.GetAxisAngle(Axis, Angle);

                        if (Angle > JPH::JPH_PI)
                        {
                            Angle -= 2.0f * JPH::JPH_PI;
                        }

                        JPH::Vec3 AngularVel = Axis * (Angle / FixedDt);
                        BodyInterface.SetAngularVelocity(BodyID, AngularVel);

                        if (Update.bActivate)
                        {
                            BodyInterface.ActivateBody(BodyID);
                        }
                        break;
                    }
                    case EMoveMode::ActivateOnly:
                    {
                        BodyInterface.ActivateBody(BodyID);
                        break;
                    }
                }
            }
            
        });

        // Consume the requests. A blanket clear used to drop the ones this pass could not apply: an entity
        // whose body does not exist yet (creation deferred behind a still-loading collider asset) was skipped
        // above and then had its request wiped, so a spawn-then-SetLocation silently never reached the body.
        // Carry those forward instead, payload intact, and let the next step retry.
        //
        // Only entities in this view are candidates. A tag on an entity with no SRigidBodyComponent -- the
        // character path tags them too, but moves them through SCharacterMovementComponent::bPendingTeleport
        // instead -- is still consumed here, or it would pin that entity's transform writeback forever.
        RetryBodyUpdates.clear();
        for (auto [Entity, BodyComponent, Update] : BodySyncView.each())
        {
            if (JPH::BodyID(BodyComponent.BodyID).IsInvalid())
            {
                RetryBodyUpdates.push_back({ Entity, Update });
            }
        }

        Registry.clear<FNeedsPhysicsBodyUpdate>();

        for (const FDeferredBodyUpdate& Retry : RetryBodyUpdates)
        {
            if (Registry.valid(Retry.Entity))
            {
                Registry.emplace<FNeedsPhysicsBodyUpdate>(Retry.Entity, Retry.Update);
            }
        }
    }
    

    void FJoltPhysicsScene::Update(double DeltaTime)
    {
        LUMINA_PROFILE_SCOPE();

        // Mark this scene as stepping so on_construct defers body creation (Jolt forbids it mid-step).
        // Per-scene flag, so it survives the physics job migrating workers.
        bStepInProgress.store(true, std::memory_order_release);
        struct FStepGuard { TAtomic<bool>& F; ~FStepGuard() { F.store(false, std::memory_order_release); } } StepGuard{ bStepInProgress };

        // Drain pending body creations before the step. Snapshot the queue first: an entity that
        // still is not buildable re-pushes itself, so draining the live queue until empty spins forever.
        // Re-pushes and on_construct deferrals land in the now-empty live queue and retry next step.
        {
            TQueue<entt::entity> PendingThisStep;
            {
                FScopeLock Lock(PendingRigidBodyMutex);
                PendingThisStep.swap(PendingRigidBodyCreations);
            }

            while (!PendingThisStep.empty())
            {
                const entt::entity Entity = PendingThisStep.front();
                PendingThisStep.pop();

                if (World->IsValidEntity(Entity))
                {
                    CreateRigidBodyImmediate(ECS::GetWorldRegistry(*World), Entity);
                }
            }
        }

        // Build any component-authored joints whose bodies are now ready (pre-step, so locking is safe).
        DrainPendingConstraints();

        const SDefaultWorldSettings& WorldSettings = World->GetDefaultWorldSettings();

        // Re-apply global physics settings each frame so editor changes take effect immediately.
        {
            FVector3 GravityDir = Math::LengthSquared(WorldSettings.GravityDirection) > 0.0f
                ? Math::Normalize(WorldSettings.GravityDirection) : FVector3(0.0f, -1.0f, 0.0f);
            JoltSystem->SetGravity(JoltUtils::ToJPHVec3(GravityDir * Math::Abs(GEarthGravity) * WorldSettings.GravityScale));

            JPH::PhysicsSettings JoltSettings                           = JoltSystem->GetPhysicsSettings();
            JoltSettings.mDeterministicSimulation                       = WorldSettings.bDeterministicSimulation;
            JoltSettings.mNumVelocitySteps                              = WorldSettings.NumVelocitySteps;
            JoltSettings.mNumPositionSteps                              = WorldSettings.NumPositionSteps;
            JoltSettings.mBaumgarte                                     = WorldSettings.BaumgarteStabilizationFactor;
            JoltSettings.mSpeculativeContactDistance                    = WorldSettings.SpeculativeContactDistance;
            JoltSettings.mPenetrationSlop                               = WorldSettings.PenetrationSlop;
            JoltSettings.mMaxPenetrationDistance                        = WorldSettings.MaxPenetrationDistance;
            JoltSettings.mLinearCastThreshold                           = WorldSettings.LinearCastThreshold;
            JoltSettings.mLinearCastMaxPenetration                      = WorldSettings.LinearCastMaxPenetration;
            JoltSettings.mManifoldTolerance                             = WorldSettings.ManifoldTolerance;
            JoltSettings.mMinVelocityForRestitution                     = WorldSettings.MinVelocityForRestitution;
            JoltSettings.mTimeBeforeSleep                               = WorldSettings.TimeBeforeSleep;
            JoltSettings.mPointVelocitySleepThreshold                   = WorldSettings.SleepVelocityThreshold;
            JoltSettings.mBodyPairCacheMaxDeltaPositionSq               = WorldSettings.BodyPairCacheMaxDeltaPositionSq;
            JoltSettings.mBodyPairCacheCosMaxDeltaRotationDiv2          = WorldSettings.BodyPairCacheCosMaxDeltaRotationDiv2;
            JoltSettings.mContactNormalCosMaxDeltaRotation              = WorldSettings.ContactNormalCosMaxDeltaRotation;
            JoltSettings.mContactPointPreserveLambdaMaxDistSq           = WorldSettings.ContactPointPreserveLambdaMaxDistSq;
            JoltSettings.mMaxInFlightBodyPairs                          = WorldSettings.MaxInFlightBodyPairs;
            JoltSettings.mStepListenersBatchSize                        = WorldSettings.StepListenersBatchSize;
            JoltSettings.mStepListenerBatchesPerJob                     = WorldSettings.StepListenerBatchesPerJob;
            JoltSettings.mConstraintWarmStart                           = WorldSettings.bConstraintWarmStart;
            JoltSettings.mUseBodyPairContactCache                       = WorldSettings.bUseBodyPairContactCache;
            JoltSettings.mUseManifoldReduction                          = WorldSettings.bUseManifoldReduction;
            JoltSettings.mUseLargeIslandSplitter                        = WorldSettings.bUseLargeIslandSplitter;
            JoltSettings.mAllowSleeping                                 = WorldSettings.bAllowSleeping;
            JoltSettings.mCheckActiveEdges                              = WorldSettings.bCheckActiveEdges;
            JoltSystem->SetPhysicsSettings(JoltSettings);
        }

        const float PhysicsRateHz  = eastl::max(10.0f, WorldSettings.PhysicsHz);
        const float FixedTimestep  = 1.0f / PhysicsRateHz;
        const float MaxAccumulation = (float)WorldSettings.MaxPhysicsSteps * FixedTimestep;

        Accumulator += static_cast<float>(DeltaTime);

        // Clamp to prevent spiral-of-death on heavy frames.
        Accumulator = std::min(Accumulator, MaxAccumulation);

        if (Accumulator >= FixedTimestep)
        {
            // Cap at MaxPhysicsSteps to bound work per frame.
            CollisionSteps = Math::Min((uint32)WorldSettings.MaxPhysicsSteps, (uint32)(Accumulator / FixedTimestep));
        }
        else
        {
            CollisionSteps = 0;
        }

        #if JPH_DEBUG_RENDERER
        if (FConsoleRegistry::Get().GetAs<bool>("Jolt.Debug.Draw"))
        {
            FJoltDebugRenderer* DebugRenderer = FJoltPhysicsContext::GetDebugRenderer();
            DebugRenderer->DrawBodies(JoltSystem.get(), World);
        }
        #endif

        if (CollisionSteps > 0)
        {
            // Apply game-thread transform writes once before stepping; this also
            // clears the FNeedsPhysicsBodyUpdate flags for the frame.
            ApplyDirtyTransforms(FixedTimestep);
            LatchCharacterInput();
            
            for (uint32 Step = 0; Step < CollisionSteps; ++Step)
            {
                if (Step == CollisionSteps - 1)
                {
                    SnapshotBodyStates();
                }

                CheckJoltError(JoltSystem->Update(FixedTimestep, 1, &Allocator, FJoltPhysicsContext::GetThreadPool()));
                UpdateCharacters(FixedTimestep);
            }

            // Disable any breakable joint that exceeded its force threshold this frame (reads fresh lambdas).
            MonitorBreakableConstraints(FixedTimestep);

            // Contact records drained on game thread via DispatchPendingEvents.

            Accumulator -= (float)CollisionSteps * FixedTimestep;
        }
        
        const float InterpolationAlpha = WorldSettings.bEnablePhysicsInterpolation
            ? Math::Clamp(Accumulator / FixedTimestep, 0.0f, 1.0f)
            : 1.0f;
        
        BuildInterpolatedTransforms(InterpolationAlpha);

        #if JPH_DEBUG_RENDERER
        FJoltPhysicsContext::GetDebugRenderer()->NextFrame();
        #endif
    }
    
    void FJoltPhysicsScene::Simulate()
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        
        BulkCreateRigidBodies(Registry);

        auto CharacterView = Registry.view<SCharacterPhysicsComponent>();

        CharacterView.each([&] (entt::entity EntityID, SCharacterPhysicsComponent&)
        {
            OnCharacterComponentConstructed(Registry, EntityID);
        });

        // Seed the has-body hint for bodies that existed at play start (BulkCreateRigidBodies bypasses the
        // on_construct hook). MarkDirty uses it to skip the DirtyBodies queue for bodiless entities.
        Registry.view<SRigidBodyComponent, STransformComponent>().each([](SRigidBodyComponent&, STransformComponent& T) { T.SetHasPhysicsBody(true); });

        JoltSystem->OptimizeBroadPhase();
        
        Registry.on_construct<SCharacterPhysicsComponent>().connect<&FJoltPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.on_destroy<SCharacterPhysicsComponent>().connect<&FJoltPhysicsScene::OnCharacterComponentDestroyed>(this);

        Registry.on_update<SRigidBodyComponent>().connect<&FJoltPhysicsScene::OnRigidBodyComponentUpdated>(this);
        Registry.on_construct<SRigidBodyComponent>().connect<&FJoltPhysicsScene::OnRigidBodyComponentConstructed>(this);
        Registry.on_destroy<SRigidBodyComponent>().connect<&FJoltPhysicsScene::OnRigidBodyComponentDestroyed>(this);

        Registry.on_construct<SPhysicsConstraintComponent>().connect<&FJoltPhysicsScene::OnConstraintComponentConstructed>(this);
        Registry.on_destroy<SPhysicsConstraintComponent>().connect<&FJoltPhysicsScene::OnConstraintComponentDestroyed>(this);

        // Queue joints that already exist on entities at play start (on_construct won't fire retroactively).
        Registry.view<SPhysicsConstraintComponent>().each([&](entt::entity E, SPhysicsConstraintComponent& C)
        {
            C.ConstraintID = 0;
            PendingConstraintCreations.push_back(E);
        });

        entt::dispatcher& Dispatcher = World->GetSingleton<entt::dispatcher&>();
        Dispatcher.sink<SImpulseEvent>().connect<&FJoltPhysicsScene::OnImpulseEvent>(this);
        Dispatcher.sink<SForceEvent>().connect<&FJoltPhysicsScene::OnForceEvent>(this);
        Dispatcher.sink<STorqueEvent>().connect<&FJoltPhysicsScene::OnTorqueEvent>(this);
        Dispatcher.sink<SAngularImpulseEvent>().connect<&FJoltPhysicsScene::OnAngularImpulseEvent>(this);
        Dispatcher.sink<SSetVelocityEvent>().connect<&FJoltPhysicsScene::OnSetVelocityEvent>(this);
        Dispatcher.sink<SSetAngularVelocityEvent>().connect<&FJoltPhysicsScene::OnSetAngularVelocityEvent>(this);
        Dispatcher.sink<SAddImpulseAtPositionEvent>().connect<&FJoltPhysicsScene::OnAddImpulseAtPositionEvent>(this);
        Dispatcher.sink<SAddForceAtPositionEvent>().connect<&FJoltPhysicsScene::OnAddForceAtPositionEvent>(this);
        Dispatcher.sink<SSetGravityFactorEvent>().connect<&FJoltPhysicsScene::OnSetGravityFactorEvent>(this);
    }

    void FJoltPhysicsScene::StopSimulate()
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        Registry.on_construct<SCharacterPhysicsComponent>().disconnect<&FJoltPhysicsScene::OnCharacterComponentConstructed>(this);
        Registry.on_destroy<SCharacterPhysicsComponent>().disconnect<&FJoltPhysicsScene::OnCharacterComponentDestroyed>(this);

        Registry.on_construct<SRigidBodyComponent>().disconnect<&FJoltPhysicsScene::OnRigidBodyComponentConstructed>(this);
        Registry.on_destroy<SRigidBodyComponent>().disconnect<&FJoltPhysicsScene::OnRigidBodyComponentDestroyed>(this);

        Registry.on_construct<SPhysicsConstraintComponent>().disconnect<&FJoltPhysicsScene::OnConstraintComponentConstructed>(this);
        Registry.on_destroy<SPhysicsConstraintComponent>().disconnect<&FJoltPhysicsScene::OnConstraintComponentDestroyed>(this);

        entt::dispatcher& Dispatcher = World->GetSingleton<entt::dispatcher&>();
        Dispatcher.sink<SImpulseEvent>().disconnect<&FJoltPhysicsScene::OnImpulseEvent>(this);
        Dispatcher.sink<SForceEvent>().disconnect<&FJoltPhysicsScene::OnForceEvent>(this);
        Dispatcher.sink<STorqueEvent>().disconnect<&FJoltPhysicsScene::OnTorqueEvent>(this);
        Dispatcher.sink<SAngularImpulseEvent>().disconnect<&FJoltPhysicsScene::OnAngularImpulseEvent>(this);
        Dispatcher.sink<SSetVelocityEvent>().disconnect<&FJoltPhysicsScene::OnSetVelocityEvent>(this);
        Dispatcher.sink<SSetAngularVelocityEvent>().disconnect<&FJoltPhysicsScene::OnSetAngularVelocityEvent>(this);
        Dispatcher.sink<SAddImpulseAtPositionEvent>().disconnect<&FJoltPhysicsScene::OnAddImpulseAtPositionEvent>(this);
        Dispatcher.sink<SAddForceAtPositionEvent>().disconnect<&FJoltPhysicsScene::OnAddForceAtPositionEvent>(this);
        Dispatcher.sink<SSetGravityFactorEvent>().disconnect<&FJoltPhysicsScene::OnSetGravityFactorEvent>(this);

        auto View = Registry.view<SRigidBodyComponent>();
        View.each([&] (entt::entity EntityID, SRigidBodyComponent&)
        {
            OnRigidBodyComponentDestroyed(Registry, EntityID); 
        });
    }

    void FJoltPhysicsScene::ActivateBody(uint32 BodyID)
    {
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        BodyInterface.ActivateBody(JPH::BodyID(BodyID));
    }

    bool FJoltPhysicsScene::IsBodyActive(uint32 BodyID)
    {
        return JoltSystem->GetBodyInterface().IsActive(JPH::BodyID(BodyID));
    }

    void FJoltPhysicsScene::DeactivateBody(uint32 BodyID)
    {
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        BodyInterface.DeactivateBody(JPH::BodyID(BodyID));
    }

    void FJoltPhysicsScene::ChangeBodyMotionType(uint32 BodyID, EBodyType NewType)
    {
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        BodyInterface.SetMotionType(JPH::BodyID(BodyID), JoltUtils::ToJoltMotionType(NewType), JPH::EActivation::Activate);
    }

    void FJoltPhysicsScene::SnapshotBodyStates()
    {
        LUMINA_PROFILE_SCOPE();

        const JPH::BodyLockInterfaceNoLock& LockInterface = JoltSystem->GetBodyLockInterfaceNoLock();
        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        
        auto RigidView = Registry.view<SRigidBodyComponent>();
        auto RigidHandle = RigidView.handle();
        Task::ParallelFor(RigidHandle->size(), [&](uint32 Index)
        {
            entt::entity Entity = (*RigidHandle)[Index];

            // Reflected components are in_place_delete: the packed array has tombstone holes that raw
            // indexing/get() walks out of bounds. each() skips them, manual indexing must too. Do NOT remove.
            if (Entity == entt::tombstone)
            {
                return;
            }

            SRigidBodyComponent& BodyComponent = RigidView.get<SRigidBodyComponent>(Entity);

            const JPH::Body* Body = LockInterface.TryGetBody(JPH::BodyID(BodyComponent.BodyID));
            if (Body == nullptr || Body->IsStatic())
            {
                return;
            }

            BodyComponent.LastBodyPosition = JoltUtils::FromJPHVec3(Body->GetPosition());
            BodyComponent.LastBodyRotation = JoltUtils::FromJPHQuat(Body->GetRotation());
        });

        auto CharView = Registry.view<SCharacterPhysicsComponent>();
        CharView.each([&](SCharacterPhysicsComponent& CharComponent)
        {
            if (CharComponent.Character == nullptr)
            {
                return;
            }

            CharComponent.LastBodyPosition = JoltUtils::FromJPHVec3(CharComponent.Character->Ref->GetPosition());
            CharComponent.LastBodyRotation = JoltUtils::FromJPHQuat(CharComponent.Character->Ref->GetRotation());
        });
    }

    // In-place SoA quaternion nlerp toward Prev along the shortest arc, 8 quats per iteration (Curr* in/out).
    // nlerp not slerp: per-frame alpha is tiny so the difference is invisible, and it drops the per-body acos/sin.
    static void NlerpQuatsSoA(float* Qx, float* Qy, float* Qz, float* Qw,
                              const float* Px, const float* Py, const float* Pz, const float* Pw,
                              uint32 Count, float Alpha)
    {
        using namespace SIMD;
        const VFloat8 A         = VFloat8::Broadcast(Alpha);
        const VFloat8 OneMinusA = VFloat8::Broadcast(1.0f - Alpha);
        const VFloat8 VZero     = VFloat8::Zero();

        uint32 i = 0;
        for (; i + 8 <= Count; i += 8)
        {
            const VFloat8 px = VFloat8::Load(Px + i), py = VFloat8::Load(Py + i),
                          pz = VFloat8::Load(Pz + i), pw = VFloat8::Load(Pw + i);
            VFloat8 cx = VFloat8::Load(Qx + i), cy = VFloat8::Load(Qy + i),
                    cz = VFloat8::Load(Qz + i), cw = VFloat8::Load(Qw + i);

            const VFloat8 Dot  = MulAdd(px, cx, MulAdd(py, cy, MulAdd(pz, cz, pw * cw)));
            const VFloat8 Flip = CmpLt(Dot, VZero);
            cx = Select(Flip, -cx, cx); cy = Select(Flip, -cy, cy);
            cz = Select(Flip, -cz, cz); cw = Select(Flip, -cw, cw);

            VFloat8 ox = MulAdd(px, OneMinusA, cx * A);
            VFloat8 oy = MulAdd(py, OneMinusA, cy * A);
            VFloat8 oz = MulAdd(pz, OneMinusA, cz * A);
            VFloat8 ow = MulAdd(pw, OneMinusA, cw * A);

            const VFloat8 Inv = InvSqrt(MulAdd(ox, ox, MulAdd(oy, oy, MulAdd(oz, oz, ow * ow))));
            (ox * Inv).Store(Qx + i); (oy * Inv).Store(Qy + i);
            (oz * Inv).Store(Qz + i); (ow * Inv).Store(Qw + i);
        }

        const float OneMinus = 1.0f - Alpha;
        for (; i < Count; ++i)
        {
            const float px = Px[i], py = Py[i], pz = Pz[i], pw = Pw[i];
            float cx = Qx[i], cy = Qy[i], cz = Qz[i], cw = Qw[i];
            if (px * cx + py * cy + pz * cz + pw * cw < 0.0f) { cx = -cx; cy = -cy; cz = -cz; cw = -cw; }

            const float ox = px * OneMinus + cx * Alpha, oy = py * OneMinus + cy * Alpha,
                        oz = pz * OneMinus + cz * Alpha, ow = pw * OneMinus + cw * Alpha;
            const float Inv = 1.0f / std::sqrt(ox * ox + oy * oy + oz * oz + ow * ow);
            Qx[i] = ox * Inv; Qy[i] = oy * Inv; Qz[i] = oz * Inv; Qw[i] = ow * Inv;
        }
    }

    void FJoltPhysicsScene::BuildInterpolatedTransforms(float Alpha)
    {
        LUMINA_PROFILE_SCOPE();

        const float KillHeight = World->GetDefaultWorldSettings().WorldKillHeight;
        const JPH::BodyLockInterfaceNoLock& LockInterface = JoltSystem->GetBodyLockInterfaceNoLock();
        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        auto RigidView   = Registry.view<SRigidBodyComponent>();
        auto RigidHandle = RigidView.handle();
        const uint32 RigidCount = (uint32)RigidHandle->size();

        auto CharView = Registry.view<SCharacterPhysicsComponent>();
        const uint32 CharCount = (uint32)CharView.handle()->size();

        const uint32 Total = RigidCount + CharCount;
        InterpStaging.Resize(Total);

        // Identity-fill a skipped/killed slot so the bulk interp never sees NaNs
        // (physics builds enable FP exceptions).
        auto WriteIdentity = [&](uint32 i)
        {
            InterpStaging.PrevPos[i] = FVector3(0.0f);
            InterpStaging.CurrPos[i] = FVector3(0.0f);
            InterpStaging.PrevQx[i] = InterpStaging.CurrQx[i] = 0.0f;
            InterpStaging.PrevQy[i] = InterpStaging.CurrQy[i] = 0.0f;
            InterpStaging.PrevQz[i] = InterpStaging.CurrQz[i] = 0.0f;
            InterpStaging.PrevQw[i] = InterpStaging.CurrQw[i] = 1.0f;
        };

        auto StoreState = [&](uint32 i, const FVector3& Prev, const FVector3& Curr, const FQuat& PrevR, const FQuat& CurrR)
        {
            InterpStaging.Flags[i]   = EInterpFlag::Interpolate;
            InterpStaging.PrevPos[i] = Prev;   InterpStaging.CurrPos[i] = Curr;
            InterpStaging.PrevQx[i] = PrevR.x; InterpStaging.PrevQy[i] = PrevR.y; InterpStaging.PrevQz[i] = PrevR.z; InterpStaging.PrevQw[i] = PrevR.w;
            InterpStaging.CurrQx[i] = CurrR.x; InterpStaging.CurrQy[i] = CurrR.y; InterpStaging.CurrQz[i] = CurrR.z; InterpStaging.CurrQw[i] = CurrR.w;
        };

        // Gather prev (component snapshot) + curr (Jolt) into SoA; no interp math, this pass is body-lookup bound.
        Task::ParallelFor(RigidCount, [&](uint32 i)
        {
            const entt::entity Entity = (*RigidHandle)[i];
            InterpStaging.Entities[i] = Entity;

            // In_place_delete leaves tombstone holes in the packed array (see SnapshotBodyStates);
            // skip them, identity-filling the slot so the bulk SIMD pass never sees NaNs.
            if (Entity == entt::tombstone)
            {
                InterpStaging.Flags[i] = EInterpFlag::Skip;
                WriteIdentity(i);
                return;
            }

            const SRigidBodyComponent& BodyComponent = RigidView.get<SRigidBodyComponent>(Entity);
            const JPH::Body* Body = LockInterface.TryGetBody(JPH::BodyID(BodyComponent.BodyID));
            if (Body == nullptr || Body->IsStatic())
            {
                InterpStaging.Flags[i] = EInterpFlag::Skip;
                WriteIdentity(i);
                return;
            }

            const JPH::RVec3 CurrPos = Body->GetPosition();
            if (CurrPos.GetY() < KillHeight)
            {
                InterpStaging.Flags[i] = EInterpFlag::BelowKill;
                WriteIdentity(i);
                return;
            }

            StoreState(i, BodyComponent.LastBodyPosition, JoltUtils::FromJPHVec3(CurrPos),
                          BodyComponent.LastBodyRotation, JoltUtils::FromJPHQuat(Body->GetRotation()));
        });

        // Characters are few -- gather them serially into the tail of the batch.
        uint32 ci = RigidCount;
        CharView.each([&](entt::entity Entity, SCharacterPhysicsComponent& CharacterComponent)
        {
            const uint32 i = ci++;
            InterpStaging.Entities[i] = Entity;

            if (CharacterComponent.Character == nullptr)
            {
                InterpStaging.Flags[i] = EInterpFlag::Skip;
                WriteIdentity(i);
                return;
            }

            const JPH::RVec3 CurrPos = CharacterComponent.Character->Ref->GetPosition();
            if (CurrPos.GetY() < KillHeight)
            {
                InterpStaging.Flags[i] = EInterpFlag::BelowKill;
                WriteIdentity(i);
                return;
            }

            StoreState(i, CharacterComponent.LastBodyPosition, JoltUtils::FromJPHVec3(CurrPos),
                          CharacterComponent.LastBodyRotation, JoltUtils::FromJPHQuat(CharacterComponent.Character->Ref->GetRotation()));
        });

        // Interp: one SIMD pass over the whole batch -- position lerp + quat nlerp, out-of-place into Lerp*.
        // Curr* must survive as the simulated pose; it is what gets written back to the ECS.
        if (Total > 0)
        {
            SIMD::LerpArray(reinterpret_cast<float*>(InterpStaging.LerpPos.data()),
                            reinterpret_cast<const float*>(InterpStaging.PrevPos.data()),
                            reinterpret_cast<const float*>(InterpStaging.CurrPos.data()),
                            int(Total) * 3, Alpha);

            eastl::copy(InterpStaging.CurrQx.begin(), InterpStaging.CurrQx.end(), InterpStaging.LerpQx.begin());
            eastl::copy(InterpStaging.CurrQy.begin(), InterpStaging.CurrQy.end(), InterpStaging.LerpQy.begin());
            eastl::copy(InterpStaging.CurrQz.begin(), InterpStaging.CurrQz.end(), InterpStaging.LerpQz.begin());
            eastl::copy(InterpStaging.CurrQw.begin(), InterpStaging.CurrQw.end(), InterpStaging.LerpQw.begin());

            NlerpQuatsSoA(InterpStaging.LerpQx.data(), InterpStaging.LerpQy.data(),
                          InterpStaging.LerpQz.data(), InterpStaging.LerpQw.data(),
                          InterpStaging.PrevQx.data(), InterpStaging.PrevQy.data(),
                          InterpStaging.PrevQz.data(), InterpStaging.PrevQw.data(),
                          Total, Alpha);
        }
    }

    void FJoltPhysicsScene::ApplyInterpolatedTransforms()
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)InterpStaging.Entities.size();
        if (Count == 0)
        {
            return;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        auto& TransformStorage = Registry.storage<STransformComponent>();
        auto& RenderStorage    = Registry.storage<FRenderTransform>();

        ECS::Utils::FlushDirtyPhysicsBodies(Registry);

        const auto& PendingTeleport = Registry.storage<FNeedsPhysicsBodyUpdate>();

        // Slots written this pass; the render matrix needs the resolved world scale, so it is built after
        // the resolve below rather than inline.
        InterpApplied.clear();
        InterpApplied.reserve(Count);

        for (uint32 i = 0; i < Count; ++i)
        {
            const EInterpFlag Flag    = InterpStaging.Flags[i];
            const entt::entity Entity = InterpStaging.Entities[i];

            if (Flag == EInterpFlag::Skip || !Registry.valid(Entity))
            {
                // Body went static or vanished: drop the override and republish so the primitive picks the
                // resolved matrix back up instead of freezing on the last blend.
                if (Registry.valid(Entity) && RenderStorage.contains(Entity))
                {
                    RenderStorage.erase(Entity);
                    ECS::Utils::MarkTransformDirtyNoBody(Registry, Entity);
                }
                continue;
            }

            if (Flag == EInterpFlag::BelowKill)
            {
                Registry.destroy(Entity);
                continue;
            }

            if (!TransformStorage.contains(Entity))
            {
                continue;
            }

            // An authored move that hasn't reached the body yet outranks the body's own pose; drop the render
            // override too so the visual shows the authored target instead of a stale blend.
            if (PendingTeleport.contains(Entity))
            {
                if (RenderStorage.contains(Entity))
                {
                    RenderStorage.erase(Entity);
                }
                continue;
            }

            // The SIMULATED pose, not the blend: this is the value gameplay reads and the body re-sync writes
            // back, so the round trip is a no-op instead of dragging the body onto a display-time pose.
            STransformComponent& TransformComponent = TransformStorage.get(Entity);
            TransformComponent.SetRaw(InterpStaging.CurrPos[i],
                FQuat(InterpStaging.CurrQw[i], InterpStaging.CurrQx[i], InterpStaging.CurrQy[i], InterpStaging.CurrQz[i]));

            // Unconditional: on a frame with no fixed step the simulated pose is unchanged but the blend still
            // advanced, and the resolve's moved-publish is what refreshes the render primitive.
            ECS::Utils::MarkTransformDirtyNoBody(Registry, Entity);

            InterpApplied.push_back(i);
        }

        ECS::Utils::ResolveAllDirtyTransforms(Registry);

        // Render pose = blended location/rotation on the resolved world scale.
        for (uint32 i : InterpApplied)
        {
            const entt::entity Entity = InterpStaging.Entities[i];

            FTransform RenderPose = TransformStorage.get(Entity).WorldTransform;
            RenderPose.SetLocation(InterpStaging.LerpPos[i]);
            RenderPose.SetRotation(FQuat(InterpStaging.LerpQw[i], InterpStaging.LerpQx[i],
                                         InterpStaging.LerpQy[i], InterpStaging.LerpQz[i]));

            if (RenderStorage.contains(Entity))
            {
                RenderStorage.get(Entity).Matrix = RenderPose.GetMatrix();
            }
            else
            {
                RenderStorage.emplace(Entity, FRenderTransform{ RenderPose.GetMatrix() });
            }
        }
    }

    void FJoltPhysicsScene::LatchCharacterInput()
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        auto View = Registry.view<SCharacterControllerComponent, SCharacterMovementComponent>();

        View.each([&](SCharacterControllerComponent& Controller, SCharacterMovementComponent& Movement)
        {
            if (Math::LengthSquared(Controller.MoveInput) > LE_SMALL_NUMBER)
            {
                FVector3 Forward = RenderUtils::GetForwardVector(Controller.LookInput.x, 0.0f);
                FVector3 Right   = RenderUtils::GetRightVector(Controller.LookInput.x);
                FVector3 Up      = Math::Cross(Right, Forward);

                FVector3 Direction = Right * Controller.MoveInput.x + Up * Controller.MoveInput.y + Forward * Controller.MoveInput.z;
                const float Magnitude = Math::Length(Direction);
                if (Magnitude > LE_SMALL_NUMBER)
                {
                    Movement.PendingMoveDirection = Direction / Magnitude;
                    Movement.PendingMoveThrottle  = Math::Min(Magnitude, 1.0f);
                    Movement.bHasPendingMoveInput = true;
                }
                else
                {
                    Movement.PendingMoveDirection = FVector3(0.0f);
                    Movement.bHasPendingMoveInput = false;
                }
            }
            else
            {
                Movement.PendingMoveDirection = FVector3(0.0f);
                Movement.bHasPendingMoveInput = false;
            }

            Movement.PendingLookYaw = Controller.LookInput.x;
            Controller.MoveInput    = {};

            if (Controller.bJumpPressed)
            {
                Movement.bPendingJump    = true;
                Controller.bJumpPressed  = false;
            }

            if (Controller.bLaunchRequested)
            {
                Controller.bLaunchRequested        = false;
                Movement.PendingLaunchVelocity     = Controller.PendingLaunchVelocity;
                Movement.bLaunchOverrideHorizontal = Controller.bLaunchOverrideHorizontal;
                Movement.bLaunchOverrideVertical   = Controller.bLaunchOverrideVertical;
                Movement.bPendingLaunch            = true;
            }

            if (Controller.bTeleportRequested)
            {
                Controller.bTeleportRequested    = false;
                Movement.PendingTeleportLocation = Controller.PendingTeleportLocation;
                Movement.bPendingTeleport        = true;
            }
        });
    }

    void FJoltPhysicsScene::UpdateCharacters(float FixedDt)
    {
        LUMINA_PROFILE_SCOPE();

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        JPH::PhysicsSystem* PhysicsSystem = JoltSystem.get();

        auto View = Registry.view<SCharacterPhysicsComponent, SCharacterMovementComponent>();
        auto Handle = View.handle();
        const uint32 Count = (uint32)Handle->size();
        if (Count == 0)
        {
            return;
        }
        
        constexpr uint32 ParallelMinCount = 8;

        auto SubStep = [&](entt::entity Entity, JPH::TempAllocator& InAllocator)
        {
            LUMINA_PROFILE_SECTION("Character Sub-Step");

            if (!View.contains(Entity))
            {
                return;
            }

            SCharacterPhysicsComponent&  Physics  = View.get<SCharacterPhysicsComponent>(Entity);
            SCharacterMovementComponent& Movement = View.get<SCharacterMovementComponent>(Entity);

            JPH::CharacterVirtual* Character = Physics.Character ? Physics.Character->Ref.GetPtr() : nullptr;
            if (Character == nullptr)
            {
                return;
            }

            // Teleport before integration: move capsule, zero velocity, reseed interp snapshot so the render
            // transform doesn't streak across the jump.
            if (Movement.bPendingTeleport)
            {
                Movement.bPendingTeleport = false;

                const FVector3 TeleportLocation = Movement.PendingTeleportLocation;
                Character->SetPosition(JoltUtils::ToJPHRVec3(TeleportLocation));
                Character->SetLinearVelocity(JPH::Vec3::sZero());

                // Re-query contacts at the new spot; SetPosition alone leaves stale contacts, so the character
                // can spawn stuck-in-air or fall through the floor until its next move.
                const JPH::ObjectLayer TeleportLayer = JoltUtils::PackToObjectLayer(Physics.CollisionProfile);
                Character->RefreshContacts(
                    PhysicsSystem->GetDefaultBroadPhaseLayerFilter(TeleportLayer),
                    PhysicsSystem->GetDefaultLayerFilter(TeleportLayer),
                    {}, {}, InAllocator);

                Movement.Velocity        = FVector3(0.0f);
                Movement.bGrounded       = false;
                Movement.GroundEntity    = 0xFFFFFFFFu;
                Movement.GroundNormal    = FVector3(0.0f, 1.0f, 0.0f);
                Physics.LastBodyPosition = TeleportLocation;
                return;
            }

            const bool bHasMovementInput = Movement.bHasPendingMoveInput;
            const FVector3 DesiredDirection = Movement.PendingMoveDirection;

            const float    TargetSpeed    = bHasMovementInput ? Movement.MoveSpeed * Movement.PendingMoveThrottle : 0.0f;
            const FVector3 TargetVelocity = DesiredDirection * TargetSpeed;

            JPH::CharacterVirtual::ExtendedUpdateSettings UpdateSettings;
            UpdateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.5f, 0.0f);
            UpdateSettings.mWalkStairsStepUp     = JPH::Vec3(0.0f, Physics.StepHeight, 0.0f);

            const JPH::CharacterVirtual::EGroundState GroundState = Character->GetGroundState();
            const bool bWasGrounded = Movement.bGrounded;
            Movement.bGrounded = (GroundState == JPH::CharacterVirtual::EGroundState::OnGround);

            if (!bWasGrounded && Movement.bGrounded)
            {
                Movement.JumpCount = 0;
            }

            // Surface the ground we're standing on: normal for slope handling, entity for footstep-surface
            // lookups and moving-platform logic. GetGroundUserData carries the ground body's entity id.
            if (Movement.bGrounded)
            {
                Movement.GroundNormal = JoltUtils::FromJPHVec3(Character->GetGroundNormal());
                Movement.GroundEntity = static_cast<uint32>(Character->GetGroundUserData());
            }
            else
            {
                Movement.GroundNormal = FVector3(0.0f, 1.0f, 0.0f);
                Movement.GroundEntity = 0xFFFFFFFFu;
            }

            FQuat TargetRotation = JoltUtils::FromJPHQuat(Character->GetRotation());
            if (Movement.bUseControllerRotation)
            {
                TargetRotation = FQuat(FVector3(0.0f, Math::Radians(Movement.PendingLookYaw), 0.0f));
            }
            else if (Movement.bOrientRotationToMovement && bHasMovementInput)
            {
                float TargetYaw    = Math::Atan2(DesiredDirection.x, DesiredDirection.z);
                FQuat Rotation = FQuat(FVector3(0.0f, TargetYaw, 0.0f));
                TargetRotation     = Math::Slerp(TargetRotation, Rotation, Math::Clamp(Movement.RotationRate * FixedDt, 0.0f, 1.0f));
            }

            // Horizontal velocity integration with accel / friction / air drag.
            FVector3 HorizontalVelocity(Movement.Velocity.x, 0.0f, Movement.Velocity.z);
            const float CurrentSpeed = Math::Length(HorizontalVelocity);

            if (bHasMovementInput)
            {
                // Airborne steering is scaled by AirControl (0 = no air steering).
                const float Accel = Movement.bGrounded ? Movement.Acceleration
                                                       : Movement.Acceleration * Movement.AirControl;
                const float Blend = Math::Clamp(Accel * FixedDt, 0.0f, 1.0f);
                HorizontalVelocity = Math::Mix(HorizontalVelocity, TargetVelocity, Blend);
            }
            else if (Movement.bGrounded)
            {
                const float DecelerationAmount = Movement.Deceleration * FixedDt;
                const float NewSpeed           = Math::Max(0.0f, CurrentSpeed - DecelerationAmount);

                if (CurrentSpeed > 0.001f)
                {
                    HorizontalVelocity = Math::Normalize(HorizontalVelocity) * NewSpeed;
                }
                else
                {
                    HorizontalVelocity = FVector3(0.0f);
                }

                const float Friction = Math::Max(0.0f, 1.0f - Movement.GroundFriction * FixedDt);
                HorizontalVelocity *= Friction;
            }
            else
            {
                const float AirFriction = Math::Max(0.0f, 1.0f - (Movement.GroundFriction * 0.1f) * FixedDt);
                HorizontalVelocity *= AirFriction;
            }

            Movement.Velocity.x = HorizontalVelocity.x;
            Movement.Velocity.z = HorizontalVelocity.z;

            if (Movement.bGrounded)
            {
                const JPH::Vec3 GroundVelocity = Character->GetGroundVelocity();
                Movement.Velocity.x += GroundVelocity.GetX();
                Movement.Velocity.y  = GroundVelocity.GetY();
                Movement.Velocity.z += GroundVelocity.GetZ();
            }
            else
            {
                Movement.Velocity.y += Movement.Gravity * FixedDt;
            }

            if (Movement.bPendingJump)
            {
                Movement.bPendingJump = false;

                if (Movement.JumpCount < Movement.MaxJumpCount)
                {
                    Movement.Velocity.y = Movement.JumpSpeed;
                    Movement.JumpCount++;
                }
            }

            // Launch applied after the ground-velocity block and jump so an
            // upward impulse survives even when the character is still grounded.
            if (Movement.bPendingLaunch)
            {
                Movement.bPendingLaunch = false;

                if (Movement.bLaunchOverrideHorizontal)
                {
                    Movement.Velocity.x = Movement.PendingLaunchVelocity.x;
                    Movement.Velocity.z = Movement.PendingLaunchVelocity.z;
                }
                else
                {
                    Movement.Velocity.x += Movement.PendingLaunchVelocity.x;
                    Movement.Velocity.z += Movement.PendingLaunchVelocity.z;
                }

                if (Movement.bLaunchOverrideVertical)
                {
                    Movement.Velocity.y = Movement.PendingLaunchVelocity.y;
                }
                else
                {
                    Movement.Velocity.y += Movement.PendingLaunchVelocity.y;
                }
            }

            Character->SetRotation(JoltUtils::ToJPHQuat(TargetRotation));
            Character->SetLinearVelocity(JoltUtils::ToJPHVec3(Movement.Velocity));

            const JPH::ObjectLayer Layer = JoltUtils::PackToObjectLayer(Physics.CollisionProfile);

            // Char-vs-char characters skip all proxy inner bodies during movement (their mutual collision
            // runs through CharacterVsCharacterCollision instead); others use the default filter so they
            // still block via inner bodies.
            FCharacterProxyFilter ProxyFilter{CharacterProxyBodies};
            JPH::BodyFilter       DefaultBodyFilter;
            const JPH::BodyFilter& MovementBodyFilter = Physics.bCollideWithCharacters
                ? static_cast<const JPH::BodyFilter&>(ProxyFilter)
                : DefaultBodyFilter;

            Character->ExtendedUpdate(
                FixedDt,
                JPH::Vec3(0.0f, Movement.Gravity, 0.0f),
                UpdateSettings,
                PhysicsSystem->GetDefaultBroadPhaseLayerFilter(Layer),
                PhysicsSystem->GetDefaultLayerFilter(Layer),
                MovementBodyFilter,
                {},
                InAllocator);

            Movement.Velocity = JoltUtils::FromJPHVec3(Character->GetLinearVelocity());
        };

        // CharacterVsCharacterCollisionSimple is brute-force and NOT thread-safe (it reads every other
        // character's transform), so fall back to serial updates whenever any character opted into it.
        const bool bCharacterVsCharacterActive = !CharacterVsCharacter->mCharacters.empty();

        if (Count < ParallelMinCount || CharacterAllocators.empty() || bCharacterVsCharacterActive)
        {
            for (uint32 i = 0; i < Count; ++i)
            {
                SubStep((*Handle)[i], Allocator);
            }
            return;
        }

        const uint32 NumWorkers = (uint32)CharacterAllocators.size();
        Task::ParallelFor(Count, [&](uint32 Index, uint32 ThreadNum)
        {
            const uint32 AllocIdx = ThreadNum < NumWorkers ? ThreadNum : 0u;
            SubStep((*Handle)[Index], *CharacterAllocators[AllocIdx]);
        });
    }

    uint32 FJoltPhysicsScene::GetEntityBodyID(entt::entity Entity)
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        if (auto* RigidBody = Registry.try_get<SRigidBodyComponent>(Entity))
        {
            return RigidBody->BodyID;
        }

        if (auto* Character = Registry.try_get<SCharacterPhysicsComponent>(Entity))
        {
            return Character->GetBodyID();
        }

        return JPH::BodyID::cInvalidBodyID;
    }

    int32 FJoltPhysicsScene::ResolveHitBoneIndex(entt::entity Entity, JPH::BodyID BodyID) const
    {
        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        if (!Registry.valid(Entity))
        {
            return INDEX_NONE;
        }

        const SRagdollComponent* RagdollComp = Registry.try_get<SRagdollComponent>(Entity);
        if (RagdollComp == nullptr || !RagdollComp->Ragdoll || RagdollComp->Ragdoll->Ragdoll == nullptr)
        {
            return INDEX_NONE;
        }

        // Ragdolls are a handful of bodies; a linear scan beats maintaining a body->bone map.
        const FJoltRagdollHandle& Handle = *RagdollComp->Ragdoll;
        const int32 BodyCount = (int32)Handle.Ragdoll->GetBodyCount();
        for (int32 j = 0; j < BodyCount && j < (int32)Handle.JointToBone.size(); ++j)
        {
            if (Handle.Ragdoll->GetBodyID(j) == BodyID)
            {
                return Handle.JointToBone[j];
            }
        }
        return INDEX_NONE;
    }

    TOptional<SRayResult> FJoltPhysicsScene::CastRay(const SRayCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        JPH::Vec3 JPHStart  = JoltUtils::ToJPHVec3(Settings.Start);
        JPH::Vec3 JPHEnd    = JoltUtils::ToJPHVec3(Settings.End);
        JPH::Vec3 Direction = JPHEnd - JPHStart;
        
        if (Direction.Length() < LE_SMALL_NUMBER)
        {
            return eastl::nullopt;
        }
        
        JPH::RRayCast Ray;
        Ray.mOrigin = JPHStart;
        Ray.mDirection = Direction;
        
        class LayerMaskFilter : public JPH::ObjectLayerFilter
        {
        public:
            LayerMaskFilter(uint32 InLayerMask) 
                : LayerMask(InLayerMask) {}

            bool ShouldCollide(JPH::ObjectLayer InLayer) const override
            {
                ECollisionProfiles LayerA = (ECollisionProfiles)(uint16)(LayerMask & 0xFFFF);
                ECollisionProfiles MaskA  = (ECollisionProfiles)(uint16)(LayerMask >> 16);
    
                ECollisionProfiles LayerB = (ECollisionProfiles)(uint16)(InLayer & 0xFFFF);
                ECollisionProfiles MaskB  = (ECollisionProfiles)(uint16)(InLayer >> 16);

                return (MaskA & LayerB) != (ECollisionProfiles)0 || (MaskB & LayerA) != (ECollisionProfiles)0;
            }

            uint32 LayerMask;
        };
        
        FIgnoreFilter IgnoreFilter{Settings.IgnoreBodies};
        
        JPH::RayCastResult Hit;
        
        LayerMaskFilter LayerFilter{(uint32)Settings.LayerMask};

        bool bHit = JoltSystem->GetNarrowPhaseQuery().CastRay(Ray, Hit, {}, LayerFilter, IgnoreFilter);
        if (!bHit)
        {
            return eastl::nullopt;
        }
        
        const JPH::BodyLockInterfaceNoLock& LockInterface = JoltSystem->GetBodyLockInterfaceNoLock();
        
        JPH::Body* Body = LockInterface.TryGetBody(Hit.mBodyID);
        if (!Body)
        {
            return eastl::nullopt;
        }
        
        JPH::Vec3 SurfaceNormal = Body->GetWorldSpaceSurfaceNormal(Hit.mSubShapeID2, Ray.GetPointOnRay(Hit.mFraction));
        
        FVector3 Distance  = (Settings.Start - Settings.End);
        float Length        = Math::Length(Distance);
        
        SRayResult Result
        {
            .BodyID     = Hit.mBodyID.GetIndexAndSequenceNumber(),
            .Entity     = static_cast<uint32>(Body->GetUserData()),
            .Start      = Settings.Start,
            .End        = Settings.End,
            .Location   = JoltUtils::FromJPHRVec3(Ray.GetPointOnRay(Hit.mFraction)),
            .Normal     = Math::Normalize(JoltUtils::FromJPHVec3(SurfaceNormal)),
            .Fraction   = Hit.mFraction,
            .Distance   = Hit.mFraction * Length,
            .BoneIndex  = ResolveHitBoneIndex(static_cast<entt::entity>(static_cast<uint32>(Body->GetUserData())), Hit.mBodyID),
        };

        return Result;
    }

    TVector<SRayResult> FJoltPhysicsScene::CastSphere(const SSphereCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();
        
        TVector<SRayResult> Results;
        if (Math::IsNearlyZero(Settings.Radius))
        {
            return Results;
        }
        
        JPH::RVec3 JPHStart  = JoltUtils::ToJPHRVec3(Settings.Start);
        JPH::RVec3 JPHEnd    = JoltUtils::ToJPHRVec3(Settings.End);
        JPH::Vec3 Direction = (JPHEnd - JPHStart);
        
        class FMyCollector : public JPH::CastShapeCollector
        {
        public:
            
            FMyCollector(const FJoltPhysicsScene& InScene, TVector<SRayResult>& OutResults, const SSphereCastSettings& InSettings, const JPH::BodyLockInterfaceNoLock& NoLock)
                : Scene(InScene)
                , Out(OutResults)
                , Settings(InSettings)
                , Lock(NoLock)
            {}

            const FJoltPhysicsScene& Scene;
            TVector<SRayResult>& Out;
            const SSphereCastSettings& Settings;
            const JPH::BodyLockInterfaceNoLock& Lock;

            void AddHit(const JPH::ShapeCastResult& Hit) override
            {
                const JPH::Body* Body = Lock.TryGetBody(Hit.mBodyID2);

                SRayResult R;
                R.BodyID   = Hit.mBodyID2.GetIndexAndSequenceNumber();
                R.Entity   = (uint32)Body->GetUserData();
                R.Start    = Settings.Start;
                R.End      = Settings.End;
                R.Location = JoltUtils::FromJPHVec3(Hit.mContactPointOn2);
                R.Normal   = Math::Normalize(JoltUtils::FromJPHVec3(Hit.mPenetrationAxis.Normalized()));
                R.Fraction = Hit.mFraction;
                R.BoneIndex = Scene.ResolveHitBoneIndex(static_cast<entt::entity>(R.Entity), Hit.mBodyID2);

                Out.emplace_back(Move(R));
            }
        };

        const JPH::BodyLockInterfaceNoLock& Lock = JoltSystem->GetBodyLockInterfaceNoLock();


        FMyCollector Collector(*this, Results, Settings, Lock);
        FIgnoreFilter Filter{Settings.IgnoreBodies};
        
        JPH::SphereShape QuerySphere(Settings.Radius);
        JPH::RShapeCast ShapeCast = JPH::RShapeCast::sFromWorldTransform(&QuerySphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPHStart), Direction);
        
        JPH::ShapeCastSettings ShapeSettings;
        ShapeSettings.mBackFaceModeTriangles    = JPH::EBackFaceMode::CollideWithBackFaces;
        ShapeSettings.mBackFaceModeConvex       = JPH::EBackFaceMode::CollideWithBackFaces;
        ShapeSettings.mReturnDeepestPoint       = false;
        
        
        JoltSystem->GetNarrowPhaseQuery().CastShape(
            ShapeCast,
            ShapeSettings,
            JPH::RVec3::sZero(),
            Collector,
            JPH::BroadPhaseLayerFilter(),
            JPH::ObjectLayerFilter(),
            Filter,
            JPH::ShapeFilter()
        );
        
        #if JPH_DEBUG_RENDERER
        DEFER 
        { 
            FJoltPhysicsContext::GetDebugRenderer()->SetDrawDuration(0.0f); 
        };
        
        FJoltPhysicsContext::GetDebugRenderer()->SetWorld(World);
        FJoltPhysicsContext::GetDebugRenderer()->SetDrawDuration(Settings.DebugDuration);
        
        if (Settings.bDrawDebug)
        {
            QuerySphere.Draw(FJoltPhysicsContext::GetDebugRenderer(),
                JPH::RMat44::sTranslation(JPHStart), 
                JPH::Vec3::sReplicate(1.0f), 
                JPH::Color(0, 255, 0, 255),
                false, 
                true);
            
            if (Results.empty())
            {
                QuerySphere.Draw(FJoltPhysicsContext::GetDebugRenderer(),
                JPH::RMat44::sTranslation(JPHStart), 
                JPH::Vec3::sReplicate(1.0f), 
                JPH::Color(255, 0, 0, 255),
                false, 
                true);
            
                return Results;
            }
        }
        #endif
        
        eastl::sort(Results.begin(), Results.end(), [](const SRayResult& A, const SRayResult& B)
        {
            return A.Fraction < B.Fraction;
        });
        
        return Results;
    }

    namespace
    {
        // Collects the distinct entities of every body a CollideShape overlap touches. A single body can
        // report multiple sub-shape hits, so de-dup on the entity (linear scan: overlap result sets are small).
        class FOverlapCollector : public JPH::CollideShapeCollector
        {
        public:
            FOverlapCollector(const JPH::BodyLockInterfaceNoLock& InLock, TVector<entt::entity>& InOut)
                : Lock(InLock)
                , Out(InOut)
            {}

            void AddHit(const JPH::CollideShapeResult& Hit) override
            {
                const JPH::Body* Body = Lock.TryGetBody(Hit.mBodyID2);
                if (!Body)
                {
                    return;
                }
                const entt::entity E = static_cast<entt::entity>(static_cast<uint32>(Body->GetUserData()));
                if (eastl::find(Out.begin(), Out.end(), E) == Out.end())
                {
                    Out.push_back(E);
                }
            }

            const JPH::BodyLockInterfaceNoLock& Lock;
            TVector<entt::entity>&              Out;
        };
    }

    TVector<SRayResult> FJoltPhysicsScene::CastRayAll(const SRayCastSettings& Settings)
    {
        LUMINA_PROFILE_SCOPE();

        TVector<SRayResult> Results;

        const JPH::Vec3 JPHStart  = JoltUtils::ToJPHVec3(Settings.Start);
        const JPH::Vec3 JPHEnd    = JoltUtils::ToJPHVec3(Settings.End);
        const JPH::Vec3 Direction = JPHEnd - JPHStart;
        if (Direction.Length() < LE_SMALL_NUMBER)
        {
            return Results;
        }

        JPH::RRayCast Ray;
        Ray.mOrigin    = JPHStart;
        Ray.mDirection = Direction;

        const float RayLength = Math::Length(Settings.End - Settings.Start);
        const JPH::BodyLockInterfaceNoLock& Lock = JoltSystem->GetBodyLockInterfaceNoLock();

        class FRayAllCollector : public JPH::CastRayCollector
        {
        public:
            FRayAllCollector(const FJoltPhysicsScene& InScene, TVector<SRayResult>& OutResults, const SRayCastSettings& InSettings, const JPH::RRayCast& InRay, float InLength, const JPH::BodyLockInterfaceNoLock& InLock)
                : Scene(InScene), Out(OutResults), Settings(InSettings), Ray(InRay), Length(InLength), Lock(InLock)
            {}

            void AddHit(const JPH::RayCastResult& Hit) override
            {
                const JPH::Body* Body = Lock.TryGetBody(Hit.mBodyID);
                if (!Body)
                {
                    return;
                }
                const JPH::RVec3 Point  = Ray.GetPointOnRay(Hit.mFraction);
                const JPH::Vec3  Normal = Body->GetWorldSpaceSurfaceNormal(Hit.mSubShapeID2, Point);

                SRayResult R;
                R.BodyID   = Hit.mBodyID.GetIndexAndSequenceNumber();
                R.Entity   = static_cast<uint32>(Body->GetUserData());
                R.Start    = Settings.Start;
                R.End      = Settings.End;
                R.Location = FVector3(JoltUtils::FromJPHRVec3(Point));
                R.Normal   = Math::Normalize(JoltUtils::FromJPHVec3(Normal));
                R.Fraction = Hit.mFraction;
                R.Distance = Hit.mFraction * Length;
                R.BoneIndex = Scene.ResolveHitBoneIndex(static_cast<entt::entity>(R.Entity), Hit.mBodyID);
                Out.emplace_back(Move(R));
            }

            const FJoltPhysicsScene&            Scene;
            TVector<SRayResult>&                Out;
            const SRayCastSettings&             Settings;
            const JPH::RRayCast&                Ray;
            float                               Length;
            const JPH::BodyLockInterfaceNoLock& Lock;
        };

        FRayAllCollector Collector(*this, Results, Settings, Ray, RayLength, Lock);
        FIgnoreFilter Filter{Settings.IgnoreBodies};

        // Default layer filters = test every body along the ray (the "penetrate everything" use case);
        // IgnoreBodies still excludes specific bodies (e.g. the shooter's own).
        JoltSystem->GetNarrowPhaseQuery().CastRay(Ray, JPH::RayCastSettings(), Collector, {}, {}, Filter, {});

        std::sort(Results.begin(), Results.end(), [](const SRayResult& A, const SRayResult& B) { return A.Fraction < B.Fraction; });
        return Results;
    }

    void FJoltPhysicsScene::CollidePoint(const FVector3& Point, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities)
    {
        LUMINA_PROFILE_SCOPE();

        const JPH::BodyLockInterfaceNoLock& Lock = JoltSystem->GetBodyLockInterfaceNoLock();

        class FPointCollector : public JPH::CollidePointCollector
        {
        public:
            FPointCollector(const JPH::BodyLockInterfaceNoLock& InLock, TVector<entt::entity>& InOut)
                : Lock(InLock), Out(InOut)
            {}

            void AddHit(const JPH::CollidePointResult& Hit) override
            {
                const JPH::Body* Body = Lock.TryGetBody(Hit.mBodyID);
                if (!Body)
                {
                    return;
                }
                const entt::entity E = static_cast<entt::entity>(static_cast<uint32>(Body->GetUserData()));
                if (eastl::find(Out.begin(), Out.end(), E) == Out.end())
                {
                    Out.push_back(E);
                }
            }

            const JPH::BodyLockInterfaceNoLock& Lock;
            TVector<entt::entity>&              Out;
        };

        FPointCollector Collector(Lock, OutEntities);
        FIgnoreFilter Filter{IgnoreBodies};
        JoltSystem->GetNarrowPhaseQuery().CollidePoint(
            JoltUtils::ToJPHRVec3(FDoubleVector3(Point)), Collector, {}, {}, Filter, {});
    }

    void FJoltPhysicsScene::OverlapShapeInternal(const JPH::Shape* Shape, const JPH::RMat44& Transform, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities)
    {
        const JPH::BodyLockInterfaceNoLock& Lock = JoltSystem->GetBodyLockInterfaceNoLock();
        FOverlapCollector Collector(Lock, OutEntities);
        FIgnoreFilter Filter{IgnoreBodies};

        JoltSystem->GetNarrowPhaseQuery().CollideShape(
            Shape,
            JPH::Vec3::sReplicate(1.0f),
            Transform,
            JPH::CollideShapeSettings(),
            JPH::RVec3::sZero(),
            Collector,
            JPH::BroadPhaseLayerFilter(),
            JPH::ObjectLayerFilter(),
            Filter,
            JPH::ShapeFilter());
    }

    void FJoltPhysicsScene::OverlapSphere(const FVector3& Center, float Radius, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities)
    {
        LUMINA_PROFILE_SCOPE();
        if (Math::IsNearlyZero(Radius))
        {
            return;
        }
        JPH::SphereShape Shape(Radius);
        OverlapShapeInternal(&Shape, JPH::RMat44::sTranslation(JoltUtils::ToJPHRVec3(Center)), IgnoreBodies, OutEntities);
    }

    void FJoltPhysicsScene::OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const TVector<uint32>& IgnoreBodies, TVector<entt::entity>& OutEntities)
    {
        LUMINA_PROFILE_SCOPE();
        if (HalfExtents.x <= 0.0f || HalfExtents.y <= 0.0f || HalfExtents.z <= 0.0f)
        {
            return;
        }
        // Jolt enforces a minimum box half-extent (convex radius); clamp so thin volumes don't assert.
        const JPH::Vec3 Half = JPH::Vec3(Math::Max(HalfExtents.x, 0.01f), Math::Max(HalfExtents.y, 0.01f), Math::Max(HalfExtents.z, 0.01f));
        JPH::BoxShape Shape(Half);
        const JPH::RMat44 Transform = JPH::RMat44::sRotationTranslation(JoltUtils::ToJPHQuat(Rotation), JoltUtils::ToJPHRVec3(Center));
        OverlapShapeInternal(&Shape, Transform, IgnoreBodies, OutEntities);
    }

    void FJoltPhysicsScene::EnsureCharacterAllocators()
    {
        if (!CharacterAllocators.empty())
        {
            return;
        }

        // First character in this scene: stand up the per-worker substep allocator pool. 4 MiB
        // per worker is comfortably above a single CharacterVirtual::ExtendedUpdate's high-water.
        const uint32 NumWorkers = GTaskSystem ? GTaskSystem->GetNumTaskThreads() : 1u;
        CharacterAllocators.reserve(NumWorkers);
        for (uint32 i = 0; i < NumWorkers; ++i)
        {
            CharacterAllocators.push_back(MakeUnique<JPH::TempAllocatorImpl>(4ull * 1024 * 1024));
        }
    }

    void FJoltPhysicsScene::OnCharacterComponentConstructed(entt::registry& Registry, entt::entity Entity)
    {
        LUMINA_PROFILE_SCOPE();

        EnsureCharacterAllocators();

        SCharacterPhysicsComponent& CharacterComponent = Registry.get<SCharacterPhysicsComponent>(Entity);
        STransformComponent& TransformComponent = Registry.get<STransformComponent>(Entity);
        TransformComponent.SetHasPhysicsBody(true);   // setter MarkDirty now enqueues this entity for body re-sync

        auto Result = JPH::RotatedTranslatedShapeSettings(
            JPH::Vec3(0, 0, 0),
            JPH::Quat::sIdentity(),
            Memory::New<JPH::CapsuleShape>(CharacterComponent.HalfHeight, CharacterComponent.Radius * TransformComponent.MaxScale())).Create();

        if (Result.HasError())
        {
            LOG_ERROR("Failed to create Character for entity: {} - {}", entt::to_integral(Entity), Result.GetError());
            return;
        }

        const JPH::Ref<JPH::Shape>& StandingShape = Result.Get();
        
        JPH::Ref Settings                       = Memory::New<JPH::CharacterVirtualSettings>();
        Settings->mShape                        = StandingShape;
        Settings->mInnerBodyShape               = StandingShape;
        Settings->mInnerBodyLayer               = JoltUtils::PackToObjectLayer(CharacterComponent.CollisionProfile);
        Settings->mMass                         = CharacterComponent.Mass;
        Settings->mMaxStrength                  = CharacterComponent.MaxStrength;
        Settings->mMinTimeRemaining             = CharacterComponent.MinTimeRemaining;
        Settings->mMaxConstraintIterations      = CharacterComponent.MaxConstraintIterations;
        Settings->mMaxCollisionIterations       = CharacterComponent.MaxCollisionIterations;
        Settings->mCollisionTolerance           = CharacterComponent.CollisionTolerance;
        Settings->mMaxNumHits                   = CharacterComponent.MaxNumHits;
        Settings->mHitReductionCosMaxAngle      = CharacterComponent.HitReductionCosMaxAngle;
        Settings->mCharacterPadding             = CharacterComponent.Padding;
        Settings->mPenetrationRecoverySpeed     = CharacterComponent.PenetrationRecoverySpeed;
        Settings->mPredictiveContactDistance    = CharacterComponent.PredictiveContactDistance;
        Settings->mSupportingVolume             = JPH::Plane(JPH::Vec3::sAxisY(), 0.0f);
        
        JPH::Ref<JPH::CharacterVirtual> Character = Memory::New<JPH::CharacterVirtual>(Settings,
            JoltUtils::ToJPHRVec3(TransformComponent.GetLocation()),
            JoltUtils::ToJPHQuat(TransformComponent.GetRotation()),
            0,
            JoltSystem.get());

        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        BodyInterface.SetUserData(Character->GetInnerBodyID(), entt::to_integral(Entity));

        // Entity id on the character itself so the contact listener can resolve it; smoother motion over
        // mesh seams; and route the capsule's contacts to the shared character listener.
        Character->SetUserData(entt::to_integral(Entity));
        Character->SetEnhancedInternalEdgeRemoval(CharacterComponent.bEnhancedInternalEdgeRemoval);
        Character->SetListener(CharacterContactListener.get());

        // Opt into mutual char-vs-char pushing. Its inner body joins the proxy set so OTHER characters'
        // movement collision skips it -- char-char then runs solely through CharacterVsCharacterCollision.
        if (CharacterComponent.bCollideWithCharacters)
        {
            Character->SetCharacterVsCharacterCollision(CharacterVsCharacter.get());
            CharacterVsCharacter->Add(Character.GetPtr());
            CharacterProxyBodies.insert(Character->GetInnerBodyID().GetIndexAndSequenceNumber());
        }

        // Wrap into the pimpl handle the component owns (component header
        // doesn't see <Jolt/...> directly).
        CharacterComponent.Character        = MakeShared<FJoltCharacterHandle>();
        CharacterComponent.Character->Ref   = Move(Character);

        // Seed interpolation state so the first frame blends from a valid quaternion.
        CharacterComponent.LastBodyPosition = TransformComponent.GetLocation();
        CharacterComponent.LastBodyRotation = TransformComponent.GetRotation();
    }

    void FJoltPhysicsScene::OnCharacterComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        // Clear the transform's has-body hint unless a rigid body still remains on the entity.
        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(Registry.any_of<SRigidBodyComponent>(Entity));
        }

        DropRenderTransform(Registry, Entity);

        // Drop the character from the char-vs-char registry before its CharacterVirtual is released (the
        // shared list holds raw pointers). The proxy set is the source of truth for membership, so this is
        // correct even if bCollideWithCharacters was toggled after construction.
        SCharacterPhysicsComponent* CharacterComponent = Registry.try_get<SCharacterPhysicsComponent>(Entity);
        if (CharacterComponent == nullptr || !CharacterComponent->Character || CharacterComponent->Character->Ref == nullptr)
        {
            return;
        }

        JPH::CharacterVirtual* Character = CharacterComponent->Character->Ref.GetPtr();
        const uint32 InnerID = Character->GetInnerBodyID().GetIndexAndSequenceNumber();
        if (CharacterProxyBodies.find(InnerID) != CharacterProxyBodies.end())
        {
            CharacterVsCharacter->Remove(Character);
            CharacterProxyBodies.erase(InnerID);
        }
    }

    void FJoltPhysicsScene::OnRigidBodyComponentUpdated(entt::registry& Registry, entt::entity Entity)
    {
        
    }

    // Collision geometry comes from LOD 0's meshlets, which every mesh keeps after upload (the raw index and
    // vertex streams are dropped). Sourced from the resource rather than a CMesh so the dynamic-mesh path --
    // which owns an FMeshResource without owning a CMesh -- shares exactly this builder.
    static JPH::ShapeRefC BuildMeshColliderShape(const FMeshResource& Resource, FStringView DebugName,
                                                 const FVector3& Scale, bool bConvex)
    {
        const FMeshletData& MD = Resource.MeshletData;

        if (MD.IsEmpty() || Resource.bSkinnedMesh)
        {
            return nullptr;
        }

        JPH::VertexList         Vertices;
        JPH::IndexedTriangleList Triangles;

        for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
        {
            const uint32 Offset = Surface.LODMeshletOffset[0];
            const uint32 Count  = Surface.LODMeshletCount[0];
            for (uint32 i = 0; i < Count; ++i)
            {
                const FMeshlet& M       = MD.Meshlets[Offset + i];
                const uint32 BaseVertex = (uint32)Vertices.size();

                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    FVector3 Pos = MD.MeshletVertices[M.VertexOffset + v].Position;
                    Pos *= Scale;
                    Vertices.push_back(JPH::Float3(Pos.x, Pos.y, Pos.z));
                }

                for (uint32 t = 0; t < M.TriangleCount; ++t)
                {
                    const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                    const uint32 i0 = (Packed      ) & 0xFFu;
                    const uint32 i1 = (Packed >>  8) & 0xFFu;
                    const uint32 i2 = (Packed >> 16) & 0xFFu;
                    Triangles.emplace_back(BaseVertex + i0, BaseVertex + i1, BaseVertex + i2, 0);
                }
            }
        }

        if (Vertices.empty() || Triangles.empty())
        {
            return nullptr;
        }

        if (bConvex)
        {
            JPH::Array<JPH::Vec3> Points;
            Points.reserve(Vertices.size());
            for (const JPH::Float3& V : Vertices)
            {
                Points.emplace_back(V.x, V.y, V.z);
            }

            JPH::ConvexHullShapeSettings Settings(Points);
            Settings.SetEmbedded();
            auto Result = Settings.Create();
            if (Result.HasError())
            {
                LOG_ERROR("Failed to build convex hull from mesh '{}': {}", DebugName, Result.GetError());
                return nullptr;
            }
            return Result.Get();
        }

        JPH::MeshShapeSettings Settings(std::move(Vertices), std::move(Triangles));
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to build triangle mesh from mesh '{}': {}", DebugName, Result.GetError());
            return nullptr;
        }
        return Result.Get();
    }

    static JPH::ShapeRefC BuildTerrainHeightFieldShape(const STerrainComponent& Terrain)
    {
        const int32 Res = Terrain.Resolution;
        if (Res < 2 || (int64)Terrain.Heightmap.size() < (int64)Res * (int64)Res)
        {
            return nullptr;
        }

        // Jolt rounds mSampleCount up to mBlockSize multiples; buffer must match or Jolt reads OOB.
        constexpr uint32 BlockSize = 2;
        const uint32 SampleCount   = ((uint32(Res) + BlockSize - 1) / BlockSize) * BlockSize;
        const float   Stride       = Terrain.TileWorldSize / float(Res - 1);
        const float   HalfSize     = Terrain.TileWorldSize * 0.5f;

        TVector<float> Samples;
        Samples.resize(size_t(SampleCount) * size_t(SampleCount));

        // Copy heightmap, scaling normalized [0,1] to world Y; replicate edge for padding rows/cols.
        for (uint32 Y = 0; Y < SampleCount; ++Y)
        {
            const uint32 SrcY = Y < uint32(Res) ? Y : uint32(Res - 1);
            for (uint32 X = 0; X < SampleCount; ++X)
            {
                const uint32 SrcX = X < uint32(Res) ? X : uint32(Res - 1);
                Samples[size_t(Y) * SampleCount + X] = Terrain.Heightmap[size_t(SrcY) * size_t(Res) + size_t(SrcX)] * Terrain.MaxHeight;
            }
        }

        JPH::HeightFieldShapeSettings Settings(
            Samples.data(),
            JPH::Vec3(-HalfSize, 0.0f, -HalfSize),
            JPH::Vec3(Stride, 1.0f, Stride),
            SampleCount);
        Settings.mBlockSize = BlockSize;
        Settings.SetEmbedded();

        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to build terrain heightfield shape: {}", Result.GetError());
            return nullptr;
        }
        return Result.Get();
    }

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
        JPH::BodyCreationSettings   Settings;
        FVector3                   LastBodyPosition = FVector3(0.0f);
        FQuat                   LastBodyRotation = FQuat::Identity();
        // Resolved from the collider's PhysicsMaterial (if any), copied into BodyMaterials on commit.
        // Cached on the build result so the parallel build never touches the registry post-commit.
        bool                       bHasMaterial = false;
        float                      MaterialFriction = 0.0f;
        float                      MaterialRestitution = 0.0f;
        uint8                      MaterialFrictionCombine = 0;
        uint8                      MaterialRestitutionCombine = 0;
        // Conveyor surface velocity from an SConveyorComponent (world space; zero = none).
        FVector3                   SurfaceLinearVelocity = FVector3(0.0f);
        FVector3                   SurfaceAngularVelocity = FVector3(0.0f);
    };

    // Thread-safe: reads only registry + loaded assets; no PhysicsSystem mutation.
    static EBodyBuildStatus TryBuildRigidBodyCreationSettings(FJoltPhysicsScene* Scene, entt::registry& Registry, entt::entity Entity, FRigidBodyBuildResult& Out)
    {
        LUMINA_PROFILE_SCOPE();

        const SRigidBodyComponent* RigidBodyComponent = Registry.try_get<SRigidBodyComponent>(Entity);
        if (RigidBodyComponent == nullptr)
        {
            return EBodyBuildStatus::Error;
        }

        if (RigidBodyComponent->BodyID != JPH::BodyID::cInvalidBodyID)
        {
            return EBodyBuildStatus::AlreadyExists;
        }

        const STransformComponent* TransformComponent = Registry.try_get<STransformComponent>(Entity);
        if (TransformComponent == nullptr)
        {
            return EBodyBuildStatus::Defer;
        }

        JPH::ShapeRefC Shape;
        FVector3 ColliderTranslationOffset(0.0f);
        FVector3 ColliderRotationOffset(0.0f);
        bool      bIsTriangleMesh = false;
        bool      bColliderIsTrigger = false;
        const CPhysicsMaterial* ResolvedMaterial = nullptr;

        if (const SBoxColliderComponent* BC = Registry.try_get<SBoxColliderComponent>(Entity))
        {
            ColliderTranslationOffset       = BC->TranslationOffset;
            ColliderRotationOffset          = BC->RotationOffset;
            bColliderIsTrigger              = BC->bIsTrigger;
            ResolvedMaterial                = BC->PhysicsMaterial.Get();

            Shape = Scene->GetOrCreateBoxShape(BC->HalfExtent * TransformComponent->GetScale());
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create BoxCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SSphereColliderComponent* SC = Registry.try_get<SSphereColliderComponent>(Entity))
        {
            ColliderTranslationOffset           = SC->TranslationOffset;
            bColliderIsTrigger                  = SC->bIsTrigger;
            ResolvedMaterial                    = SC->PhysicsMaterial.Get();

            Shape = Scene->GetOrCreateSphereShape(SC->Radius * TransformComponent->MaxScale());
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create SphereCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SCapsuleColliderComponent* CC = Registry.try_get<SCapsuleColliderComponent>(Entity))
        {
            ColliderTranslationOffset           = CC->TranslationOffset;
            ColliderRotationOffset              = CC->RotationOffset;
            bColliderIsTrigger                  = CC->bIsTrigger;
            ResolvedMaterial                    = CC->PhysicsMaterial.Get();

            // Capsule scales uniformly with the entity (Jolt has no non-uniform capsule).
            const float ScaleFactor = TransformComponent->MaxScale();
            Shape = Scene->GetOrCreateCapsuleShape(CC->Radius * ScaleFactor, CC->HalfHeight * ScaleFactor);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create CapsuleCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SCylinderColliderComponent* CyC = Registry.try_get<SCylinderColliderComponent>(Entity))
        {
            ColliderTranslationOffset           = CyC->TranslationOffset;
            ColliderRotationOffset              = CyC->RotationOffset;
            bColliderIsTrigger                  = CyC->bIsTrigger;
            ResolvedMaterial                    = CyC->PhysicsMaterial.Get();

            const float ScaleFactor = TransformComponent->MaxScale();
            Shape = Scene->GetOrCreateCylinderShape(
                CyC->Radius     * ScaleFactor,
                CyC->HalfHeight * ScaleFactor,
                CyC->CapRadius  * ScaleFactor);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create CylinderCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const STaperedCapsuleColliderComponent* TCC = Registry.try_get<STaperedCapsuleColliderComponent>(Entity))
        {
            ColliderTranslationOffset           = TCC->TranslationOffset;
            ColliderRotationOffset              = TCC->RotationOffset;
            bColliderIsTrigger                  = TCC->bIsTrigger;
            ResolvedMaterial                    = TCC->PhysicsMaterial.Get();

            const float ScaleFactor = TransformComponent->MaxScale();
            Shape = Scene->GetOrCreateTaperedCapsuleShape(TCC->HalfHeight * ScaleFactor, TCC->TopRadius * ScaleFactor, TCC->BottomRadius * ScaleFactor);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create TaperedCapsuleCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const STaperedCylinderColliderComponent* TCyC = Registry.try_get<STaperedCylinderColliderComponent>(Entity))
        {
            ColliderTranslationOffset           = TCyC->TranslationOffset;
            ColliderRotationOffset              = TCyC->RotationOffset;
            bColliderIsTrigger                  = TCyC->bIsTrigger;
            ResolvedMaterial                    = TCyC->PhysicsMaterial.Get();

            const float ScaleFactor = TransformComponent->MaxScale();
            Shape = Scene->GetOrCreateTaperedCylinderShape(
                TCyC->HalfHeight   * ScaleFactor,
                TCyC->TopRadius    * ScaleFactor,
                TCyC->BottomRadius * ScaleFactor,
                TCyC->ConvexRadius * ScaleFactor);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create TaperedCylinderCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SPlaneColliderComponent* PC = Registry.try_get<SPlaneColliderComponent>(Entity))
        {
            bColliderIsTrigger = PC->bIsTrigger;
            ResolvedMaterial   = PC->PhysicsMaterial.Get();

            // Local plane through the origin with +Y normal; the entity's rotation/position orient + place it
            // (negative half-space is solid). Built inline -- planes are few and rarely share dimensions.
            JPH::PlaneShapeSettings PlaneSettings(
                JPH::Plane::sFromPointAndNormal(JPH::Vec3::sZero(), JPH::Vec3::sAxisY()),
                nullptr,
                Math::Max(PC->HalfExtent, 1.0f));
            PlaneSettings.SetEmbedded();
            auto PlaneResult = PlaneSettings.Create();
            if (PlaneResult.HasError())
            {
                LOG_ERROR("Failed to create PlaneCollider Shape for Entity: {} - {}", entt::to_integral(Entity), PlaneResult.GetError());
                return EBodyBuildStatus::Error;
            }
            Shape = PlaneResult.Get();

            // PlaneShape::MustBeStatic() -> true; force the body Static (shares the triangle-mesh path below).
            bIsTriangleMesh = true;
        }
        else if (const SCompoundColliderComponent* CompC = Registry.try_get<SCompoundColliderComponent>(Entity))
        {
            bColliderIsTrigger = CompC->bIsTrigger;
            ResolvedMaterial   = CompC->PhysicsMaterial.Get();

            // Children carry their own local offsets, so the outer collider-offset wrapper stays disabled.
            // All children are convex primitives, so the compound can be a dynamic body.
            Shape = Scene->BuildCompoundShape(*CompC, *TransformComponent);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create CompoundCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }
        }
        else if (const SMeshColliderComponent* MC = Registry.try_get<SMeshColliderComponent>(Entity))
        {
            ColliderTranslationOffset = MC->TranslationOffset;
            ColliderRotationOffset    = MC->RotationOffset;
            bColliderIsTrigger        = MC->bIsTrigger;
            ResolvedMaterial          = MC->PhysicsMaterial.Get();

            CStaticMesh* Mesh = MC->Mesh.Get();
            if (Mesh == nullptr)
            {
                if (const SStaticMeshComponent* SMC = Registry.try_get<SStaticMeshComponent>(Entity))
                {
                    Mesh = SMC->StaticMesh.Get();
                }
            }

            if (Mesh == nullptr || Mesh->HasAnyFlag(OF_NeedsLoad) || Mesh->GetMeshResource().MeshletData.IsEmpty())
            {
                return EBodyBuildStatus::Defer;
            }

            Shape = Scene->GetOrCreateMeshShape(Mesh, TransformComponent->GetScale(), MC->bConvex);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create MeshCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }

            bIsTriangleMesh = !MC->bConvex;
        }
        else if (const SDynamicMeshColliderComponent* DMC = Registry.try_get<SDynamicMeshColliderComponent>(Entity))
        {
            bColliderIsTrigger = DMC->bIsTrigger;
            ResolvedMaterial   = DMC->PhysicsMaterial.Get();

            const SDynamicMeshComponent* DM = Registry.try_get<SDynamicMeshComponent>(Entity);

            // Ref-held across the shape build: Commit can swap the component's data from a worker, and
            // BuildMeshColliderShape reads the resource for the whole call.
            const TSharedPtr<FDynamicMeshRenderData> MeshData = DM ? DM->LoadRenderData() : nullptr;
            if (!MeshData || MeshData->Resource.MeshletData.IsEmpty())
            {
                // Nothing committed yet. Defer rather than error: a streamed chunk builds its geometry a
                // frame or more after the entity exists, and the pending list retries.
                return EBodyBuildStatus::Defer;
            }

            // Deliberately uncached: a dynamic mesh is unique per component and rebuilt on every commit, so
            // the CMesh-keyed shape cache has nothing to reuse.
            Shape = BuildMeshColliderShape(MeshData->Resource, "DynamicMesh",
                                           TransformComponent->GetScale(), DMC->bConvex);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create DynamicMeshCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }

            bIsTriangleMesh = !DMC->bConvex;
        }
        else if (const STerrainColliderComponent* TC = Registry.try_get<STerrainColliderComponent>(Entity))
        {
            bColliderIsTrigger = TC->bIsTrigger;
            ResolvedMaterial   = TC->PhysicsMaterial.Get();

            const STerrainComponent* Terrain = Registry.try_get<STerrainComponent>(Entity);
            if (Terrain == nullptr || Terrain->Heightmap.empty())
            {
                return EBodyBuildStatus::Defer;
            }

            Shape = BuildTerrainHeightFieldShape(*Terrain);
            if (Shape == nullptr)
            {
                LOG_ERROR("Failed to create TerrainCollider Shape for Entity: {}", entt::to_integral(Entity));
                return EBodyBuildStatus::Error;
            }

            // HeightFieldShape::MustBeStatic() -> true; the body must be Static.
            bIsTriangleMesh = true;
        }
        else
        {
            return EBodyBuildStatus::NoCollider;
        }

        JPH::ObjectLayer Layer      = JoltUtils::PackToObjectLayer(RigidBodyComponent->CollisionProfile);
        JPH::EMotionType MotionType = ToJoltMotionType(RigidBodyComponent->BodyType);

        // Jolt's triangle mesh shape only supports Static / Kinematic bodies.
        if (bIsTriangleMesh && MotionType == JPH::EMotionType::Dynamic)
        {
            LOG_WARN("MeshCollider on Entity {} is a non-convex triangle mesh; forcing motion type to Static.", entt::to_integral(Entity));
            MotionType = JPH::EMotionType::Static;
        }

        FQuat Rotation      = TransformComponent->GetRotation();
        FVector3 Position      = TransformComponent->GetLocation();

        // Skip the wrapper when there's no offset; unoffset bodies can share the bare cached shape.
        const bool bHasColliderOffset = Math::LengthSquared(ColliderTranslationOffset) > LE_SMALL_NUMBER
                                     || Math::LengthSquared(ColliderRotationOffset)    > LE_SMALL_NUMBER;
        if (bHasColliderOffset)
        {
            FQuat QuatRotation(ColliderRotationOffset);
            JPH::RotatedTranslatedShapeSettings RTS(JoltUtils::ToJPHVec3(ColliderTranslationOffset), JoltUtils::ToJPHQuat(QuatRotation), Shape);
            auto RTSResult = RTS.Create();
            if (RTSResult.HasError())
            {
                LOG_ERROR("Failed to create offset shape for Entity: {} - {}", entt::to_integral(Entity), RTSResult.GetError());
                return EBodyBuildStatus::Error;
            }

            Shape = RTSResult.Get();
        }

        if (Math::Dot(RigidBodyComponent->CenterOfMassOffset, RigidBodyComponent->CenterOfMassOffset) > 0.0f)
        {
            JPH::OffsetCenterOfMassShapeSettings COMS(JoltUtils::ToJPHVec3(RigidBodyComponent->CenterOfMassOffset), Shape);
            auto COMResult = COMS.Create();
            if (COMResult.HasError())
            {
                LOG_ERROR("Failed to apply CenterOfMassOffset for Entity: {} - {}", entt::to_integral(Entity), COMResult.GetError());
                return EBodyBuildStatus::Error;
            }
            Shape = COMResult.Get();
        }

        Out.Settings = JPH::BodyCreationSettings(
            Shape,
            JoltUtils::ToJPHRVec3(Position),
            JoltUtils::ToJPHQuat(Rotation),
            MotionType,
            Layer);

        Out.Settings.mNumPositionStepsOverride  = RigidBodyComponent->NumPositionStepsOverride;
        Out.Settings.mNumVelocityStepsOverride  = RigidBodyComponent->NumVelocityStepsOverride;
        Out.Settings.mIsSensor                  = RigidBodyComponent->bIsSensor || bColliderIsTrigger;
        Out.Settings.mUseManifoldReduction      = RigidBodyComponent->bUseManifoldReduction;
        Out.Settings.mApplyGyroscopicForce      = RigidBodyComponent->bApplyGyroscopicForce;
        Out.Settings.mMotionQuality             = RigidBodyComponent->MotionQualityLevel == 0 ? JPH::EMotionQuality::Discrete : JPH::EMotionQuality::LinearCast;
        Out.Settings.mMaxLinearVelocity         = RigidBodyComponent->MaxLinearVelocity;
        Out.Settings.mMaxAngularVelocity        = RigidBodyComponent->MaxAngularVelocity;

        // Per-axis DOF locks: start from All and clear each locked axis. Skip if nothing is locked, or if
        // every axis is locked (EAllowedDOFs::None is invalid in Jolt -- such a body should be Static).
        {
            JPH::EAllowedDOFs DOFs = JPH::EAllowedDOFs::All;
            if (RigidBodyComponent->bLockTranslationX) { DOFs &= ~JPH::EAllowedDOFs::TranslationX; }
            if (RigidBodyComponent->bLockTranslationY) { DOFs &= ~JPH::EAllowedDOFs::TranslationY; }
            if (RigidBodyComponent->bLockTranslationZ) { DOFs &= ~JPH::EAllowedDOFs::TranslationZ; }
            if (RigidBodyComponent->bLockRotationX)    { DOFs &= ~JPH::EAllowedDOFs::RotationX; }
            if (RigidBodyComponent->bLockRotationY)    { DOFs &= ~JPH::EAllowedDOFs::RotationY; }
            if (RigidBodyComponent->bLockRotationZ)    { DOFs &= ~JPH::EAllowedDOFs::RotationZ; }
            if (DOFs != JPH::EAllowedDOFs::All && DOFs != JPH::EAllowedDOFs::None)
            {
                Out.Settings.mAllowedDOFs = DOFs;
            }
        }
        if (ResolvedMaterial != nullptr)
        {
            Out.Settings.mFriction              = ResolvedMaterial->Friction;
            Out.Settings.mRestitution           = ResolvedMaterial->Restitution;
            Out.bHasMaterial                    = true;
            Out.MaterialFriction                = ResolvedMaterial->Friction;
            Out.MaterialRestitution             = ResolvedMaterial->Restitution;
            Out.MaterialFrictionCombine         = (uint8)ResolvedMaterial->FrictionCombine;
            Out.MaterialRestitutionCombine      = (uint8)ResolvedMaterial->RestitutionCombine;
        }
        else
        {
            Out.Settings.mRestitution           = RigidBodyComponent->RestitutionOverride;
            Out.Settings.mFriction              = RigidBodyComponent->FrictionOverride;
        }
        Out.Settings.mAngularDamping            = RigidBodyComponent->AngularDamping;
        Out.Settings.mLinearDamping             = RigidBodyComponent->LinearDamping;

        if (MotionType == JPH::EMotionType::Dynamic)
        {
            if (RigidBodyComponent->bOverrideInertia)
            {
                // Supply both mass and a diagonal inertia tensor; Jolt uses them verbatim.
                Out.Settings.mOverrideMassProperties         = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                Out.Settings.mMassPropertiesOverride.mMass   = RigidBodyComponent->Mass;
                Out.Settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(JoltUtils::ToJPHVec3(RigidBodyComponent->InertiaTensor));
            }
            else if (RigidBodyComponent->bOverrideMass)
            {
                Out.Settings.mOverrideMassProperties        = JPH::EOverrideMassProperties::CalculateInertia;
                Out.Settings.mMassPropertiesOverride.mMass  = RigidBodyComponent->Mass;
            }
        }

        if (const SConveyorComponent* Conveyor = Registry.try_get<SConveyorComponent>(Entity))
        {
            Out.SurfaceLinearVelocity  = Conveyor->SurfaceVelocity;
            Out.SurfaceAngularVelocity = Conveyor->AngularSurfaceVelocity;
        }

        Out.LastBodyPosition = Position;
        Out.LastBodyRotation = Rotation;

        return EBodyBuildStatus::Success;
    }

    void FJoltPhysicsScene::OnRigidBodyComponentConstructed(entt::registry& Registry, entt::entity Entity)
    {
        LUMINA_PROFILE_SCOPE();

        // Flag the transform so setter MarkDirty enqueues this entity for the body re-sync.
        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(true);
        }

        // Inside a game-thread batch (e.g. fracture): collect and create together in EndBodyBatch.
        if (bBatchingBodies && !bStepInProgress.load(std::memory_order_acquire))
        {
            BatchedBodyCreations.push_back(Entity);
            return;
        }

        // Jolt forbids CreateBody/AddBody during a step; defer if the scene is mid-step.
        if (bStepInProgress.load(std::memory_order_acquire))
        {
            FScopeLock Lock(PendingRigidBodyMutex);
            PendingRigidBodyCreations.push(Entity);
            return;
        }

        CreateRigidBodyImmediate(Registry, Entity);
    }

    void FJoltPhysicsScene::BeginBodyBatch()
    {
        // Game-thread only; fracture is synchronous so no nesting is expected.
        bBatchingBodies = true;
        BatchedBodyCreations.clear();
    }

    void FJoltPhysicsScene::EndBodyBatch()
    {
        bBatchingBodies = false;
        CreateRigidBodiesBatched(BatchedBodyCreations);
        BatchedBodyCreations.clear();
    }

    namespace
    {
        // Quaternion (w,x,y,z) rotating by Angle (radians) about a unit Axis.
        FQuat RagdollAxisAngle(const FVector3& Axis, float Angle)
        {
            const float Half = Angle * 0.5f;
            const float S = Math::Sin(Half);
            return FQuat(Math::Cos(Half), Axis.x * S, Axis.y * S, Axis.z * S);
        }

        // Rotation taking +Y onto Dir (Dir assumed normalized). Jolt capsules run along local Y.
        FQuat RagdollYToDir(const FVector3& Dir)
        {
            const FVector3 Y(0.0f, 1.0f, 0.0f);
            const float D = Math::Clamp(Math::Dot(Y, Dir), -1.0f, 1.0f);
            if (D > 0.99999f)
            {
                return FQuat::Identity();
            }
            if (D < -0.99999f)
            {
                return RagdollAxisAngle(FVector3(0.0f, 0.0f, 1.0f), LE_PI_F);
            }
            return RagdollAxisAngle(Math::Normalize(Math::Cross(Y, Dir)), Math::Acos(D));
        }

        // Any unit vector perpendicular to Axis (assumed normalized).
        FVector3 RagdollPerpendicular(const FVector3& Axis)
        {
            const FVector3 Ref = Math::Abs(Axis.x) < 0.9f ? FVector3(1.0f, 0.0f, 0.0f) : FVector3(0.0f, 0.0f, 1.0f);
            return Math::Normalize(Math::Cross(Axis, Ref));
        }

        // One resolved ragdoll body before it becomes a Jolt part.
        struct FRagdollBodyDef
        {
            int32           BoneIndex = INDEX_NONE;
            int32           ParentBodyIndex = -1;
            FVector3        WorldPos;
            FQuat           WorldRot;
            JPH::ShapeRefC  Shape;
            bool            bOverrideMass = false;
            float           Mass = 1.0f;
            float           TwistDeg = 30.0f;
            float           Swing1Deg = 45.0f;
            float           Swing2Deg = 45.0f;
        };
    }

    TSharedPtr<FJoltRagdollHandle> FJoltPhysicsScene::CreateRagdoll(const FRagdollDesc& Desc)
    {
        LUMINA_PROFILE_SCOPE();

        const FSkeletonResource* Skeleton = Desc.Skeleton;
        if (Skeleton == nullptr || Desc.ComponentBoneGlobals == nullptr)
        {
            return nullptr;
        }

        const TVector<FMatrix4>& Globals = *Desc.ComponentBoneGlobals;
        const int32 NumBones = Skeleton->GetNumBones();
        if ((int32)Globals.size() != NumBones || NumBones == 0)
        {
            return nullptr;
        }

        const FCollisionProfile Profile = Desc.Asset ? Desc.Asset->CollisionProfile : Desc.FallbackProfile;
        const JPH::ObjectLayer Layer = JoltUtils::PackToObjectLayer(Profile);

        // Resolve which bones get bodies (authored or auto-generated), sorted parent-before-child (ascending
        // bone index, since the skeleton stores parents before children).
        TVector<FRagdollBodyDef> Defs;
        THashMap<int32, int32> BoneToBody;

        auto DecomposeWorld = [&](int32 BoneIndex, FVector3& OutPos, FQuat& OutRot)
        {
            const FMatrix4 World = Desc.EntityToWorld * Globals[BoneIndex];
            FVector3 Scale;
            AnimPose::DecomposeTRS(World, OutPos, OutRot, Scale);
        };

        if (Desc.Asset && !Desc.Asset->Bodies.empty())
        {
            // Stable order by bone index.
            TVector<int32> Order;
            Order.reserve(Desc.Asset->Bodies.size());
            for (int32 i = 0; i < (int32)Desc.Asset->Bodies.size(); ++i)
            {
                const int32 BoneIndex = Skeleton->FindBoneIndex(Desc.Asset->Bodies[i].BoneName);
                if (BoneIndex != INDEX_NONE)
                {
                    Order.push_back(i);
                }
            }
            std::sort(Order.begin(), Order.end(), [&](int32 A, int32 B)
            {
                return Skeleton->FindBoneIndex(Desc.Asset->Bodies[A].BoneName) < Skeleton->FindBoneIndex(Desc.Asset->Bodies[B].BoneName);
            });

            for (int32 SetupIdx : Order)
            {
                const SPhysicsBodySetup& Setup = Desc.Asset->Bodies[SetupIdx];
                const int32 BoneIndex = Skeleton->FindBoneIndex(Setup.BoneName);

                FRagdollBodyDef Def;
                Def.BoneIndex = BoneIndex;
                Def.bOverrideMass = Setup.bOverrideMass;
                Def.Mass = Setup.Mass;
                DecomposeWorld(BoneIndex, Def.WorldPos, Def.WorldRot);

                // Inner primitive.
                JPH::Shape* InnerRaw = nullptr;
                switch (Setup.Shape)
                {
                case ERagdollBodyShape::Box:
                    InnerRaw = Memory::New<JPH::BoxShape>(JoltUtils::ToJPHVec3(Setup.HalfExtent));
                    break;
                case ERagdollBodyShape::Sphere:
                    InnerRaw = Memory::New<JPH::SphereShape>(Setup.Radius);
                    break;
                default:
                    InnerRaw = Memory::New<JPH::CapsuleShape>(Setup.HalfHeight, Setup.Radius);
                    break;
                }

                const FQuat OffsetRot = FQuat(Math::Radians(Setup.RotationOffset));
                auto Result = JPH::RotatedTranslatedShapeSettings(
                    JoltUtils::ToJPHVec3(Setup.TranslationOffset),
                    JoltUtils::ToJPHQuat(OffsetRot).Normalized(),
                    InnerRaw).Create();
                if (Result.HasError())
                {
                    LOG_ERROR("Ragdoll shape create failed for bone {}: {}", BoneIndex, Result.GetError().c_str());
                    continue;
                }
                Def.Shape = Result.Get();

                BoneToBody[BoneIndex] = (int32)Defs.size();
                Defs.push_back(Move(Def));
            }
        }
        else
        {
            // Auto-generate: a capsule per bone fit toward the average child position.
            for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
            {
                FRagdollBodyDef Def;
                Def.BoneIndex = BoneIndex;
                FVector3 Scale;
                AnimPose::DecomposeTRS(Desc.EntityToWorld * Globals[BoneIndex], Def.WorldPos, Def.WorldRot, Scale);

                // Component-space bone origin + average child origin -> limb direction in bone-local space.
                const FVector3 BonePos(Globals[BoneIndex][3]);
                FQuat BoneRotC; FVector3 BonePosC, BoneScaleC;
                AnimPose::DecomposeTRS(Globals[BoneIndex], BonePosC, BoneRotC, BoneScaleC);

                const TVector<int32> Children = Skeleton->GetChildBones(BoneIndex);
                JPH::Shape* InnerRaw = nullptr;
                JPH::Vec3 OffsetPos = JPH::Vec3::sZero();
                JPH::Quat OffsetRot = JPH::Quat::sIdentity();

                if (!Children.empty())
                {
                    FVector3 ChildAvg(0.0f);
                    for (int32 C : Children)
                    {
                        ChildAvg += FVector3(Globals[C][3]);
                    }
                    ChildAvg /= (float)Children.size();

                    const FVector3 DirC = ChildAvg - BonePos;
                    const float Length = Math::Length(DirC);
                    if (Length > 1e-4f)
                    {
                        const FVector3 DirLocal = Math::Normalize(Math::Inverse(BoneRotC) * DirC);
                        const float Radius = Math::Clamp(Length * 0.12f, 0.02f, 0.1f);
                        const float HalfHeight = Math::Max(Length * 0.5f - Radius, 0.01f);
                        InnerRaw = Memory::New<JPH::CapsuleShape>(HalfHeight, Radius);
                        OffsetPos = JoltUtils::ToJPHVec3(DirLocal * (Length * 0.5f));
                        OffsetRot = JoltUtils::ToJPHQuat(RagdollYToDir(DirLocal)).Normalized();
                    }
                }

                if (InnerRaw == nullptr)
                {
                    InnerRaw = Memory::New<JPH::SphereShape>(0.03f);
                }

                auto Result = JPH::RotatedTranslatedShapeSettings(OffsetPos, OffsetRot, InnerRaw).Create();
                if (Result.HasError())
                {
                    LOG_ERROR("Ragdoll shape create failed for bone {}: {}", BoneIndex, Result.GetError().c_str());
                    continue;
                }
                Def.Shape = Result.Get();

                BoneToBody[BoneIndex] = (int32)Defs.size();
                Defs.push_back(Move(Def));
            }
        }

        if (Defs.empty())
        {
            return nullptr;
        }

        // Resolve each body's parent body by walking up the bone hierarchy to the nearest bodied ancestor.
        for (FRagdollBodyDef& Def : Defs)
        {
            int32 Parent = Skeleton->GetBone(Def.BoneIndex).ParentIndex;
            while (Parent >= 0)
            {
                auto It = BoneToBody.find(Parent);
                if (It != BoneToBody.end())
                {
                    Def.ParentBodyIndex = It->second;
                    break;
                }
                Parent = Skeleton->GetBone(Parent).ParentIndex;
            }
        }

        // Build the Jolt ragdoll settings.
        JPH::Ref<JPH::RagdollSettings> Settings = Memory::New<JPH::RagdollSettings>();
        Settings->mSkeleton = Memory::New<JPH::Skeleton>();
        Settings->mParts.resize(Defs.size());

        for (int32 i = 0; i < (int32)Defs.size(); ++i)
        {
            const FRagdollBodyDef& Def = Defs[i];
            const FString BoneName = Skeleton->GetBone(Def.BoneIndex).Name.ToString();
            Settings->mSkeleton->AddJoint(BoneName.c_str(), Def.ParentBodyIndex);

            JPH::RagdollSettings::Part& Part = Settings->mParts[i];
            Part.SetShape(Def.Shape.GetPtr());
            Part.mPosition = JoltUtils::ToJPHRVec3(FDoubleVector3(Def.WorldPos));
            Part.mRotation = JoltUtils::ToJPHQuat(Def.WorldRot).Normalized();
            Part.mMotionType = JPH::EMotionType::Dynamic;
            Part.mObjectLayer = Layer;
            Part.mUserData = (uint64)entt::to_integral(Desc.Entity);
            Part.mAllowSleeping = true;
            if (Def.bOverrideMass)
            {
                Part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                Part.mMassPropertiesOverride.mMass = Def.Mass;
            }

            if (Def.ParentBodyIndex >= 0)
            {
                const FRagdollBodyDef& ParentDef = Defs[Def.ParentBodyIndex];

                FVector3 TwistAxis = Def.WorldPos - ParentDef.WorldPos;
                if (Math::Length(TwistAxis) > 1e-4f)
                {
                    TwistAxis = Math::Normalize(TwistAxis);
                }
                else
                {
                    TwistAxis = Math::Normalize(Def.WorldRot * FVector3(0.0f, 1.0f, 0.0f));
                }
                const FVector3 PlaneAxis = RagdollPerpendicular(TwistAxis);

                float Twist = Def.TwistDeg, Swing1 = Def.Swing1Deg, Swing2 = Def.Swing2Deg;
                if (Desc.Asset)
                {
                    const FName ChildBone = Skeleton->GetBone(Def.BoneIndex).Name;
                    for (const SPhysicsConstraintSetup& C : Desc.Asset->Constraints)
                    {
                        if (C.ChildBone == ChildBone)
                        {
                            Twist = C.TwistLimitDegrees; Swing1 = C.Swing1LimitDegrees; Swing2 = C.Swing2LimitDegrees;
                            break;
                        }
                    }
                }

                JPH::Ref<JPH::SwingTwistConstraintSettings> Cs = Memory::New<JPH::SwingTwistConstraintSettings>();
                Cs->mSpace = JPH::EConstraintSpace::WorldSpace;
                Cs->mPosition1 = Cs->mPosition2 = JoltUtils::ToJPHRVec3(FDoubleVector3(Def.WorldPos));
                Cs->mTwistAxis1 = Cs->mTwistAxis2 = JoltUtils::ToJPHVec3(TwistAxis);
                Cs->mPlaneAxis1 = Cs->mPlaneAxis2 = JoltUtils::ToJPHVec3(PlaneAxis);
                Cs->mNormalHalfConeAngle = Math::Radians(Swing1);
                Cs->mPlaneHalfConeAngle = Math::Radians(Swing2);
                Cs->mTwistMinAngle = -Math::Radians(Twist);
                Cs->mTwistMaxAngle = Math::Radians(Twist);
                Part.mToParent = Cs;
            }
        }

        Settings->Stabilize();
        Settings->DisableParentChildCollisions();
        Settings->CalculateBodyIndexToConstraintIndex();

        JPH::Ragdoll* Ragdoll = Settings->CreateRagdoll(
            (JPH::CollisionGroup::GroupID)Desc.CollisionGroupID,
            (uint64)entt::to_integral(Desc.Entity),
            JoltSystem.get());
        if (Ragdoll == nullptr)
        {
            return nullptr;
        }

        TSharedPtr<FJoltRagdollHandle> Handle = MakeShared<FJoltRagdollHandle>();
        Handle->Settings = Settings;
        Handle->Ragdoll = Ragdoll;
        Handle->JointToBone.resize(Defs.size());
        for (int32 i = 0; i < (int32)Defs.size(); ++i)
        {
            Handle->JointToBone[i] = Defs[i].BoneIndex;
        }

        Ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
        Handle->bAddedToScene = true;
        return Handle;
    }

    void FJoltPhysicsScene::ReadRagdollPose(const FJoltRagdollHandle& Handle, const FMatrix4& WorldToEntity, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms)
    {
        LUMINA_PROFILE_SCOPE();

        if (Handle.Ragdoll == nullptr || Skeleton == nullptr)
        {
            return;
        }

        const int32 NumBones = Skeleton->GetNumBones();
        OutBoneTransforms.resize(NumBones);

        TVector<FMatrix4> ComponentGlobals;
        ComponentGlobals.resize(NumBones);
        TVector<uint8> Mapped;
        Mapped.resize(NumBones, 0);

        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();

        for (int32 j = 0; j < (int32)Handle.JointToBone.size(); ++j)
        {
            const int32 BoneIndex = Handle.JointToBone[j];
            if (!Skeleton->IsBoneIndexValid(BoneIndex))
            {
                continue;
            }
            const JPH::BodyID BodyID = Handle.Ragdoll->GetBodyID(j);
            const FVector3 Pos = FVector3(JoltUtils::FromJPHRVec3(BodyInterface.GetPosition(BodyID)));
            const FQuat Rot = JoltUtils::FromJPHQuat(BodyInterface.GetRotation(BodyID));
            const FMatrix4 WorldBone = AnimPose::ComposeTRS(Pos, Rot, FVector3(1.0f));
            ComponentGlobals[BoneIndex] = WorldToEntity * WorldBone;
            Mapped[BoneIndex] = 1;
        }

        // Bones without a body follow their parent rigidly via the bind-pose local transform (parents
        // precede children in the bone array, so a single forward pass resolves them).
        for (int32 i = 0; i < NumBones; ++i)
        {
            if (!Mapped[i])
            {
                const int32 Parent = Skeleton->GetBone(i).ParentIndex;
                const FMatrix4& Local = Skeleton->GetBone(i).LocalTransform;
                ComponentGlobals[i] = Parent >= 0 ? ComponentGlobals[Parent] * Local : Local;
            }
            OutBoneTransforms[i] = ComponentGlobals[i] * Skeleton->GetBone(i).InvBindMatrix;
        }
    }

    void FJoltPhysicsScene::DestroyRagdoll(const TSharedPtr<FJoltRagdollHandle>& Handle)
    {
        if (!Handle || Handle->Ragdoll == nullptr)
        {
            return;
        }
        if (Handle->bAddedToScene)
        {
            Handle->Ragdoll->RemoveFromPhysicsSystem();
            Handle->bAddedToScene = false;
        }
        Handle->Ragdoll = nullptr;
        Handle->Settings = nullptr;
    }

    void FJoltPhysicsScene::GetRagdollRootTransform(const FJoltRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation)
    {
        OutPosition = FVector3(0.0f);
        OutRotation = FQuat::Identity();

        if (Handle.Ragdoll == nullptr || Handle.Ragdoll->GetBodyCount() == 0)
        {
            return;
        }

        // Body 0 is the ragdoll's root (lowest bodied bone; its bodied ancestors are none).
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        const JPH::BodyID RootID = Handle.Ragdoll->GetBodyID(0);
        OutPosition = FVector3(JoltUtils::FromJPHRVec3(BodyInterface.GetPosition(RootID)));
        OutRotation = JoltUtils::FromJPHQuat(BodyInterface.GetRotation(RootID));
    }

    //================================================================================================
    // Constraints / joints. Built from a Jolt-free FConstraintDesc; bodies resolved from entities, frames
    // world-space. Create/Destroy run on the game thread outside the step (same contract as CreateRagdoll);
    // the breakable monitor runs on the physics step right after Update().
    //================================================================================================

    namespace
    {
        // An axis perpendicular to In (normalized); seeds the hinge/slider reference normal when the caller
        // doesn't care about the zero-angle orientation.
        FVector3 ConstraintPerpendicular(const FVector3& In)
        {
            const FVector3 N   = Math::LengthSquared(In) > LE_SMALL_NUMBER ? Math::Normalize(In) : FVector3(0.0f, 1.0f, 0.0f);
            const FVector3 Ref = Math::Abs(N.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            return Math::Normalize(Math::Cross(N, Ref));
        }

        void ConfigureMotor(JPH::MotorSettings& Motor, const FConstraintDesc& Desc, bool bAngular)
        {
            if (Desc.MotorFrequency > 0.0f)
            {
                Motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, Desc.MotorFrequency, Desc.MotorDamping);
            }
            if (bAngular)
            {
                if (Desc.MotorTorqueLimit > 0.0f) { Motor.SetTorqueLimit(Desc.MotorTorqueLimit); }
            }
            else if (Desc.MotorForceLimit > 0.0f)
            {
                Motor.SetForceLimit(Desc.MotorForceLimit);
            }
        }

        // Linear impulse the joint applied last step, converted to an average force (N) for break detection.
        float ConstraintAppliedForce(const JPH::TwoBodyConstraint* C, EPhysicsConstraintType Type, float Dt)
        {
            if (C == nullptr || Dt <= 0.0f) { return 0.0f; }
            float Impulse = 0.0f;
            switch (Type)
            {
            case EPhysicsConstraintType::Point:    Impulse = static_cast<const JPH::PointConstraint*>(C)->GetTotalLambdaPosition().Length(); break;
            case EPhysicsConstraintType::Fixed:    Impulse = static_cast<const JPH::FixedConstraint*>(C)->GetTotalLambdaPosition().Length(); break;
            case EPhysicsConstraintType::Hinge:    Impulse = static_cast<const JPH::HingeConstraint*>(C)->GetTotalLambdaPosition().Length(); break;
            case EPhysicsConstraintType::Cone:     Impulse = static_cast<const JPH::ConeConstraint*>(C)->GetTotalLambdaPosition().Length(); break;
            case EPhysicsConstraintType::Distance: Impulse = Math::Abs(static_cast<const JPH::DistanceConstraint*>(C)->GetTotalLambdaPosition()); break;
            case EPhysicsConstraintType::Slider:
            {
                const JPH::Vector<2> L = static_cast<const JPH::SliderConstraint*>(C)->GetTotalLambdaPosition();
                Impulse = Math::Sqrt(L[0] * L[0] + L[1] * L[1]);
                break;
            }
            }
            return Impulse / Dt;
        }
    }

    uint32 FJoltPhysicsScene::CreateConstraint(const FConstraintDesc& Desc)
    {
        LUMINA_PROFILE_SCOPE();

        // All callers are outside the actual JoltSystem->Update() step: gameplay scripts run on the game
        // thread (physics joined), and the component drain runs at the top of Update() before the step. So
        // body locking + AddConstraint here is safe; we intentionally do not gate on bStepInProgress.

        // Resolve both bodies; entt::null or a body-less entity attaches that side to the world.
        auto ResolveID = [&](entt::entity E) -> JPH::BodyID
        {
            if (E == entt::null) { return JPH::BodyID(); }
            const uint32 Raw = GetEntityBodyID(E);
            return Raw == 0xFFFFFFFFu ? JPH::BodyID() : JPH::BodyID(Raw);
        };
        const JPH::BodyID IDA = ResolveID(Desc.BodyA);
        const JPH::BodyID IDB = ResolveID(Desc.BodyB);

        // Two world anchors constrain nothing.
        if (IDA.IsInvalid() && IDB.IsInvalid())
        {
            return 0;
        }

        // Lock the real bodies for stable Body& refs; the world side uses Body::sFixedToWorld.
        const JPH::BodyLockInterface& LockInterface = JoltSystem->GetBodyLockInterface();
        JPH::BodyID ToLock[2];
        int32 NumLock = 0;
        int32 SlotA = -1, SlotB = -1;
        if (!IDA.IsInvalid()) { SlotA = NumLock; ToLock[NumLock++] = IDA; }
        if (!IDB.IsInvalid()) { SlotB = NumLock; ToLock[NumLock++] = IDB; }

        JPH::BodyLockMultiWrite Lock(LockInterface, ToLock, NumLock);
        JPH::Body* BodyA = &JPH::Body::sFixedToWorld;
        JPH::Body* BodyB = &JPH::Body::sFixedToWorld;
        if (SlotA >= 0) { BodyA = Lock.GetBody(SlotA); }
        if (SlotB >= 0) { BodyB = Lock.GetBody(SlotB); }
        if (BodyA == nullptr || BodyB == nullptr)
        {
            return 0;
        }

        const JPH::RVec3 Anchor = JoltUtils::ToJPHRVec3(FDoubleVector3(Desc.Anchor));
        const FVector3   AxisN  = Math::LengthSquared(Desc.Axis) > LE_SMALL_NUMBER ? Math::Normalize(Desc.Axis) : FVector3(0.0f, 1.0f, 0.0f);
        const JPH::Vec3  Axis   = JoltUtils::ToJPHVec3(AxisN);
        const JPH::Vec3  Normal = JoltUtils::ToJPHVec3(ConstraintPerpendicular(AxisN));

        JPH::TwoBodyConstraint* Created = nullptr;
        switch (Desc.Type)
        {
        case EPhysicsConstraintType::Fixed:
        {
            JPH::FixedConstraintSettings S;
            S.mSpace = JPH::EConstraintSpace::WorldSpace;
            S.mAutoDetectPoint = true;     // Weld at the bodies' current relative pose.
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        case EPhysicsConstraintType::Point:
        {
            JPH::PointConstraintSettings S;
            S.mSpace = JPH::EConstraintSpace::WorldSpace;
            S.mPoint1 = S.mPoint2 = Anchor;
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        case EPhysicsConstraintType::Distance:
        {
            JPH::DistanceConstraintSettings S;
            S.mSpace  = JPH::EConstraintSpace::WorldSpace;
            S.mPoint1 = Anchor;
            S.mPoint2 = JoltUtils::ToJPHRVec3(FDoubleVector3(Desc.AnchorB));
            if (Desc.bHasLimits) { S.mMinDistance = Desc.MinLimit; S.mMaxDistance = Desc.MaxLimit; }
            if (Desc.LimitFrequency > 0.0f) { S.mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, Desc.LimitFrequency, Desc.LimitDamping); }
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        case EPhysicsConstraintType::Hinge:
        {
            JPH::HingeConstraintSettings S;
            S.mSpace = JPH::EConstraintSpace::WorldSpace;
            S.mPoint1 = S.mPoint2 = Anchor;
            S.mHingeAxis1 = S.mHingeAxis2 = Axis;
            S.mNormalAxis1 = S.mNormalAxis2 = Normal;
            if (Desc.bHasLimits) { S.mLimitsMin = Desc.MinLimit; S.mLimitsMax = Desc.MaxLimit; }
            if (Desc.LimitFrequency > 0.0f) { S.mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, Desc.LimitFrequency, Desc.LimitDamping); }
            S.mMaxFrictionTorque = Desc.MaxFriction;
            ConfigureMotor(S.mMotorSettings, Desc, true);
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        case EPhysicsConstraintType::Slider:
        {
            JPH::SliderConstraintSettings S;
            S.mSpace = JPH::EConstraintSpace::WorldSpace;
            S.mAutoDetectPoint = true;
            S.SetSliderAxis(Axis);
            if (Desc.bHasLimits) { S.mLimitsMin = Desc.MinLimit; S.mLimitsMax = Desc.MaxLimit; }
            if (Desc.LimitFrequency > 0.0f) { S.mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, Desc.LimitFrequency, Desc.LimitDamping); }
            S.mMaxFrictionForce = Desc.MaxFriction;
            ConfigureMotor(S.mMotorSettings, Desc, false);
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        case EPhysicsConstraintType::Cone:
        {
            JPH::ConeConstraintSettings S;
            S.mSpace = JPH::EConstraintSpace::WorldSpace;
            S.mPoint1 = S.mPoint2 = Anchor;
            S.mTwistAxis1 = S.mTwistAxis2 = Axis;
            S.mHalfConeAngle = Desc.HalfConeAngle;
            Created = S.Create(*BodyA, *BodyB);
            break;
        }
        }

        if (Created == nullptr)
        {
            return 0;
        }

        JoltSystem->AddConstraint(Created);

        FScopeLock MapLock(ConstraintsMutex);
        const uint32 Handle = NextConstraintID++;
        FJoltConstraint& Entry = Constraints[Handle];
        Entry.Constraint = Created;     // JPH::Ref adopts ownership.
        Entry.Type       = Desc.Type;
        Entry.BreakForce = Desc.BreakForce;
        Entry.bBroken    = false;
        return Handle;
    }

    void FJoltPhysicsScene::DestroyConstraint(uint32 ConstraintID)
    {
        if (ConstraintID == 0) { return; }

        JPH::Ref<JPH::TwoBodyConstraint> KeepAlive;
        {
            FScopeLock MapLock(ConstraintsMutex);
            auto It = Constraints.find(ConstraintID);
            if (It == Constraints.end()) { return; }
            KeepAlive = It->second.Constraint;
            Constraints.erase(It);
        }
        if (KeepAlive != nullptr)
        {
            JoltSystem->RemoveConstraint(KeepAlive.GetPtr());
        }
    }

    void FJoltPhysicsScene::SetConstraintEnabled(uint32 ConstraintID, bool bEnabled)
    {
        FScopeLock MapLock(ConstraintsMutex);
        auto It = Constraints.find(ConstraintID);
        if (It != Constraints.end() && It->second.Constraint != nullptr)
        {
            It->second.Constraint->SetEnabled(bEnabled);
            if (bEnabled) { It->second.bBroken = false; }
        }
    }

    void FJoltPhysicsScene::SetConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target)
    {
        FScopeLock MapLock(ConstraintsMutex);
        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end() || It->second.Constraint == nullptr) { return; }

        const JPH::EMotorState State = Mode == EConstraintMotorMode::Velocity ? JPH::EMotorState::Velocity
                                     : Mode == EConstraintMotorMode::Position ? JPH::EMotorState::Position
                                     : JPH::EMotorState::Off;

        if (It->second.Type == EPhysicsConstraintType::Hinge)
        {
            auto* H = static_cast<JPH::HingeConstraint*>(It->second.Constraint.GetPtr());
            H->SetMotorState(State);
            if (Mode == EConstraintMotorMode::Velocity)      { H->SetTargetAngularVelocity(Target); }
            else if (Mode == EConstraintMotorMode::Position) { H->SetTargetAngle(Target); }
        }
        else if (It->second.Type == EPhysicsConstraintType::Slider)
        {
            auto* Sl = static_cast<JPH::SliderConstraint*>(It->second.Constraint.GetPtr());
            Sl->SetMotorState(State);
            if (Mode == EConstraintMotorMode::Velocity)      { Sl->SetTargetVelocity(Target); }
            else if (Mode == EConstraintMotorMode::Position) { Sl->SetTargetPosition(Target); }
        }
    }

    bool FJoltPhysicsScene::IsConstraintBroken(uint32 ConstraintID)
    {
        FScopeLock MapLock(ConstraintsMutex);
        auto It = Constraints.find(ConstraintID);
        return It != Constraints.end() && It->second.bBroken;
    }

    float FJoltPhysicsScene::GetConstraintValue(uint32 ConstraintID)
    {
        FScopeLock MapLock(ConstraintsMutex);
        auto It = Constraints.find(ConstraintID);
        if (It == Constraints.end() || It->second.Constraint == nullptr)
        {
            return 0.0f;
        }
        if (It->second.Type == EPhysicsConstraintType::Hinge)
        {
            return static_cast<JPH::HingeConstraint*>(It->second.Constraint.GetPtr())->GetCurrentAngle();
        }
        if (It->second.Type == EPhysicsConstraintType::Slider)
        {
            return static_cast<JPH::SliderConstraint*>(It->second.Constraint.GetPtr())->GetCurrentPosition();
        }
        return 0.0f;
    }

    void FJoltPhysicsScene::MonitorBreakableConstraints(float Dt)
    {
        FScopeLock MapLock(ConstraintsMutex);
        for (auto It = Constraints.begin(); It != Constraints.end(); ++It)
        {
            FJoltConstraint& Entry = It->second;
            if (Entry.BreakForce <= 0.0f || Entry.bBroken || Entry.Constraint == nullptr)
            {
                continue;
            }
            if (ConstraintAppliedForce(Entry.Constraint.GetPtr(), Entry.Type, Dt) > Entry.BreakForce)
            {
                Entry.Constraint->SetEnabled(false);
                Entry.bBroken = true;
            }
        }
    }

    void FJoltPhysicsScene::DestroyAllConstraints()
    {
        FScopeLock MapLock(ConstraintsMutex);
        for (auto It = Constraints.begin(); It != Constraints.end(); ++It)
        {
            if (It->second.Constraint != nullptr)
            {
                JoltSystem->RemoveConstraint(It->second.Constraint.GetPtr());
            }
        }
        Constraints.clear();
    }

    void FJoltPhysicsScene::OnConstraintComponentConstructed(entt::registry& Registry, entt::entity Entity)
    {
        // Reset any handle copied in by duplication, then queue for deferred creation (bodies may not exist yet).
        if (SPhysicsConstraintComponent* C = Registry.try_get<SPhysicsConstraintComponent>(Entity))
        {
            C->ConstraintID = 0;
        }
        FScopeLock Lock(PendingConstraintMutex);
        PendingConstraintCreations.push_back(Entity);
    }

    void FJoltPhysicsScene::OnConstraintComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        if (SPhysicsConstraintComponent* C = Registry.try_get<SPhysicsConstraintComponent>(Entity))
        {
            if (C->ConstraintID != 0)
            {
                DestroyConstraint(C->ConstraintID);
                C->ConstraintID = 0;
            }
        }
    }

    bool FJoltPhysicsScene::TryCreateComponentConstraint(entt::registry& Registry, entt::entity Entity)
    {
        SPhysicsConstraintComponent* C = Registry.try_get<SPhysicsConstraintComponent>(Entity);
        if (C == nullptr || C->ConstraintID != 0)
        {
            return true;    // gone or already built; stop retrying.
        }

        // This entity is the child (body B) and must have a body before we can pin a joint to it.
        const uint32 BodyBID = GetEntityBodyID(Entity);
        if (BodyBID == 0xFFFFFFFFu)
        {
            return false;   // retry next frame.
        }

        // Optional parent (body A); when set, wait for its body too.
        entt::entity Target = entt::null;
        uint32 TargetBodyID = 0xFFFFFFFFu;
        if (C->TargetBody != 0xFFFFFFFFu)
        {
            Target = static_cast<entt::entity>(C->TargetBody);
            TargetBodyID = Registry.valid(Target) ? GetEntityBodyID(Target) : 0xFFFFFFFFu;
            if (TargetBodyID == 0xFFFFFFFFu)
            {
                return false;
            }
        }

        // Resolve the world-space frame from body B's live transform (BodyID overload; the entity helper is
        // hidden by this class's GetBodyPosition(uint32) override).
        const FVector3 BodyPos = GetBodyPosition(BodyBID);
        const FQuat    BodyRot = GetBodyRotation(BodyBID);

        FConstraintDesc Desc;
        Desc.Type        = C->Type;
        Desc.BodyA       = Target;          // parent (null => world).
        Desc.BodyB       = Entity;          // child.
        Desc.Anchor      = BodyPos + BodyRot * C->PivotOffset;
        Desc.Axis        = BodyRot * C->Axis;
        Desc.MaxFriction = C->Friction;
        Desc.BreakForce  = C->BreakForce;

        switch (C->Type)
        {
        case EPhysicsConstraintType::Hinge:
            Desc.bHasLimits = C->bLimited;
            Desc.MinLimit   = Math::Radians(C->LowerLimit);
            Desc.MaxLimit   = Math::Radians(C->UpperLimit);
            break;
        case EPhysicsConstraintType::Slider:
            Desc.bHasLimits = C->bLimited;
            Desc.MinLimit   = C->LowerLimit;
            Desc.MaxLimit   = C->UpperLimit;
            break;
        case EPhysicsConstraintType::Cone:
            Desc.HalfConeAngle = Math::Radians(C->ConeHalfAngle);
            break;
        case EPhysicsConstraintType::Distance:
            Desc.bHasLimits = C->bLimited;
            Desc.MinLimit   = C->LowerLimit;
            Desc.MaxLimit   = C->UpperLimit;
            Desc.AnchorB    = (TargetBodyID != 0xFFFFFFFFu) ? GetBodyPosition(TargetBodyID) : Desc.Anchor;
            break;
        default:
            break;
        }

        C->ConstraintID = CreateConstraint(Desc);   // 0 on failure -> we stop retrying a bad config.
        return true;
    }

    void FJoltPhysicsScene::DrainPendingConstraints()
    {
        TVector<entt::entity> Batch;
        {
            FScopeLock Lock(PendingConstraintMutex);
            if (PendingConstraintCreations.empty())
            {
                return;
            }
            Batch.swap(PendingConstraintCreations);
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);
        for (entt::entity Entity : Batch)
        {
            if (!Registry.valid(Entity))
            {
                continue;
            }
            if (!TryCreateComponentConstraint(Registry, Entity))
            {
                FScopeLock Lock(PendingConstraintMutex);
                PendingConstraintCreations.push_back(Entity);   // bodies not ready; retry next frame.
            }
        }
    }

    void FJoltPhysicsScene::CreateRigidBodyImmediate(entt::registry& Registry, entt::entity Entity)
    {
        LUMINA_PROFILE_SCOPE();

        FRigidBodyBuildResult BuildResult;
        const EBodyBuildStatus Status = TryBuildRigidBodyCreationSettings(this, Registry, Entity, BuildResult);

        switch (Status)
        {
        case EBodyBuildStatus::Defer:
        case EBodyBuildStatus::NoCollider:
        {
            FScopeLock Lock(PendingRigidBodyMutex);
            PendingRigidBodyCreations.push(Entity);
            return;
        }
        case EBodyBuildStatus::AlreadyExists:
        case EBodyBuildStatus::Error:
            return;
        case EBodyBuildStatus::Success:
            break;
        }

        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        JPH::Body* Body                   = BodyInterface.CreateBody(BuildResult.Settings);

        if (Body == nullptr)
        {
            LOG_ERROR("Failed to create body for Entity: {}", entt::to_integral(Entity));
            return;
        }

        Body->SetUserData(static_cast<uint64>(Entity));

        SRigidBodyComponent& RigidBodyComponent = Registry.get<SRigidBodyComponent>(Entity);
        RigidBodyComponent.BodyID               = Body->GetID().GetIndexAndSequenceNumber();
        RigidBodyComponent.LastBodyPosition     = BuildResult.LastBodyPosition;
        RigidBodyComponent.LastBodyRotation     = BuildResult.LastBodyRotation;
        RigidBodyComponent.Mass                 = BuildResult.Settings.GetMassProperties().mMass;

        StoreBodyMaterial(Body->GetID(), BuildResult);

        BodyInterface.AddBody(Body->GetID(), JPH::EActivation::Activate);
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateSphereShape(float Radius)
    {
        const FShapeKey Key{ 0, Radius, 0.0f, 0.0f };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        JPH::SphereShapeSettings Settings(Radius);
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached sphere shape (radius {}): {}", Radius, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateBoxShape(const FVector3& HalfExtent)
    {
        const FShapeKey Key{ 1, HalfExtent.x, HalfExtent.y, HalfExtent.z };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        JPH::BoxShapeSettings Settings(JoltUtils::ToJPHVec3(HalfExtent));
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached box shape ({}, {}, {}): {}", HalfExtent.x, HalfExtent.y, HalfExtent.z, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateCapsuleShape(float Radius, float HalfHeight)
    {
        // Kind = 2 (capsule). Z unused.
        const FShapeKey Key{ 2, Radius, HalfHeight, 0.0f };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        JPH::CapsuleShapeSettings Settings(HalfHeight, Radius);
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached capsule shape (r {}, hh {}): {}", Radius, HalfHeight, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateCylinderShape(float Radius, float HalfHeight, float CapRadius)
    {
        // Kind = 3 (cylinder). Z = cap (edge-rounding) radius so two cylinders with different
        // bevels don't share a cached shape.
        const FShapeKey Key{ 3, Radius, HalfHeight, CapRadius };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        // Jolt requires CapRadius <= min(Radius, HalfHeight); clamp defensively so authoring an
        // oversized bevel never trips an internal assert.
        const float ClampedCap = Math::Clamp(CapRadius, 0.0f, Math::Min(Radius, HalfHeight));
        JPH::CylinderShapeSettings Settings(HalfHeight, Radius, ClampedCap);
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached cylinder shape (r {}, hh {}, cap {}): {}", Radius, HalfHeight, ClampedCap, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateTaperedCapsuleShape(float HalfHeight, float TopRadius, float BottomRadius)
    {
        // Kind = 4 (tapered capsule). Radii must be strictly positive (Jolt asserts otherwise).
        const float Top    = Math::Max(TopRadius, 0.001f);
        const float Bottom = Math::Max(BottomRadius, 0.001f);
        const FShapeKey Key{ 4, HalfHeight, Top, Bottom, 0.0f };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        JPH::TaperedCapsuleShapeSettings Settings(HalfHeight, Top, Bottom);
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached tapered capsule shape (hh {}, top {}, bottom {}): {}", HalfHeight, Top, Bottom, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateTaperedCylinderShape(float HalfHeight, float TopRadius, float BottomRadius, float ConvexRadius)
    {
        // Kind = 5 (tapered cylinder); W = convex (bevel) radius. Clamp the bevel so an oversized value
        // can't trip an internal assert.
        const float Top    = Math::Max(TopRadius, 0.001f);
        const float Bottom = Math::Max(BottomRadius, 0.001f);
        const float Convex = Math::Clamp(ConvexRadius, 0.0f, Math::Min(Math::Min(Top, Bottom), HalfHeight));
        const FShapeKey Key{ 5, HalfHeight, Top, Bottom, Convex };

        FScopeLock Lock(ShapeCacheMutex);
        auto It = ShapeCache.find(Key);
        if (It != ShapeCache.end())
        {
            return It->second;
        }

        JPH::TaperedCylinderShapeSettings Settings(HalfHeight, Top, Bottom, Convex);
        Settings.SetEmbedded();
        auto Result = Settings.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create cached tapered cylinder shape (hh {}, top {}, bottom {}, cv {}): {}", HalfHeight, Top, Bottom, Convex, Result.GetError());
            return {};
        }

        JPH::ShapeRefC Shape = Result.Get();
        ShapeCache.emplace(Key, Shape);
        return Shape;
    }

    JPH::ShapeRefC FJoltPhysicsScene::BuildCompoundShape(const SCompoundColliderComponent& Comp, const STransformComponent& Transform)
    {
        const float Scale = Transform.MaxScale();

        // Each child reuses the cached primitive shapes, so N identical legs share one JPH::Shape.
        auto BuildChild = [&](const SCompoundSubShape& Sub) -> JPH::ShapeRefC
        {
            switch (Sub.Type)
            {
            case ECompoundShapeType::Box:      return GetOrCreateBoxShape(Sub.HalfExtent * Scale);
            case ECompoundShapeType::Sphere:   return GetOrCreateSphereShape(Sub.Radius * Scale);
            case ECompoundShapeType::Capsule:  return GetOrCreateCapsuleShape(Sub.Radius * Scale, Sub.HalfHeight * Scale);
            case ECompoundShapeType::Cylinder: return GetOrCreateCylinderShape(Sub.Radius * Scale, Sub.HalfHeight * Scale, 0.0f);
            }
            return {};
        };

        if (Comp.Shapes.empty())
        {
            return {};
        }

        // A static compound needs >= 2 children; with exactly 1, use the child directly (offset/rotated).
        if (Comp.Shapes.size() == 1)
        {
            const SCompoundSubShape& Sub = Comp.Shapes[0];
            JPH::ShapeRefC Child = BuildChild(Sub);
            if (Child == nullptr)
            {
                return {};
            }
            const bool bHasOffset = Math::LengthSquared(Sub.Offset) > LE_SMALL_NUMBER || Math::LengthSquared(Sub.Rotation) > LE_SMALL_NUMBER;
            if (!bHasOffset)
            {
                return Child;
            }
            JPH::RotatedTranslatedShapeSettings RTS(JoltUtils::ToJPHVec3(Sub.Offset * Scale), JoltUtils::ToJPHQuat(FQuat(Sub.Rotation)), Child);
            auto Result = RTS.Create();
            return Result.HasError() ? JPH::ShapeRefC{} : Result.Get();
        }

        JPH::StaticCompoundShapeSettings Compound;
        Compound.SetEmbedded();
        uint32 ValidChildren = 0;
        for (const SCompoundSubShape& Sub : Comp.Shapes)
        {
            JPH::ShapeRefC Child = BuildChild(Sub);
            if (Child == nullptr)
            {
                continue;
            }
            Compound.AddShape(JoltUtils::ToJPHVec3(Sub.Offset * Scale), JoltUtils::ToJPHQuat(FQuat(Sub.Rotation)), Child.GetPtr());
            ++ValidChildren;
        }

        if (ValidChildren < 2)
        {
            return {};
        }

        auto Result = Compound.Create();
        if (Result.HasError())
        {
            LOG_ERROR("Failed to create compound shape: {}", Result.GetError());
            return {};
        }
        return Result.Get();
    }

    void FJoltPhysicsScene::StoreBodyMaterial(JPH::BodyID BodyID, const FRigidBodyBuildResult& Build)
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodyMaterials.size())
        {
            return;
        }
        FBodyMaterialEntry& Entry  = BodyMaterials[Index];
        Entry.bHasMaterial         = Build.bHasMaterial;
        Entry.Friction             = Build.MaterialFriction;
        Entry.Restitution          = Build.MaterialRestitution;
        Entry.FrictionCombine      = Build.MaterialFrictionCombine;
        Entry.RestitutionCombine   = Build.MaterialRestitutionCombine;

        // Seed the conveyor surface velocity from the authored component (runtime changes go through
        // SetSurfaceVelocity). Both creation paths route here, so this covers immediate + batched spawns.
        StoreBodySurfaceVelocity(BodyID, Build.SurfaceLinearVelocity, Build.SurfaceAngularVelocity);
    }

    void FJoltPhysicsScene::ClearBodyMaterial(JPH::BodyID BodyID)
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodyMaterials.size())
        {
            return;
        }
        BodyMaterials[Index] = FBodyMaterialEntry{};
        ClearBodySurfaceVelocity(BodyID);
    }

    const FJoltPhysicsScene::FBodyMaterialEntry* FJoltPhysicsScene::GetBodyMaterial(JPH::BodyID BodyID) const
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodyMaterials.size())
        {
            return nullptr;
        }
        return &BodyMaterials[Index];
    }

    void FJoltPhysicsScene::StoreBodySurfaceVelocity(JPH::BodyID BodyID, const FVector3& Linear, const FVector3& Angular)
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodySurfaceVelocities.size())
        {
            return;
        }
        FBodySurfaceVelocity& Entry = BodySurfaceVelocities[Index];
        Entry.Linear  = Linear;
        Entry.Angular = Angular;
        Entry.bActive = Math::LengthSquared(Linear) > LE_SMALL_NUMBER || Math::LengthSquared(Angular) > LE_SMALL_NUMBER;
    }

    void FJoltPhysicsScene::ClearBodySurfaceVelocity(JPH::BodyID BodyID)
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodySurfaceVelocities.size())
        {
            return;
        }
        BodySurfaceVelocities[Index] = FBodySurfaceVelocity{};
    }

    const FJoltPhysicsScene::FBodySurfaceVelocity* FJoltPhysicsScene::GetBodySurfaceVelocity(JPH::BodyID BodyID) const
    {
        const uint32 Index = BodyID.GetIndex();
        if (Index >= BodySurfaceVelocities.size())
        {
            return nullptr;
        }
        return &BodySurfaceVelocities[Index];
    }

    void FJoltPhysicsScene::SetSurfaceVelocity(entt::entity Entity, const FVector3& Linear, const FVector3& Angular)
    {
        const uint32 Raw = GetEntityBodyID(Entity);
        if (Raw != 0xFFFFFFFFu)
        {
            StoreBodySurfaceVelocity(JPH::BodyID(Raw), Linear, Angular);
        }
    }

    // Decorate a cached unscaled shape for one scaled instance. ScaledShape is a refcounted wrapper holding a
    // ref plus a Vec3, so instances share the one baked BVH/hull instead of each owning a copy.
    static JPH::ShapeRefC MakeScaledShape(const JPH::ShapeRefC& Base, const FVector3& Scale, FStringView DebugName)
    {
        if (Base == nullptr)
        {
            return Base;
        }

        const JPH::Vec3 JoltScale = JoltUtils::ToJPHVec3(Scale);
        if (JoltScale.IsClose(JPH::Vec3::sOne()))
        {
            return Base;   // unscaled instances use the cached shape directly
        }

        // Jolt rejects scales a given shape type can't represent (zero on an axis, shear on a rotated
        // compound child). MakeScaleValid drops the offending components; warn rather than silently
        // building a collider that doesn't match the mesh.
        const JPH::Vec3 ValidScale = Base->MakeScaleValid(JoltScale);
        if (!ValidScale.IsClose(JoltScale))
        {
            LOG_WARN("Mesh collider '{}' cannot take scale ({}, {}, {}); clamped to ({}, {}, {}).",
                     DebugName, Scale.x, Scale.y, Scale.z,
                     ValidScale.GetX(), ValidScale.GetY(), ValidScale.GetZ());
        }

        if (ValidScale.IsClose(JPH::Vec3::sOne()))
        {
            return Base;
        }

        return JPH::ShapeRefC(new JPH::ScaledShape(Base, ValidScale));
    }

    JPH::ShapeRefC FJoltPhysicsScene::GetOrCreateMeshShape(const CMesh* Mesh, const FVector3& Scale, bool bConvex)
    {
        const FMeshShapeKey Key{ Mesh, (uint8)(bConvex ? 1 : 0) };

        JPH::ShapeRefC Base;
        {
            FScopeLock Lock(MeshShapeCacheMutex);
            auto It = MeshShapeCache.find(Key);
            if (It != MeshShapeCache.end())
            {
                Base = It->second;
            }
        }

        if (Base == nullptr)
        {
            // Build outside the lock so parallel QuickHull on distinct meshes isn't serialized; the
            // try_emplace below only guards the rare same-key race. Always built at unit scale.
            JPH::ShapeRefC Built = BuildMeshColliderShape(Mesh->GetMeshResource(), Mesh->GetName().ToString(),
                                                          FVector3(1.0f), bConvex);
            if (Built == nullptr)
            {
                return {};
            }

            FScopeLock Lock(MeshShapeCacheMutex);
            Base = MeshShapeCache.try_emplace(Key, Built).first->second;
        }

        return MakeScaledShape(Base, Scale, Mesh->GetName().ToString());
    }

    void FJoltPhysicsScene::BulkCreateRigidBodies(entt::registry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        TVector<entt::entity> Candidates;
        auto View = Registry.view<SRigidBodyComponent>();
        Candidates.reserve(View.size_hint());
        for (entt::entity Entity : View)
        {
            Candidates.push_back(Entity);
        }

        CreateRigidBodiesBatched(Candidates);
    }

    void FJoltPhysicsScene::CreateRigidBodiesBatched(const TVector<entt::entity>& Candidates)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 Count = (uint32)Candidates.size();
        if (Count == 0)
        {
            return;
        }

        entt::registry& Registry = ECS::GetWorldRegistry(*World);

        TVector<FRigidBodyBuildResult> Results(Count);
        TVector<EBodyBuildStatus>      Statuses(Count, EBodyBuildStatus::Error);

        Task::ParallelFor(Count, [&](uint32 Index)
        {
            Statuses[Index] = TryBuildRigidBodyCreationSettings(this, Registry, Candidates[Index], Results[Index]);
        });

        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        TVector<JPH::BodyID> BodyIDsToAdd;
        BodyIDsToAdd.reserve(Count);

        for (uint32 i = 0; i < Count; ++i)
        {
            const entt::entity Entity = Candidates[i];
            switch (Statuses[i])
            {
            case EBodyBuildStatus::Defer:
            case EBodyBuildStatus::NoCollider:
            {
                FScopeLock Lock(PendingRigidBodyMutex);
                PendingRigidBodyCreations.push(Entity);
                continue;
            }
            case EBodyBuildStatus::AlreadyExists:
            case EBodyBuildStatus::Error:
                continue;
            case EBodyBuildStatus::Success:
                break;
            }

            JPH::Body* Body = BodyInterface.CreateBody(Results[i].Settings);
            if (Body == nullptr)
            {
                LOG_ERROR("Failed to create body for Entity: {}", entt::to_integral(Entity));
                continue;
            }

            Body->SetUserData(static_cast<uint64>(Entity));

            SRigidBodyComponent& RigidBodyComponent = Registry.get<SRigidBodyComponent>(Entity);
            RigidBodyComponent.BodyID               = Body->GetID().GetIndexAndSequenceNumber();
            RigidBodyComponent.LastBodyPosition     = Results[i].LastBodyPosition;
            RigidBodyComponent.LastBodyRotation     = Results[i].LastBodyRotation;

            StoreBodyMaterial(Body->GetID(), Results[i]);

            BodyIDsToAdd.push_back(Body->GetID());
        }

        if (!BodyIDsToAdd.empty())
        {
            JPH::BodyInterface::AddState AddState = BodyInterface.AddBodiesPrepare(BodyIDsToAdd.data(), (int)BodyIDsToAdd.size());
            BodyInterface.AddBodiesFinalize(BodyIDsToAdd.data(), (int)BodyIDsToAdd.size(), AddState, JPH::EActivation::Activate);
        }
    }

    void FJoltPhysicsScene::OnRigidBodyComponentDestroyed(entt::registry& Registry, entt::entity Entity)
    {
        // Clear the transform's has-body hint unless a character body still remains on the entity.
        if (STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity))
        {
            Transform->SetHasPhysicsBody(Registry.any_of<SCharacterPhysicsComponent>(Entity));
        }

        DropRenderTransform(Registry, Entity);

        SRigidBodyComponent& RigidBodyComponent = Registry.get<SRigidBodyComponent>(Entity);
        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID(RigidBodyComponent.BodyID);
        
        if (BodyID.IsInvalid())
        {
            return;
        }

        ClearBodyMaterial(BodyID);

        BodyInterface.RemoveBody(BodyID);
        BodyInterface.DestroyBody(BodyID);
    }

    void FJoltPhysicsScene::OnColliderComponentAdded(entt::registry& Registry, entt::entity Entity)
    {
    }

    void FJoltPhysicsScene::OnColliderComponentRemoved(entt::registry& Registry, entt::entity Entity)
    {
    }

    void FJoltPhysicsScene::OnImpulseEvent(const SImpulseEvent& Impulse)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Impulse.BodyID);
        JPH::RVec3 VecImpulse = JoltUtils::ToJPHRVec3(Impulse.Impulse);
        
        if (VecImpulse.IsNaN() || VecImpulse.IsNearZero())
        {
            return;
        }

        Interface.AddImpulse(BodyID, VecImpulse);
    }
    
    void FJoltPhysicsScene::OnForceEvent(const SForceEvent& Force)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Force.BodyID);
        
        Interface.AddForce(BodyID, JoltUtils::ToJPHRVec3(Force.Force));
    }
    
    void FJoltPhysicsScene::OnTorqueEvent(const STorqueEvent& Torque)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Torque.BodyID);
        
        Interface.AddTorque(BodyID, JoltUtils::ToJPHRVec3(Torque.Torque));
    }
    
    void FJoltPhysicsScene::OnAngularImpulseEvent(const SAngularImpulseEvent& AngularImpulse)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(AngularImpulse.BodyID);
        
        Interface.AddAngularImpulse(BodyID, JoltUtils::ToJPHRVec3(AngularImpulse.AngularImpulse));
    }
    
    void FJoltPhysicsScene::OnSetVelocityEvent(const SSetVelocityEvent& Velocity)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Velocity.BodyID);
        
        Interface.SetLinearVelocity(BodyID, JoltUtils::ToJPHRVec3(Velocity.Velocity));
    }
    
    void FJoltPhysicsScene::OnSetAngularVelocityEvent(const SSetAngularVelocityEvent& AngularVelocity)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(AngularVelocity.BodyID);
        
        Interface.SetAngularVelocity(BodyID, JoltUtils::ToJPHRVec3(AngularVelocity.AngularVelocity));
    }
    
    void FJoltPhysicsScene::OnAddImpulseAtPositionEvent(const SAddImpulseAtPositionEvent& Event)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Event.BodyID);
        
        Interface.AddImpulse(BodyID, JoltUtils::ToJPHRVec3(Event.Impulse), JoltUtils::ToJPHRVec3(Event.Position));
    }
    
    void FJoltPhysicsScene::OnAddForceAtPositionEvent(const SAddForceAtPositionEvent& Event)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Event.BodyID);
        
        Interface.AddForce(BodyID, JoltUtils::ToJPHRVec3(Event.Force), JoltUtils::ToJPHRVec3(Event.Position));
    }
    
    void FJoltPhysicsScene::OnSetGravityFactorEvent(const SSetGravityFactorEvent& Event)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID BodyID = JPH::BodyID(Event.BodyID);

        Interface.SetGravityFactor(BodyID, Event.GravityFactor);
    }

    void FJoltPhysicsScene::ApplyBuoyancyImpulse(entt::entity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
        float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime)
    {
        if (DeltaTime <= 0.0f)
        {
            return;
        }

        const uint32 Raw = GetEntityBodyID(Entity);
        if (Raw == 0xFFFFFFFFu)
        {
            return;
        }

        // Jolt computes the submerged volume from the body's actual shape against the surface plane, so this
        // replaces the old 4-point sampling with shape-accurate buoyancy + self-righting. Gravity is the
        // system's own vector. The surface normal must be unit-length; guard against a degenerate input.
        const FVector3 Normal = Math::LengthSquared(SurfaceNormal) > LE_SMALL_NUMBER
            ? Math::Normalize(SurfaceNormal) : FVector3(0.0f, 1.0f, 0.0f);

        JPH::BodyInterface& BodyInterface = JoltSystem->GetBodyInterface();
        BodyInterface.ApplyBuoyancyImpulse(
            JPH::BodyID(Raw),
            JoltUtils::ToJPHRVec3(FDoubleVector3(SurfacePosition)),
            JoltUtils::ToJPHVec3(Normal),
            Buoyancy,
            LinearDrag,
            AngularDrag,
            JoltUtils::ToJPHVec3(FluidVelocity),
            JoltSystem->GetGravity(),
            DeltaTime);
    }

    FVector3 FJoltPhysicsScene::GetVelocityAtPoint(uint32 BodyID, const FVector3& Point)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID JPHBodyID = JPH::BodyID(BodyID);
        
        JPH::Vec3 PointVelocity = Interface.GetPointVelocity(JPHBodyID, JoltUtils::ToJPHRVec3(Point));
        
        return JoltUtils::FromJPHVec3(PointVelocity);
    }

    FVector3 FJoltPhysicsScene::GetLinearVelocity(uint32 BodyID)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID JPHBodyID = JPH::BodyID(BodyID);
        
        return JoltUtils::FromJPHVec3(Interface.GetLinearVelocity(JPHBodyID));
    }

    FVector3 FJoltPhysicsScene::GetAngularVelocity(uint32 BodyID)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID JPHBodyID = JPH::BodyID(BodyID);
        
        return JoltUtils::FromJPHVec3(Interface.GetAngularVelocity(JPHBodyID));
    }

    FVector3 FJoltPhysicsScene::GetCenterOfMass(uint32 BodyID)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        JPH::BodyID JPHBodyID = JPH::BodyID(BodyID);

        return JoltUtils::FromJPHVec3(Interface.GetCenterOfMassPosition(JPHBodyID));
    }

    FVector3 FJoltPhysicsScene::GetBodyPosition(uint32 BodyID)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        return JoltUtils::FromJPHVec3(Interface.GetPosition(JPH::BodyID(BodyID)));
    }

    FQuat FJoltPhysicsScene::GetBodyRotation(uint32 BodyID)
    {
        JPH::BodyInterface& Interface = JoltSystem->GetBodyInterface();
        return JoltUtils::FromJPHQuat(Interface.GetRotation(JPH::BodyID(BodyID)));
    }

    uint32 FJoltPhysicsScene::GetBodyCount()
    {
        return (uint32)JoltSystem->GetNumBodies();
    }

    uint32 FJoltPhysicsScene::GetMaxBodyCount()
    {
        return (uint32)JoltSystem->GetMaxBodies();
    }
}
