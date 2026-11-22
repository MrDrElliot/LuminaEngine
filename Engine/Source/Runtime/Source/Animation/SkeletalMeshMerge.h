#pragma once

#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::SkeletalMeshMerge
{
    struct FSettings
    {
        // Whose hierarchy and bind pose win where two skeletons name the same bone; null takes the first.
        CSkeleton* BaseSkeleton = nullptr;

        // Names the merged mesh; its skeleton takes the same name with a "_Skeleton" suffix.
        FName Name;

        // Union the inputs' skeleton sockets onto the merged skeleton and their mesh sockets onto the merged mesh, first definition winning.
        bool bMergeSockets = true;
    };

    struct FResult
    {
        // Shared by every caller that merged the same inputs while any of them still holds it.
        TStrongObjectPtr<CSkeletalMesh> Mesh;
        TStrongObjectPtr<CSkeleton>     Skeleton;

        // Why the merge produced nothing; empty on success.
        FString Error;

        bool IsValid() const { return Mesh.Get() != nullptr; }
    };

    // Bones across every input matched BY NAME; OutBoneRemap[s][SourceBone] is that bone's merged index.
    RUNTIME_API bool BuildUnifiedSkeleton(TSpan<CSkeleton* const> Skeletons,
                                          CSkeleton*              Base,
                                          FSkeletonResource&      Out,
                                          TVector<TVector<int32>>& OutBoneRemap,
                                          FString&                OutError);

    // Identical inputs and settings return the mesh an earlier call built, so callers must not modify it.
    RUNTIME_API FResult Merge(TSpan<CSkeletalMesh* const> Meshes, const FSettings& Settings = FSettings());
}
