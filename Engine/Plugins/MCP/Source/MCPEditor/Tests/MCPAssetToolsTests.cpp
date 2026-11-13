#include "MCPAssetTools.h"
#include "MCPToolTestUtil.h"
#include "ScopedTempMountTestUtil.h"

#include "Agent/AgentAssetResolve.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "Assets/AssetTypes/PhysicsMaterial/PhysicsMaterial.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPAssetToolsTestsInternal
{
    class MCPAssetToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPAssetToolsTest() : FRegisteredToolsTest("MCPAssetToolsTests", &RegisterAssetTools) {}

        void SetUp() override
        {
            RegisterPendingTypes();
            FRegisteredToolsTest::SetUp();

            ASSERT_TRUE(Filesystem::MakeDirectoryTree(Mount.Dir + "/Content/Props"));

            // Package paths stay loaded between tests, so each test names its assets after itself.
            Name = testing::UnitTest::GetInstance()->current_test_info()->name();
        }

        void TearDown() override
        {
            for (const FGuid& Guid : Created)
            {
                FAssetRegistry::Get().AssetDeleted(Guid);
            }
            FRegisteredToolsTest::TearDown();
        }

        FJsonValue Create(const char* ClassName, const FString& AssetName)
        {
            const FJsonValue Info = ExpectSucceeded("assets.create", FStringView(Format(
                R"({{"ClassName":"{}","Name":"{}","Folder":"/Game/Content/Props"}})", ClassName, AssetName)))["structuredContent"];
            Created.Add(*FGuid::TryParse(Info["Guid"].AsString()));
            return Info;
        }

        // Previous and Current are JSON text, so a float compares through a parse rather than its digits.
        static float ParseFloat(const FJsonValue& JsonText)
        {
            FJsonValue Parsed;
            EXPECT_TRUE(FJsonValue::Parse(JsonText.AsString(), Parsed));
            return Parsed.AsFloat(-1.0f);
        }

        FJsonValue SetProperty(const FString& Guid, const char* Path, const FString& Value)
        {
            return ExpectSucceeded("assets.set_property", FStringView(Format(
                R"({{"Asset":"{}","Path":"{}","Value":{}}})", Guid, Path, Value)))["structuredContent"];
        }

        // A physics asset whose first body points at Guid, saved so the registry sees the import.
        void CreateReferencer(const FString& Guid)
        {
            const FString Referencer(Create("CPhysicsAsset", Name + "_User")["Guid"].AsString());
            SetProperty(Referencer, "Bodies", Format(R"([{{"PhysicsMaterial":"{}"}}])", Guid));
            ExpectSucceeded("assets.save", FStringView(Format(R"({{"Asset":"{}"}})", Referencer)));
        }

        template<typename T>
        T* Resolve(const FString& Reference)
        {
            T* Asset = nullptr;
            FString Error;
            EXPECT_TRUE(Agent::ResolveAsset<T>(FStringView(Reference), Asset, Error)) << *Error;
            return Asset;
        }

        TestUtil::FScopedTempMount Mount{ "/Game", "LuminaMCPAssetToolsTests" };
        FString Name;
        TVector<FGuid> Created;
    };
}

using namespace MCPAssetToolsTestsInternal;

TEST_F(MCPAssetToolsTest, MoveRefusesADestinationOutsideScannedContent)
{
    ExpectRefused("assets.move",
        R"({"Path":"/Game/Content/Props/Barrel.lasset","DestinationFolder":"/Game/Scripts"})",
        "/Game/Content");
}

TEST_F(MCPAssetToolsTest, MoveRefusesASourceOutsideScannedContent)
{
    ExpectRefused("assets.move",
        R"({"Path":"/Engine/Resources/Shaders/X.lasset","NewName":"Y"})",
        "/Game/Content");
}

TEST_F(MCPAssetToolsTest, CreateSavesARegisteredAssetOfTheNamedClass)
{
    const FJsonValue Info = Create("CPhysicsMaterial", Name);
    const FString Path = Format("/Game/Content/Props/{}.lasset", Name);

    EXPECT_EQ(FString(Info["Path"].AsString()), Path);
    EXPECT_EQ(FString(Info["AssetClass"].AsString()), FString("CPhysicsMaterial"));
    EXPECT_TRUE(VFS::Exists(FStringView(Path)));

    const FAssetDataPtr Data = FAssetRegistry::Get().GetAssetByGUID(Created[0]);
    ASSERT_NE(Data, nullptr);
    EXPECT_EQ(FString(*Data->Path), Path);
    EXPECT_NE(Resolve<CPhysicsMaterial>(Path), nullptr);
}

TEST_F(MCPAssetToolsTest, CreateRefusesAnUnknownOrUncreatableClass)
{
    ExpectRefused("assets.create", R"({"ClassName":"CNoSuchAsset","Name":"X","Folder":"/Game/Content/Props"})",
        "Call assets.list_classes");
    ExpectRefused("assets.create", R"({"ClassName":"CObject","Name":"X","Folder":"/Game/Content/Props"})",
        "not a creatable asset class");
    EXPECT_FALSE(VFS::Exists(FStringView("/Game/Content/Props/X.lasset")));
}

