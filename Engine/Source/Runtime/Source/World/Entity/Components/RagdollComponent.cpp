#include "RuntimePCH.h"
#include "RagdollComponent.h"

// Included here only so TSharedPtr<FPhysicsRagdollHandle> has a complete type for its special members;
#include "Physics/API/Box3D/Box3DRagdollHandle.h"

namespace Lumina
{
    SRagdollComponent::SRagdollComponent() = default;
    SRagdollComponent::~SRagdollComponent() = default;
    SRagdollComponent::SRagdollComponent(const SRagdollComponent&) = default;
    SRagdollComponent& SRagdollComponent::operator=(const SRagdollComponent&) = default;
    SRagdollComponent::SRagdollComponent(SRagdollComponent&&) noexcept = default;
    SRagdollComponent& SRagdollComponent::operator=(SRagdollComponent&&) noexcept = default;
}
