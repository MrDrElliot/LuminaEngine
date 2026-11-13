#include "MCPLookDevTools.h"
#include "MCPToolTestUtil.h"

#include "Core/Object/ObjectCore.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/LightComponent.h"
#include "World/World.h"

using namespace Lumina;
using namespace Lumina::MCP;

namespace MCPLookDevToolsTestsInternal
{
    class MCPLookDevToolsTest : public FRegisteredToolsTest
    {
    protected:

        MCPLookDevToolsTest() : FRegisteredToolsTest("MCPLookDevToolsTests", &RegisterLookDevTools) {}
    };
}

using namespace MCPLookDevToolsTestsInternal;

TEST_F(MCPLookDevToolsTest, CaptureRefusesAnUnknownPreset)
{
    ExpectRefused("lookdev.capture", R"({"Label":"T0","Presets":["ContactClose","Sideways"]})", "No preset is named 'Sideways'");
}

TEST_F(MCPLookDevToolsTest, CaptureRefusesZeroFrames)
{
    ExpectRefused("lookdev.capture", R"({"Label":"T0","FrameCount":0})", "FrameCount is 0");
}

TEST_F(MCPLookDevToolsTest, CaptureRefusesALabelThatIsNotOneFolder)
{
    ExpectRefused("lookdev.capture", R"({"Label":"a/b"})", "cannot name a folder");
    ExpectRefused("lookdev.capture", R"({"Label":""})", "cannot name a folder");
}

TEST_F(MCPLookDevToolsTest, BuildRefusesAnElevationOutsideTheSky)
{
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":0})", "SunElevationDegrees is 0");
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":-5})", "SunElevationDegrees is -5");
    ExpectRefused("lookdev.build_scene", R"({"SunElevationDegrees":91})", "SunElevationDegrees is 91");
}

// SessionOps finds no world editor in this headless harness, so this calls the shared cleanup both tools use instead.
TEST(MCPLookDevFixture, ClearFixtureTargetsItsOwnWorldEvenWhileAnotherWorldIsCurrent)
{
    CWorld* FixtureWorld = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    CWorld* OtherWorld   = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);

    const ECS::FEntity Sun = FixtureWorld->ConstructEntity("Sun");
    FixtureWorld->EmplaceComponent<SDirectionalLightComponent>(Sun);
    FixtureWorld->GetEntityRegistry().Emplace<SDisabledTag>(Sun);

    const ECS::FEntity FixtureEntity = FixtureWorld->ConstructEntity("LookDev Fixture Entity");

    FLookDevFixtureState State;
    State.World = FixtureWorld;
    State.Entities.Add(FixtureEntity);
    State.DisabledSuns.Add(Sun);

    // OtherWorld stands in for whatever the active scene tab shows now, which the fixture was never built in.
    const ECS::FEntity Unrelated = OtherWorld->ConstructEntity("Unrelated");

    int32 Destroyed = 0;
    ClearFixture(State, Destroyed);

    EXPECT_EQ(Destroyed, 1);
    EXPECT_FALSE(FixtureWorld->IsValidEntity(FixtureEntity));
    EXPECT_TRUE(FixtureWorld->IsValidEntity(Sun));
    EXPECT_FALSE(FixtureWorld->GetEntityRegistry().HasAll<SDisabledTag>(Sun));
    EXPECT_TRUE(FixtureWorld->TryGetComponent<SDirectionalLightComponent>(Sun) != nullptr);
    EXPECT_TRUE(OtherWorld->IsValidEntity(Unrelated));
    EXPECT_TRUE(State.Entities.IsEmpty());
    EXPECT_TRUE(State.DisabledSuns.IsEmpty());
    EXPECT_FALSE(State.World.IsValid());
}
