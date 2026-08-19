#pragma once

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CSkeletalMesh;
    class CSkeleton;
    struct FSkeletonResource;
}

namespace Lumina::SkeletalMeshMerge
{
    struct FSettings
    {
        // Whose hierarchy and bind pose win where two skeletons name the same bone; null takes the first.
        CSkeleton* BaseSkeleton = nullptr;

        // Names the merged mesh; its skeleton takes the same name with a "_Skeleton" suffix.
        FName Name;

        // Union every input skeleton's sockets onto the merged one, first definition winning.
        bool bMergeSockets = true;
    };

    struct FResult
    {
        CSkeletalMesh* Mesh     = nullptr;
        CSkeleton*     Skeleton = nullptr;

        // Why the merge produced nothing; empty on success.
        FString Error;

        bool IsValid() const { return Mesh != nullptr; }
    };

    // Bones across every input matched BY NAME; OutBoneRemap[s][SourceBone] is that bone's merged index.
    RUNTIME_API bool BuildUnifiedSkeleton(TSpan<CSkeleton* const> Skeletons,
                                          CSkeleton*              Base,
                                          FSkeletonResource&      Out,
                                          TVector<TVector<int32>>& OutBoneRemap,
                                          FString&                OutError);

    // Every input's geometry on one unified skeleton. Inputs are untouched; the result has no distance field.
    RUNTIME_API FResult Merge(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings = FSettings());
}
