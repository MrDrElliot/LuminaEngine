#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Physics/PhysicsScene.h"

namespace Lumina
{
    struct FFakeCharacterScene final : Physics::IPhysicsScene
    {
        static constexpr float DefaultMoveSpeed = 4.0f;

        struct FSimulateCall
        {
            ECS::FEntity                    Entity;
            Physics::FCharacterMoveInput    Input;
            bool                            bReplay = false;
        };

        THashMap<ECS::FEntity, Physics::FCharacterNetState> States;
        TVector<FSimulateCall>                              SimulateCalls;
        uint32                                              SetStateCalls = 0;

        bool GetCharacterNetState(ECS::FEntity Entity, Physics::FCharacterNetState& Out) const override
        {
            const Physics::FCharacterNetState* Found = States.Find(Entity);
            if (Found == nullptr)
            {
                return false;
            }
            Out = *Found;
            return true;
        }

        bool SetCharacterNetState(ECS::FEntity Entity, const Physics::FCharacterNetState& State) override
        {
            States[Entity] = State;
            ++SetStateCalls;
            return true;
        }

        bool SimulateCharacterStep(ECS::FEntity Entity, const Physics::FCharacterMoveInput& Input, float FixedDt, bool bReplay) override
        {
            const float Speed = Input.MoveSpeed != 0.0f ? Input.MoveSpeed : DefaultMoveSpeed;
            Physics::FCharacterNetState& State = States[Entity];
            State.Velocity = Input.Direction * Input.Throttle * Speed;
            State.Position += State.Velocity * FixedDt;
            SimulateCalls.Add(FSimulateCall{ Entity, Input, bReplay });
            return true;
        }

        void DeactivateBody(ECS::FEntity) override {}
        void ActivateBody(ECS::FEntity) override {}
        void ChangeBodyMotionType(ECS::FEntity, EBodyType) override {}
        Physics::EPhysicsBodyStatus GetBodyStatus(ECS::FEntity) const override { return Physics::EPhysicsBodyStatus::Missing; }
        bool TryGetBodyState(ECS::FEntity, Physics::FPhysicsBodyState&) const override { return false; }
        bool TryGetTargetState(const SPhysicsBodyTarget&, Physics::FPhysicsBodyState&) const override { return false; }
        void AddForceAtTarget(const SPhysicsBodyTarget&, const FVector3&, const FVector3&) override {}
        void AddImpulseAtTarget(const SPhysicsBodyTarget&, const FVector3&, const FVector3&) override {}
        void SetTargetLinearVelocity(const SPhysicsBodyTarget&, const FVector3&) override {}
        void SetTargetAngularVelocity(const SPhysicsBodyTarget&, const FVector3&) override {}
        TOptional<SRayResult> CastRay(const SRayCastSettings&) override { return {}; }
        void CastRayAll(const SRayCastSettings&, TVector<SRayResult>& OutHits) override { OutHits.Reset(); }
        bool CastRayAny(const SRayCastSettings&) override { return false; }
        TOptional<SRayResult> CastRayAgainst(ECS::FEntity, const FVector3&, const FVector3&) override { return {}; }
        void CastShapeAll(const SQueryShape&, const FVector3&, const FVector3&, ECollisionProfiles, TSpan<const ECS::FEntity>, TVector<SRayResult>& OutHits) override { OutHits.Reset(); }
        TOptional<SRayResult> CastShapeClosest(const SQueryShape&, const FVector3&, const FVector3&, ECollisionProfiles, TSpan<const ECS::FEntity>) override { return {}; }
        int32 OverlapShape(const SQueryShape&, const FVector3&, ECollisionProfiles, TSpan<const ECS::FEntity>, TSpan<ECS::FEntity>) override { return 0; }
        bool GetClosestPointOnBody(ECS::FEntity, const FVector3&, FVector3&) const override { return false; }
        bool GetBodyBounds(ECS::FEntity, SAABB&) const override { return false; }
        void AddForce(ECS::FEntity, const FVector3&) override {}
        void AddImpulse(ECS::FEntity, const FVector3&) override {}
        void AddTorque(ECS::FEntity, const FVector3&) override {}
        void AddAngularImpulse(ECS::FEntity, const FVector3&) override {}
        void AddForceAtPosition(ECS::FEntity, const FVector3&, const FVector3&) override {}
        void AddImpulseAtPosition(ECS::FEntity, const FVector3&, const FVector3&) override {}
        void SetLinearVelocity(ECS::FEntity, const FVector3&) override {}
        void SetAngularVelocity(ECS::FEntity, const FVector3&) override {}
        void SetGravityFactor(ECS::FEntity, float) override {}
        FVector3 GetVelocityAtPoint(ECS::FEntity, const FVector3&) const override { return FVector3(0.0f); }
        FVector3 GetLinearVelocity(ECS::FEntity) const override { return FVector3(0.0f); }
        FVector3 GetAngularVelocity(ECS::FEntity) const override { return FVector3(0.0f); }
        FVector3 GetCenterOfMass(ECS::FEntity) const override { return FVector3(0.0f); }
        FVector3 GetBodyPosition(ECS::FEntity) const override { return FVector3(0.0f); }
        FQuat GetBodyRotation(ECS::FEntity) const override { return FQuat::Identity(); }
        uint32 GetBodyCount() override { return 0; }
        uint32 GetMaxBodyCount() override { return 0; }
        uint32 GetAwakeBodyCount() override { return 0; }
        bool IsRagdollReady(const FPhysicsRagdollHandle&) const override { return false; }
        void ReadRagdollPose(const FPhysicsRagdollHandle&, const FMatrix4&, const FSkeletonResource*, TVector<FMatrix4>&) override {}
        SPhysicsBodyTarget GetRagdollBoneTarget(ECS::FEntity, FName) const override { return {}; }
        void GetRagdollRootTransform(const FPhysicsRagdollHandle&, FVector3&, FQuat&) override {}

        void PreUpdate() override {}
        void Update(double) override {}
        void PostUpdate() override {}
        void Simulate() override {}
        void StopSimulate() override {}
        TSharedPtr<FPhysicsRagdollHandle> CreateRagdoll(const Physics::FRagdollDesc&) override { return nullptr; }
        void DestroyRagdoll(const TSharedPtr<FPhysicsRagdollHandle>&) override {}
        uint32 AllocateRagdollGroupID() override { return 0; }
    };
}
