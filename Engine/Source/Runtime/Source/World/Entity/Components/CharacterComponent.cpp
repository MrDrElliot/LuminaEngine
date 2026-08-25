#include "RuntimePCH.h"
#include "CharacterComponent.h"

// Included only so the handle is a complete type where these special members are defined.
#include "Physics/API/Box3D/Box3DCharacterHandle.h"

namespace Lumina
{
    SCharacterPhysicsComponent::SCharacterPhysicsComponent() = default;
    SCharacterPhysicsComponent::~SCharacterPhysicsComponent() = default;
    SCharacterPhysicsComponent::SCharacterPhysicsComponent(const SCharacterPhysicsComponent&) = default;
    SCharacterPhysicsComponent& SCharacterPhysicsComponent::operator=(const SCharacterPhysicsComponent&) = default;
    SCharacterPhysicsComponent::SCharacterPhysicsComponent(SCharacterPhysicsComponent&&) noexcept = default;
    SCharacterPhysicsComponent& SCharacterPhysicsComponent::operator=(SCharacterPhysicsComponent&&) noexcept = default;

    uint32 SCharacterPhysicsComponent::GetBodyID() const
    {
        if (Character == nullptr)
        {
            return 0xFFFFFFFF;
        }
        return Character->ProxyBodyHandle;
    }
}
