#include "RuntimePCH.h"
#include "CharacterComponent.h"

// Included here only so TUniquePtr<FJoltCharacterHandle> has a complete type for its destructor;
// keeps CharacterComponent.h Jolt-free.
#include "Physics/API/Jolt/JoltCharacterHandle.h"

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
        if (Character == nullptr || Character->Ref == nullptr)
        {
            return 0xFFFFFFFF;
        }
        return Character->Ref->GetInnerBodyID().GetIndexAndSequenceNumber();
    }
}
