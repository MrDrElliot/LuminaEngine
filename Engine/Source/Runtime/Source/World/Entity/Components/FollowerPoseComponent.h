#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "FollowerPoseComponent.generated.h"

namespace Lumina
{
    // Drives this mesh from another entity's pose, so a modular character costs one evaluation.
    REFLECT(Component, Category = "Animation")
    struct RUNTIME_API SFollowerPoseComponent
    {
        GENERATED_BODY()

        static constexpr uint32 NoLeader = 0xFFFFFFFFu;

        /** Entity whose pose this mesh copies. It must carry a skeletal mesh that something animates. */
        PROPERTY(Editable, Entity, Category = "Follower Pose")
        uint32 Leader = NoLeader;

        /** Bones the leader has no match for keep their bind pose rather than following anything. */
        PROPERTY(Editable, Category = "Follower Pose")
        bool bWarnOnMissingBones = true;

        // Follower bone to leader bone, by name. INDEX_NONE where the leader has no such bone. Transient.
        TVector<int32> BoneMap;

        // Leader bind times follower inverse bind, which is what lands a copy between two skeletons.
        TVector<FMatrix4> BindFixups;

        // Skeletons the map was built against; a change on either side rebuilds it. Transient.
        const void* MappedLeaderSkeleton = nullptr;
        const void* MappedFollowerSkeleton = nullptr;

        // Skips the copy while the leader's pose has not moved. Transient.
        uint32 LastLeaderPoseSerial = 0;

        // Set once the map came back empty, so the warning fires once rather than every frame. Transient.
        bool bWarnedNoMatch = false;
    };
}
