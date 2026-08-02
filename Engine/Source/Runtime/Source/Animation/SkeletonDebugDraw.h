#pragma once

#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class CWorld;
    class IPrimitiveDrawInterface;
    struct FSkeletonResource;

    // Stateless skeleton visualization free functions; shared by every tool with a draw interface + skeleton.
    namespace SkeletonDebugDraw
    {
        // Plain options bag; pass by value/const-ref. No behavior, just knobs.
        struct FOptions
        {
            bool      bBones        = true;  // parent->child bone shapes
            bool      bJoints       = true;  // a small sphere at each bone origin
            bool      bAxes         = false; // per-bone local X/Y/Z triad
            bool      bOctahedral   = true;  // draw tapered octahedral bones (vs. plain lines)
            bool      bDepthTest    = false; // false = x-ray (always visible)
            float     JointRadius   = 0.014f;
            float     AxisLength    = 0.06f;
            float     BoneThickness = 2.4f;
            // Warm, saturated palette: reads clearly over both dark and lit viewports,
            // and deliberately avoids white (which washes out against bright geometry).
            FVector4 BoneColor    = { 1.00f, 0.66f, 0.22f, 1.0f }; // amber
            FVector4 RootColor    = { 0.30f, 0.92f, 0.70f, 1.0f }; // teal-green (roots stand out)
            FVector4 JointColor   = { 1.00f, 0.82f, 0.40f, 1.0f }; // bright gold
        };

        // A bone's name paired with its world-space origin, for screen-space labelling.
        struct FBoneLabel
        {
            FName     Name;
            FVector3 WorldPosition;
        };

        // Model-space global transform per bone: from a live pose recovers Global = Skinning * inverse(InvBind),
        // else falls back to the bind pose via FK.
        RUNTIME_API void ComputeGlobalBoneTransforms(const FSkeletonResource* Skeleton,
                                                     const TVector<FMatrix4>& BoneTransforms,
                                                     TVector<FMatrix4>& OutGlobals);

        /** Draw one skeleton's bones/joints/axes. GlobalBoneTransforms is model-space (see above). */
        RUNTIME_API void DrawSkeleton(IPrimitiveDrawInterface* DrawInterface,
                                      const FSkeletonResource* Skeleton,
                                      const TVector<FMatrix4>& GlobalBoneTransforms,
                                      const FMatrix4& MeshWorldMatrix,
                                      const FOptions& Options);

        /** Draw every skeletal mesh in the world (bone lines/joints/axes only; no names). */
        RUNTIME_API void DrawWorldSkeletons(CWorld* World, IPrimitiveDrawInterface* DrawInterface, const FOptions& Options);

        /** Collect world-space bone label positions for every skeletal mesh in the world. */
        RUNTIME_API void GatherWorldBoneLabels(CWorld* World, TVector<FBoneLabel>& OutLabels);
    }
}
