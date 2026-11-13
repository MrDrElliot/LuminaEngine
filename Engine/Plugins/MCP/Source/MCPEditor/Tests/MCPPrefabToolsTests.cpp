#include "MCPPrefabTools.h"
#include "MCPToolTestUtil.h"
#include "ScopedTempMountTestUtil.h"

#include "Agent/AgentAssetResolve.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Assets/Factories/Factory.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "GUID/GUID.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPPrefabToolsTestsInternal
{
    class MCPPrefabToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPPrefabToolsTest() : FRegisteredToolsTest("MCPPrefabToolsTests", &RegisterPrefabTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();

            ASSERT_TRUE(Filesystem::MakeDirectoryTree(Mount.Dir + "/Content/Props"));

            // A variant reloads its parent from the registry, so the parent is a saved asset as it is in the editor.
            ParentName = testing::UnitTest::GetInstance()->current_test_info()->name();
            const FString ParentPath = Format("/Game/Content/Props/{}.lasset", ParentName);
            Parent = TStrongObjectPtr<CPrefab>(CFactory::CreateNewOf<CPrefab>(FStringView(ParentPath)));
            ASSERT_TRUE(Parent);
            ASSERT_TRUE(CPackage::SavePackage(Parent->GetPackage(), FStringView(ParentPath)));
            FAssetRegistry::Get().AssetCreated(Parent.Get());
            ParentGuid = FString(*Parent->GetGUID().ToString());
            Created.Add(Parent->GetGUID());
        }

        void TearDown() override
        {
            for (const FGuid& Guid : Created)
            {
                FAssetRegistry::Get().AssetDeleted(Guid);
            }
            if (Parent)
            {
                CFactory::DiscardNew(Parent.Get());
                Parent.Reset();
            }
            FRegisteredToolsTest::TearDown();
        }

        CPrefab* ExpectVariantCreated(const FString& Arguments, const FString& ExpectedPath)
        {
            const FJsonValue Result = ExpectSucceeded("prefab.create", FStringView(Arguments));
            const FJsonValue& Structured = Result["structuredContent"];
            EXPECT_EQ(FString(Structured["Path"].AsString()), ExpectedPath);
            EXPECT_TRUE(VFS::Exists(FStringView(ExpectedPath)));

            CPrefab* Variant = nullptr;
            FString Error;
            EXPECT_TRUE(Agent::ResolveAsset<CPrefab>(Structured["Guid"].AsString(), Variant, Error)) << *Error;
            if (Variant != nullptr)
            {
                Created.Add(Variant->GetGUID());
            }
            return Variant;
        }

        TestUtil::FScopedTempMount Mount{ "/Game", "LuminaMCPPrefabToolsTests" };
        FString ParentName;
        FString ParentGuid;
        TStrongObjectPtr<CPrefab> Parent;
        TVector<FGuid> Created;
    };
}

using namespace MCPPrefabToolsTestsInternal;

TEST_F(MCPPrefabToolsTest, VariantOfSavesBesideTheParentAndPointsAtIt)
{
    CPrefab* Variant = ExpectVariantCreated(Format(R"({{"VariantOf":"{}","Name":"BigCrate"}})", ParentGuid),
        "/Game/Content/Props/BigCrate.lasset");
    ASSERT_NE(Variant, nullptr);
    EXPECT_TRUE(Variant->IsVariant());
    EXPECT_EQ(Variant->ParentPrefab.Get(), Parent.Get());
}

TEST_F(MCPPrefabToolsTest, VariantOfDefaultsTheNameAndRefusesToOverwrite)
{
    const FString Arguments = Format(R"({{"VariantOf":"{}"}})", ParentGuid);
    ExpectVariantCreated(Arguments, Format("/Game/Content/Props/{}_Variant.lasset", ParentName));
    ExpectRefused("prefab.create", FStringView(Arguments), "already exists");
}

TEST_F(MCPPrefabToolsTest, VariantOfRefusesSomethingThatIsNotAPrefab)
{
    ExpectRefused("prefab.create", R"({"VariantOf":"Crate"})", "neither a GUID nor the content path");
}

TEST_F(MCPPrefabToolsTest, SceneEditsRefuseWithoutAWorldEditor)
{
    ExpectRefused("prefab.create", R"({"Entities":["1"],"Name":"FromSelection"})", "No world editor is open");
    ExpectRefused("prefab.instance", R"({"Action":"Detach","Entity":"1"})", "No world editor is open");
    ExpectRefused("prefab.instance", R"({"Action":"Resync","Entity":"1"})", "No world editor is open");
}
