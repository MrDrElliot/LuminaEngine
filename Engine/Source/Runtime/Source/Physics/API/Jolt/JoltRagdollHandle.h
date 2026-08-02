#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include "Platform/GenericPlatform.h"
#include "Containers/Array.h"
#include "Core/Math/Matrix/MatrixMath.h"

namespace Lumina
{
    struct FJoltRagdollHandle
    {
        JPH::Ref<JPH::RagdollSettings>  Settings;
        JPH::Ref<JPH::Ragdoll>          Ragdoll;

        TVector<int32>                  JointToBone;

        bool bAddedToScene = false;
        
        ~FJoltRagdollHandle()
        {
            if (Ragdoll != nullptr && bAddedToScene)
            {
                Ragdoll->RemoveFromPhysicsSystem();
                bAddedToScene = false;
            }
        }

        FJoltRagdollHandle() = default;
        FJoltRagdollHandle(const FJoltRagdollHandle&) = delete;
        FJoltRagdollHandle& operator=(const FJoltRagdollHandle&) = delete;
    };
}
