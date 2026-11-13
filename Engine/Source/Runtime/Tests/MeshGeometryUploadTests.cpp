#include "gtest/gtest.h"

#include "Assets/AssetTypes/Mesh/MeshBuildBatch.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"
#include "World/ECS/Registry.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

// No RHI here, so every upload batch reports complete and only the staged state can be observed.

using namespace Lumina;

TEST(MeshGeometryUpload, StagedResourceLeavesTheLiveOneInPlaceUntilPublished)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>();
    Mesh->SetMeshResource(MakeUnique<FMeshResource>());
    const FMeshResource* Live = Mesh->TryGetMeshResource();
    ASSERT_NE(Live, nullptr);

    TUniquePtr<FMeshResource> Replacement = MakeUnique<FMeshResource>();
    const FMeshResource* Staged = Replacement.get();
    Mesh->StageMeshResource(Move(Replacement));

    EXPECT_EQ(Mesh->TryGetMeshResource(), Live);

    EXPECT_TRUE(Mesh->PublishPendingMeshResource());
    EXPECT_EQ(Mesh->TryGetMeshResource(), Staged);

    EXPECT_TRUE(Mesh->PublishPendingMeshResource());
    EXPECT_EQ(Mesh->TryGetMeshResource(), Staged);
}

TEST(MeshGeometryUpload, BatchedReplacementKeepsTheLiveResourceUntilTheBatchUploads)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>();
    Mesh->SetMeshResource(MakeUnique<FMeshResource>());
    const FMeshResource* Live = Mesh->TryGetMeshResource();
    const uint32 LiveVersion = Mesh->GetGeometryVersion();

    TUniquePtr<FMeshResource> Replacement = MakeUnique<FMeshResource>();
    const FMeshResource* Staged = Replacement.get();
    {
        ECS::FRegistry Registry;
        FMeshBuildBatchScope Batch(Registry);
        Mesh->SetMeshResourceBatched(Move(Replacement));

        EXPECT_EQ(Mesh->TryGetMeshResource(), Live);
        EXPECT_EQ(Mesh->GetGeometryVersion(), LiveVersion);
    }

    EXPECT_EQ(Mesh->TryGetMeshResource(), Staged);
    EXPECT_EQ(Mesh->GetGeometryVersion(), LiveVersion + 1);
}

TEST(MeshGeometryUpload, FirstResourcePublishesAtOnce)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>();
    TUniquePtr<FMeshResource> First = MakeUnique<FMeshResource>();
    const FMeshResource* Staged = First.get();

    Mesh->StageMeshResource(Move(First));

    EXPECT_EQ(Mesh->TryGetMeshResource(), Staged);
}

TEST(MeshGeometryUpload, ResolvingAMeshWhoseFirstResourceHasNotPublishedWaitsForThePublish)
{
    CSkeletalMesh* Mesh = NewObject<CSkeletalMesh>();
    ASSERT_EQ(Mesh->TryGetMeshResource(), nullptr);

    FMeshResolveCache& Cache = FMeshResolveCache::Get();
    Cache.ApplyPendingInvalidations();
    const uint32 Handle = Cache.Resolve(Mesh, {});
    ASSERT_TRUE(Cache.IsValidHandle(Handle));
    EXPECT_FALSE(Cache.GetEntry(Handle).bResolved);
    EXPECT_NE(Cache.GetEntryState(Handle), MESH_RESOLVE_STATE_STALE);

    Mesh->SetMeshResource(MakeUnique<FMeshResource>());
    Cache.ApplyPendingInvalidations();
    EXPECT_EQ(Cache.GetEntryState(Handle), MESH_RESOLVE_STATE_STALE);
}

namespace
{
    // Each bone parents the one before it, with an identity bind so the retarget sees matching poses.
    CSkeleton* MakeChainSkeleton(std::initializer_list<const char*> BoneNames)
    {
        TUniquePtr<FSkeletonResource> Rig = MakeUnique<FSkeletonResource>();
        for (const char* BoneName : BoneNames)
        {
            FSkeletonResource::FBoneInfo Bone;
            Bone.Name           = FName(BoneName);
            Bone.ParentIndex    = Rig->Bones.Num() - 1;
            Bone.InvBindMatrix  = FMatrix4::Identity();
            Bone.LocalTransform = FMatrix4::Identity();
            Rig->BoneNameToIndex[Bone.Name] = Rig->Bones.Num();
            Rig->Bones.Add(Bone);
        }
        Rig->BuildBindPoseCache();

        CSkeleton* Skeleton = NewObject<CSkeleton>();
        Skeleton->SetSkeletonResource(Move(Rig));
        return Skeleton;
    }
}

TEST(MeshGeometryUpload, SkeletonRetargetStagesARemappedResourceRatherThanRebuildingTheLiveOne)
{
    CSkeletalMesh* Mesh = NewObject<CSkeletalMesh>();
    Mesh->Skeleton = MakeChainSkeleton({ "Root", "Spine" });
    Mesh->RemapJointIndicesToSkeleton();

    TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
    Resource->bSkinnedMesh = true;
    Resource->MeshletData.MeshletBoneIndices = { 0u, 1u };
    Mesh->SetMeshResource(Move(Resource));
    const FMeshResource* Live = Mesh->TryGetMeshResource();

    Mesh->Skeleton = MakeChainSkeleton({ "Root", "Pelvis", "Spine" });
    Mesh->RemapJointIndicesToSkeleton();

    const FMeshResource* Published = Mesh->TryGetMeshResource();
    ASSERT_NE(Published, nullptr);
    EXPECT_NE(Published, Live);
    EXPECT_EQ(Published->MeshletData.MeshletBoneIndices, (TVector<uint32>{ 0u, 2u }));
}

TEST(MeshGeometryUpload, RestagingReplacesTheUnpublishedResource)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>();
    Mesh->SetMeshResource(MakeUnique<FMeshResource>());
    const FMeshResource* Live = Mesh->TryGetMeshResource();

    Mesh->StageMeshResource(MakeUnique<FMeshResource>());
    TUniquePtr<FMeshResource> Newer = MakeUnique<FMeshResource>();
    const FMeshResource* Staged = Newer.get();
    Mesh->StageMeshResource(Move(Newer));

    EXPECT_EQ(Mesh->TryGetMeshResource(), Live);
    EXPECT_TRUE(Mesh->PublishPendingMeshResource());
    EXPECT_EQ(Mesh->TryGetMeshResource(), Staged);
}
