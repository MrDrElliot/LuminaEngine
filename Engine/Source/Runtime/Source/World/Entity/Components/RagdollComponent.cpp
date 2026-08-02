#include "RuntimePCH.h"
#include "RagdollComponent.h"

// Included here only so TSharedPtr<FJoltRagdollHandle> has a complete type for its special members;
#include "Physics/API/Jolt/JoltRagdollHandle.h"

namespace Lumina
{
    SRagdollComponent::SRagdollComponent() = default;
    SRagdollComponent::~SRagdollComponent() = default;
    SRagdollComponent::SRagdollComponent(const SRagdollComponent&) = default;
    SRagdollComponent& SRagdollComponent::operator=(const SRagdollComponent&) = default;
    SRagdollComponent::SRagdollComponent(SRagdollComponent&&) noexcept = default;
    SRagdollComponent& SRagdollComponent::operator=(SRagdollComponent&&) noexcept = default;
}
