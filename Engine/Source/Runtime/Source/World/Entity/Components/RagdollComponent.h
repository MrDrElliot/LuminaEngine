#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "RagdollComponent.generated.h"

namespace Lumina
{
    class CPhysicsAsset;
    struct FJoltRagdollHandle;

    /** Drives how the ragdoll system treats the skeletal mesh. Phase 1: Inactive (animation-driven) or
     *  Simulated (full physics writes the pose). Kinematic/Blending arrive in later phases. */
    REFLECT()
    enum class RUNTIME_API ERagdollState : uint8
    {
        Inactive,    // No bodies in the scene; pose stays fully animation-driven.
        Simulated,   // Full physics; the ragdoll bodies drive the bone pose.
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SRagdollComponent
    {
        GENERATED_BODY()

        SRagdollComponent();
        ~SRagdollComponent();
        SRagdollComponent(const SRagdollComponent&);
        SRagdollComponent& operator=(const SRagdollComponent&);
        SRagdollComponent(SRagdollComponent&&) noexcept;
        SRagdollComponent& operator=(SRagdollComponent&&) noexcept;

        /** Authored bodies/constraints. If null, the system auto-generates one from the mesh's skeleton. */
        PROPERTY(Editable, Category = "Ragdoll")
        TObjectPtr<CPhysicsAsset> PhysicsAsset;

        /** Flip to Simulated to collapse into physics; the system creates/destroys bodies to match. */
        PROPERTY(Script, Editable, Category = "Ragdoll")
        ERagdollState State = ERagdollState::Inactive;

        /** While simulating, move the entity transform to follow the ragdoll's root body each frame. Keep
         *  on so the mesh's culling bounds track the ragdoll; off leaves the entity at its spawn transform. */
        PROPERTY(Script, Editable, Category = "Ragdoll")
        bool bDriveEntityFromRoot = true;

        /** Live Jolt ragdoll, created on demand by the ragdoll system. */
        TSharedPtr<FJoltRagdollHandle> Ragdoll;

        /** State the system last realized; lets it detect Inactive<->Simulated transitions. */
        ERagdollState RealizedState = ERagdollState::Inactive;
    };
}
