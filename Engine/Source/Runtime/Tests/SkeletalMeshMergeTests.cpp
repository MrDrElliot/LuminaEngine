#include "gtest/gtest.h"

#include "Animation/SkeletalMeshMerge.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"

using namespace Lumina;

namespace
{
    TUniquePtr<FSkeletonResource> MakeOneBoneRig()
    {
        TUniquePtr<FSkeletonResource> Rig = MakeUnique<FSkeletonResource>();
        FSkeletonResource::FBoneInfo Bone;
        Bone.Name           = FName("Root");
        Bone.ParentIndex    = Constants::kIndexNone;
        Bone.InvBindMatrix  = FMatrix4::Identity();
        Bone.LocalTransform = FMatrix4::Identity();
        Rig->BoneNameToIndex[Bone.Name] = Rig->Bones.Num();
        Rig->Bones.Add(Bone);
        Rig->BuildBindPoseCache();
        return Rig;
    }

    CSkeleton* MakeOneBoneSkeleton()
    {
        CSkeleton* Skeleton = NewObject<CSkeleton>();
        Skeleton->SetSkeletonResource(MakeOneBoneRig());
        return Skeleton;
    }

    // One skinned meshlet with no vertices or triangles, just enough shape for HasMergeableGeometry to pass.
    TUniquePtr<FMeshResource> MakeMergeableSkinnedResource()
    {
        TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
        Resource->bSkinnedMesh = true;

        Resource->MeshletData.Meshlets.AddDefaulted();
        Resource->MeshletData.MeshletSpheres.AddDefaulted();
        Resource->MeshletData.MeshletCones.AddDefaulted();

        FGeometrySurface& Surface  = Resource->GeometrySurfaces.EmplaceGetRef();
        Surface.NumLODs            = 1;
        Surface.LODMeshletOffset[0] = 0;
        Surface.LODMeshletCount[0]  = 1;
        return Resource;
    }

    CSkeletalMesh* MakeMergeableSkeletalMesh(CSkeleton* Skeleton)
    {
        CSkeletalMesh* Mesh = NewObject<CSkeletalMesh>();
        Mesh->Skeleton = Skeleton;
        Mesh->SetMeshResource(MakeMergeableSkinnedResource());
        return Mesh;
    }
}

TEST(SkeletalMeshMerge, MeshOrSkeletonReimportInvalidatesTheMergeCache)
{
    CSkeleton*     Skeleton = MakeOneBoneSkeleton();
    CSkeletalMesh* Mesh     = MakeMergeableSkeletalMesh(Skeleton);

    CSkeletalMesh* const Meshes[] = { Mesh };
    const TSpan<CSkeletalMesh* const> Span(Meshes, 1);

    const SkeletalMeshMerge::FResult First = SkeletalMeshMerge::Merge(Span);
    ASSERT_TRUE(First.IsValid());

    const SkeletalMeshMerge::FResult Cached = SkeletalMeshMerge::Merge(Span);
    ASSERT_TRUE(Cached.IsValid());
    EXPECT_EQ(Cached.Mesh.Get(), First.Mesh.Get());

    // A reimport replaces the geometry under the same CSkeletalMesh*, bumping its geometry version.
    Mesh->SetMeshResource(MakeMergeableSkinnedResource());

    const SkeletalMeshMerge::FResult AfterReimport = SkeletalMeshMerge::Merge(Span);
    ASSERT_TRUE(AfterReimport.IsValid());
    EXPECT_NE(AfterReimport.Mesh.Get(), First.Mesh.Get());

    // A fresh rig lands on the same bind pose generation as the old one, so only the skeleton's own counter tells them apart.
    Skeleton->SetSkeletonResource(MakeOneBoneRig());

    const SkeletalMeshMerge::FResult AfterSkeletonReimport = SkeletalMeshMerge::Merge(Span);
    ASSERT_TRUE(AfterSkeletonReimport.IsValid());
    EXPECT_NE(AfterSkeletonReimport.Mesh.Get(), AfterReimport.Mesh.Get());
}

TEST(SkeletalMeshMerge, BaseSkeletonEditInvalidatesTheMergeCache)
{
    CSkeleton*     MeshSkeleton = MakeOneBoneSkeleton();
    CSkeletalMesh* Mesh         = MakeMergeableSkeletalMesh(MeshSkeleton);
    CSkeleton*     BaseSkeleton = MakeOneBoneSkeleton();

    CSkeletalMesh* const Meshes[] = { Mesh };
    const TSpan<CSkeletalMesh* const> Span(Meshes, 1);

    SkeletalMeshMerge::FSettings Settings;
    Settings.BaseSkeleton = BaseSkeleton;

    const SkeletalMeshMerge::FResult First = SkeletalMeshMerge::Merge(Span, Settings);
    ASSERT_TRUE(First.IsValid());

    const SkeletalMeshMerge::FResult Cached = SkeletalMeshMerge::Merge(Span, Settings);
    ASSERT_TRUE(Cached.IsValid());
    EXPECT_EQ(Cached.Mesh.Get(), First.Mesh.Get());

    // BaseSkeleton differs from Mesh's own skeleton, so only its own socket generation marks this edit.
    BaseSkeleton->SetSkeletonResource(MakeOneBoneRig());

    const SkeletalMeshMerge::FResult AfterBaseEdit = SkeletalMeshMerge::Merge(Span, Settings);
    ASSERT_TRUE(AfterBaseEdit.IsValid());
    EXPECT_NE(AfterBaseEdit.Mesh.Get(), First.Mesh.Get());
}
