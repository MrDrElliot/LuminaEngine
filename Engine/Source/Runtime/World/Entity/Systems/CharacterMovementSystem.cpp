#include "pch.h"
#include "CharacterMovementSystem.h"

#include <glm/gtx/quaternion.hpp>

#include "Physics/API/Jolt/JoltPhysics.h"
#include "Physics/API/Jolt/JoltPhysicsScene.h"
#include "Physics/API/Jolt/JoltUtils.h"
#include "Renderer/RendererUtils.h"
#include "world/entity/components/charactercomponent.h"
#include "world/entity/components/charactercontrollercomponent.h"

namespace Lumina
{
    void SCharacterMovementSystem::Update(const FSystemContext& SystemContext) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        
        using namespace Physics;
        
        auto View = SystemContext.CreateView<SCharacterControllerComponent, SCharacterPhysicsComponent, SCharacterMovementComponent>();
        float DeltaTime = static_cast<float>(SystemContext.GetDeltaTime());
        auto PhysicsScene = SystemContext.GetRegistryContext().get<IPhysicsScene*>();
        FJoltPhysicsScene* JoltScene = static_cast<FJoltPhysicsScene*>(PhysicsScene);
        JPH::PhysicsSystem* JPHSystem = JoltScene->GetPhysicsSystem();
        
        
        auto Handle = View.handle();
        if (Handle->empty())
        {
            return;
        }
        
        Task::ParallelFor(Handle->size(), [&](uint32 Index)
        {
            LUMINA_PROFILE_SECTION("Process Character Movement");
            
            entt::entity Entity = (*Handle)[Index];

            if (!View.contains(Entity))
            {
                return;
            }
            
            SCharacterControllerComponent& Controller   = View.get<SCharacterControllerComponent>(Entity);
            const SCharacterPhysicsComponent& Physics   = View.get<SCharacterPhysicsComponent>(Entity);
            SCharacterMovementComponent& Movement       = View.get<SCharacterMovementComponent>(Entity);
            
            JPH::CharacterVirtual* Character            = Physics.Character;
            if (Character == nullptr)
            {
                return;
            }

            JPH::CharacterVirtual::EGroundState GroundState = Character->GetGroundState();
            bool bWasGrounded = Movement.bGrounded;
            Controller.bIsOnGround = bWasGrounded;
            Movement.bGrounded = (GroundState == JPH::CharacterVirtual::EGroundState::OnGround);
        
            if (!bWasGrounded && Movement.bGrounded)
            {
                Movement.JumpCount = 0;
            }
        
            glm::vec3 DesiredDirection(0.0f);
            bool bHasMovementInput = false;
            
            if (glm::length2(Controller.MoveInput) > LE_SMALL_NUMBER)
            {
                bHasMovementInput = true;
    
                glm::vec3 Forward   = RenderUtils::GetForwardVector(Controller.LookInput.x, Controller.LookInput.y);
                glm::vec3 Right     = RenderUtils::GetRightVector(Controller.LookInput.x);
                glm::vec3 Up        = glm::cross(Right, Forward);
    
                DesiredDirection = Right * Controller.MoveInput.x + Up * Controller.MoveInput.y + Forward * Controller.MoveInput.z;
                
                if (glm::length2(DesiredDirection) > LE_SMALL_NUMBER)
                {
                    DesiredDirection = glm::normalize(DesiredDirection);
                }
    
                Controller.MoveInput = {};
            }
        
            float TargetSpeed           = bHasMovementInput ? Movement.MoveSpeed : 0.0f;
            glm::vec3 TargetVelocity    = DesiredDirection * TargetSpeed;
            
            glm::quat TargetRotation = JoltUtils::FromJPHQuat(Character->GetRotation());
            if (Movement.bUseControllerRotation)
            {
                TargetRotation = glm::quat(glm::vec3(0.0f, glm::radians(Controller.LookInput.x), 0.0f));
            }
            else if (Movement.bOrientRotationToMovement && bHasMovementInput)
            {
                float TargetYaw     = glm::atan(DesiredDirection.x, DesiredDirection.z);
                glm::quat Rotation  = glm::quat(glm::vec3(0.0f, TargetYaw, 0.0f));
                TargetRotation      = glm::slerp(TargetRotation, Rotation, Movement.RotationRate * DeltaTime);
            }
        
            glm::vec3 HorizontalVelocity(Movement.Velocity.x, 0.0f, Movement.Velocity.z);
            float CurrentSpeed = glm::length(HorizontalVelocity);
        
            if (bHasMovementInput)
            {
                HorizontalVelocity = glm::mix(HorizontalVelocity, TargetVelocity, Movement.Acceleration * DeltaTime);
            }
            else if (Movement.bGrounded)
            {
                float DecelerationAmount    = Movement.Deceleration * DeltaTime;
                float NewSpeed              = glm::max(0.0f, CurrentSpeed - DecelerationAmount);
                
                if (CurrentSpeed > 0.001f)
                {
                    HorizontalVelocity = glm::normalize(HorizontalVelocity) * NewSpeed;
                }
                else
                {
                    HorizontalVelocity = glm::vec3(0.0f);
                }
                
                float Friction      = glm::max(0.0f, 1.0f - Movement.GroundFriction * DeltaTime);
                HorizontalVelocity  *= Friction;
            }
            else
            {
                float AirFriction   = glm::max(0.0f, 1.0f - (Movement.GroundFriction * 0.1f) * DeltaTime);
                HorizontalVelocity  *= AirFriction;
            }
        
            Movement.Velocity.x = HorizontalVelocity.x;
            Movement.Velocity.z = HorizontalVelocity.z;
        
            if (Controller.bJumpPressed)
            {
                Controller.bJumpPressed = false;
                
                if (Movement.JumpCount != Movement.MaxJumpCount)
                {
                    Movement.Velocity.y = Movement.JumpSpeed;
                    Movement.JumpCount++;
                }
            }
        
            if (Movement.bGrounded)
            {
                JPH::Vec3 GroundVelocity    = Character->GetGroundVelocity();
                Movement.Velocity.x         += GroundVelocity.GetX();
                Movement.Velocity.z         += GroundVelocity.GetZ();
            }
            else
            {
                Movement.Velocity.y += Movement.Gravity * DeltaTime;
            }
        
            Character->SetRotation(JoltUtils::ToJPHQuat(TargetRotation));
            Character->SetLinearVelocity(JoltUtils::ToJPHRVec3(Movement.Velocity));
        
            JPH::CharacterVirtual::ExtendedUpdateSettings UpdateSettings;
            UpdateSettings.mStickToFloorStepDown    = JPH::Vec3(0.0f, -0.5f, 0.0f);
            UpdateSettings.mWalkStairsStepUp        = JPH::Vec3(0.0f, Physics.StepHeight, 0.0f);
        
            JPH::TempAllocatorImpl Allocator(1024ull * 1024);
            Character->ExtendedUpdate(DeltaTime,
                JPH::Vec3(0.0f, Movement.Gravity, 0.0f),
                UpdateSettings,
                JPHSystem->GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
                JPHSystem->GetDefaultLayerFilter(Layers::MOVING),
                {},
                {},
                Allocator);
        
            JPH::Vec3 ActualVelocity    = Character->GetLinearVelocity();
            Movement.Velocity           = JoltUtils::FromJPHVec3(ActualVelocity);
        });
    }
}
