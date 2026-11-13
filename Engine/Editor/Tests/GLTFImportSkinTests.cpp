#include <cstring>

#include <gtest/gtest.h>

#include "ScopedTempMountTestUtil.h"

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/Import/GLTFImporter.h"

using namespace Lumina;

namespace
{
    // One joint, no bufferView/data on its inverseBindMatrices accessor, count 0 (malformed/truncated skin).
    constexpr const char* kZeroCountSkinGLTF = R"({
        "asset": { "version": "2.0" },
        "scene": 0,
        "scenes": [ { "nodes": [0] } ],
        "nodes": [ { "name": "Root" } ],
        "skins": [ { "joints": [0], "inverseBindMatrices": 0 } ],
        "accessors": [ { "count": 0, "type": "MAT4", "componentType": 5126 } ]
    })";
}

TEST(GLTFImportSkin, ZeroCountInverseBindMatricesImportsIdentityBindPose)
{
    TestUtil::FScopedTempMount Mount{ "/Game", "LuminaGLTFImportSkinTests" };

    const FString SourcePath = Format("{}/ZeroCountSkin_{}.gltf", Filesystem::GetTempDirectory(), FGuid::New().ToString());
    const TSpan64<const uint8> SourceBytes(reinterpret_cast<const uint8*>(kZeroCountSkinGLTF),
                                            static_cast<int64>(strlen(kZeroCountSkinGLTF)));
    ASSERT_TRUE(Filesystem::WriteFile(FStringView(*SourcePath, SourcePath.Len()), SourceBytes));

    CGLTFImporter* Importer = NewObject<CGLTFImporter>(nullptr, FName("ZeroCountSkinImporter"), FGuid::New(), OF_Transient);
    ASSERT_NE(Importer, nullptr);

    FImportRequest Request;
    Request.SourcePath = FFixedString(*SourcePath, SourcePath.Len());
    Request.DestinationPath = "/Game/ZeroCountSkinAsset";

    FString ParseError;
    ASSERT_TRUE(Importer->ParseSource(Request, ParseError, nullptr)) << *ParseError;

    FImportResult Result;
    Importer->BuildAssets(Request, Result, nullptr);
    ASSERT_TRUE(Result.Succeeded()) << *Result.Error;

    CSkeleton* Skeleton = nullptr;
    for (const TStrongObjectPtr<CObject>& Created : Result.CreatedObjects)
    {
        if (CSkeleton* AsSkeleton = Cast<CSkeleton>(Created.Get()))
        {
            Skeleton = AsSkeleton;
            break;
        }
    }
    ASSERT_NE(Skeleton, nullptr);

    const FSkeletonResource* Resource = Skeleton->GetSkeletonResource();
    ASSERT_NE(Resource, nullptr);
    ASSERT_EQ(Resource->Bones.Num(), 1);
    EXPECT_EQ(Resource->Bones[0].InvBindMatrix, FMatrix4(1.0f));

    for (const TStrongObjectPtr<CObject>& Created : Result.CreatedObjects)
    {
        if (CObject* Object = Created.Get())
        {
            FAssetRegistry::Get().AssetDeleted(Object->GetGUID());
            CFactory::DiscardNew(Object);
        }
    }

    Filesystem::RemoveFile(FStringView(*SourcePath, SourcePath.Len()));
}
