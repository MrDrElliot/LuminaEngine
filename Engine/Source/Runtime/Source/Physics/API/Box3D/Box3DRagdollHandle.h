#pragma once

#include <box3d/box3d.h>
#include "Containers/Vector.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Box3D has no ragdoll object, so a ragdoll is just the bodies and joints the scene built for it.
    struct FPhysicsRagdollHandle
    {
        b3WorldId           WorldId{};
        TVector<b3BodyId>   Bodies;
        TVector<uint32>     BodyHandles;
        TVector<b3JointId>  Joints;

        // Bind-pose body transform per part, used to rebuild the skinning matrix from the simulated pose.
        TVector<FMatrix4>   PartBindInverse;
        TVector<int32>      JointToBone;

        bool bAddedToScene = false;

        FPhysicsRagdollHandle() = default;
        FPhysicsRagdollHandle(const FPhysicsRagdollHandle&) = delete;
        FPhysicsRagdollHandle& operator=(const FPhysicsRagdollHandle&) = delete;

        ~FPhysicsRagdollHandle()
        {
            Release();
        }

        void Release()
        {
            if (!bAddedToScene || !b3World_IsValid(WorldId))
            {
                bAddedToScene = false;
                return;
            }

            for (b3JointId Joint : Joints)
            {
                if (b3Joint_IsValid(Joint))
                {
                    b3DestroyJoint(Joint, false);
                }
            }

            for (b3BodyId Body : Bodies)
            {
                if (b3Body_IsValid(Body))
                {
                    b3DestroyBody(Body);
                }
            }

            Joints.clear();
            Bodies.clear();
            bAddedToScene = false;
        }
    };
}
