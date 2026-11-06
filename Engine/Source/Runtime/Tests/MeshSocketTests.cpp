#include <gtest/gtest.h>

#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Core/Object/ObjectCore.h"

using namespace Lumina;

namespace
{
    SMeshSocket MakeSocket(const char* Name, const char* Bone, float OffsetX)
    {
        SMeshSocket Socket;
        Socket.SocketName = FName(Name);
        Socket.BoneName = FName(Bone);
        Socket.RelativeTransform = FTransform(FVector3(OffsetX, 0.0f, 0.0f));
        return Socket;
    }

    struct FSocketRig
    {
        CSkeleton*     Skeleton = nullptr;
        CSkeletalMesh* Mesh = nullptr;
    };

    // The skeleton carries Grip and Back, and the mesh overrides Back and adds Muzzle.
    FSocketRig MakeSocketRig()
    {
        FSocketRig Rig;
        Rig.Skeleton = NewObject<CSkeleton>();
        Rig.Skeleton->Sockets.Add(MakeSocket("Grip", "Hand", 1.0f));
        Rig.Skeleton->Sockets.Add(MakeSocket("Back", "Spine", 2.0f));

        Rig.Mesh = NewObject<CSkeletalMesh>();
        Rig.Mesh->Skeleton = Rig.Skeleton;
        Rig.Mesh->Sockets.Add(MakeSocket("Back", "Chest", 3.0f));
        Rig.Mesh->Sockets.Add(MakeSocket("Muzzle", "Hand", 4.0f));
        return Rig;
    }
}

TEST(MeshSocket, MeshSocketOverridesTheSkeletonSocketOfTheSameName)
{
    const FSocketRig Rig = MakeSocketRig();

    const SMeshSocket* Back = Rig.Mesh->FindSocket(FName("Back"));
    ASSERT_NE(Back, nullptr);
    EXPECT_EQ(Back->BoneName, FName("Chest"));
    EXPECT_FLOAT_EQ(Back->RelativeTransform.GetLocation().x, 3.0f);

    const SMeshSocket* Grip = Rig.Mesh->FindSocket(FName("Grip"));
    ASSERT_NE(Grip, nullptr);
    EXPECT_EQ(Grip->BoneName, FName("Hand"));

    EXPECT_NE(Rig.Mesh->FindSocket(FName("Muzzle")), nullptr);
    EXPECT_EQ(Rig.Mesh->FindSocket(FName("Missing")), nullptr);

    // The skeleton itself never sees the mesh's sockets.
    EXPECT_EQ(Rig.Skeleton->FindSocket(FName("Back"))->BoneName, FName("Spine"));
    EXPECT_EQ(Rig.Skeleton->FindSocket(FName("Muzzle")), nullptr);
}

TEST(MeshSocket, ActiveSocketsListTheMeshFirstAndDropShadowedSkeletonSockets)
{
    const FSocketRig Rig = MakeSocketRig();

    TVector<const SMeshSocket*> Active;
    Rig.Mesh->CollectActiveSockets(Active);

    ASSERT_EQ(Active.Num(), 3);
    EXPECT_EQ(Active[0]->SocketName, FName("Back"));
    EXPECT_EQ(Active[0]->BoneName, FName("Chest"));
    EXPECT_EQ(Active[1]->SocketName, FName("Muzzle"));
    EXPECT_EQ(Active[2]->SocketName, FName("Grip"));
}

TEST(MeshSocket, MeshWithoutSkeletonResolvesOnlyItsOwnSockets)
{
    CSkeletalMesh* Mesh = NewObject<CSkeletalMesh>();
    Mesh->Sockets.Add(MakeSocket("Muzzle", "Hand", 1.0f));

    EXPECT_NE(Mesh->FindSocket(FName("Muzzle")), nullptr);
    EXPECT_EQ(Mesh->FindSocket(FName("Grip")), nullptr);

    TVector<const SMeshSocket*> Active;
    Mesh->CollectActiveSockets(Active);
    EXPECT_EQ(Active.Num(), 1);
}