TEST_F(MCPAssetToolsTest, SetPropertyAppliesTheValueAndReportsPreviousAndCurrent)
{
    const FString Guid(Create("CPhysicsMaterial", Name)["Guid"].AsString());

    const FJsonValue Result = SetProperty(Guid, "Friction", "0.9");
    EXPECT_FLOAT_EQ(ParseFloat(Result["Previous"]), 0.4f);
    EXPECT_FLOAT_EQ(ParseFloat(Result["Current"]), 0.9f);
    EXPECT_FALSE(Result["bUndoable"].AsBool(true));

    CPhysicsMaterial* Material = Resolve<CPhysicsMaterial>(Guid);
    ASSERT_NE(Material, nullptr);
    EXPECT_FLOAT_EQ(Material->Friction, 0.9f);

    ExpectRefused("assets.set_property", FStringView(Format(R"({{"Asset":"{}","Path":"Friction","Value":"Torch"}})", Guid)),
        "'Friction' has to be a number");
    EXPECT_FLOAT_EQ(Material->Friction, 0.9f);
}

TEST_F(MCPAssetToolsTest, DescribeReportsPropertiesAndReferencers)
{
    const FString Guid(Create("CPhysicsMaterial", Name)["Guid"].AsString());
    SetProperty(Guid, "Restitution", "0.75");
    CreateReferencer(Guid);

    const FJsonValue Described = ExpectSucceeded("assets.describe", FStringView(Format(R"({{"Asset":"{}"}})", Guid)))["structuredContent"];
    EXPECT_EQ(FString(Described["ClassName"].AsString()), FString("CPhysicsMaterial"));
    EXPECT_EQ(FString(Described["Guid"].AsString()), Guid);

    FJsonValue Properties;
    ASSERT_TRUE(FJsonValue::Parse(FStringView(FString(Described["Properties"].AsString())), Properties));
    EXPECT_FLOAT_EQ(Properties["Restitution"].AsFloat(), 0.75f);
    EXPECT_EQ(FString(Properties["FrictionCombine"].AsString()), FString("Average"));

    ASSERT_EQ(Described["Referencers"].Num(), 1);
    EXPECT_EQ(FString(Described["Referencers"][0]["Name"].AsString()), Name + "_User");
}

TEST_F(MCPAssetToolsTest, DeleteRefusesWhileReferencedUnlessForced)
{
    const FString Guid(Create("CPhysicsMaterial", Name)["Guid"].AsString());
    const FString Path = Format("/Game/Content/Props/{}.lasset", Name);
    CreateReferencer(Guid);

    const FJsonValue Refused = ExpectSucceeded("assets.delete", FStringView(Format(R"({{"Asset":"{}"}})", Guid)));
    EXPECT_NE(FString(Refused["content"][0]["text"].AsString()).Find("Pass bForce"), Constants::kIndexNone);
    EXPECT_FALSE(Refused["structuredContent"]["bDeleted"].AsBool(true));
    EXPECT_EQ(Refused["structuredContent"]["Referencers"].Num(), 1);
    EXPECT_TRUE(VFS::Exists(FStringView(Path)));

    const FJsonValue Deleted = ExpectSucceeded("assets.delete", FStringView(Format(R"({{"Asset":"{}","bForce":true}})", Path)))["structuredContent"];
    EXPECT_TRUE(Deleted["bDeleted"].AsBool(false));
    EXPECT_EQ(FString(Deleted["Path"].AsString()), Path);
    EXPECT_FALSE(VFS::Exists(FStringView(Path)));
    EXPECT_EQ(FAssetRegistry::Get().GetAssetByGUID(Created[0]), nullptr);

    ExpectRefused("assets.delete", FStringView(Format(R"({{"Asset":"{}"}})", Guid)), "No asset has GUID");
}

TEST_F(MCPAssetToolsTest, DuplicateCopiesTheSavedAssetBesideItself)
{
    const FString Guid(Create("CPhysicsMaterial", Name)["Guid"].AsString());
    SetProperty(Guid, "Density", "250");
    ExpectSucceeded("assets.save", FStringView(Format(R"({{"Asset":"{}"}})", Guid)));

    const FString CopyPath = Format("/Game/Content/Props/{}_Copy.lasset", Name);
    const FJsonValue Result = ExpectSucceeded("assets.duplicate", FStringView(Format(R"({{"Path":"/Game/Content/Props/{}.lasset"}})", Name)))["structuredContent"];
    EXPECT_EQ(FString(Result["Path"].AsString()), CopyPath);
    EXPECT_TRUE(VFS::Exists(FStringView(CopyPath)));

    const FAssetDataPtr Copy = FAssetRegistry::Get().GetAssetByPath(FStringView(CopyPath));
    ASSERT_NE(Copy, nullptr);
    Created.Add(Copy->AssetGUID);
    EXPECT_NE(Copy->AssetGUID, Created[0]);

    CPhysicsMaterial* Duplicate = Resolve<CPhysicsMaterial>(CopyPath);
    ASSERT_NE(Duplicate, nullptr);
    EXPECT_FLOAT_EQ(Duplicate->Density, 250.0f);
}
