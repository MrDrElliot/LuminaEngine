#include <gtest/gtest.h>

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "Tools/Impostor/ImpostorBaker.h"

using namespace Lumina;

TEST(ImpostorBakerEnqueue, DoesNotRequeueAMeshAlreadyActive)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>(CPackage::CreatePackage("/ImpostorBakerEnqueueTests/Active"), "Active");
    FImpostorBaker Baker;
    FImpostorBakerTestAccess::MarkActive(Baker, Mesh);

    Baker.Enqueue(Mesh);

    EXPECT_EQ(FImpostorBakerTestAccess::QueueSize(Baker), 0);
    FImpostorBakerTestAccess::Reset(Baker);
}

TEST(ImpostorBakerEnqueue, DoesNotRequeueAMeshAlreadyBuilding)
{
    CStaticMesh* Mesh = NewObject<CStaticMesh>(CPackage::CreatePackage("/ImpostorBakerEnqueueTests/Building"), "Building");
    FImpostorBaker Baker;
    FImpostorBakerTestAccess::MarkBuilding(Baker, Mesh);

    Baker.Enqueue(Mesh);

    EXPECT_EQ(FImpostorBakerTestAccess::QueueSize(Baker), 0);
    FImpostorBakerTestAccess::Reset(Baker);
}
