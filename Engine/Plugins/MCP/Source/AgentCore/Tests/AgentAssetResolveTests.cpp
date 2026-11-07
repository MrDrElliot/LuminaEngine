#include <gtest/gtest.h>

#include "Agent/AgentAssetResolve.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"

using namespace Lumina;
using namespace Lumina::Agent;

TEST(AgentAssetResolve, AnExactClassNameIsFound)
{
    CClass* Expected = StaticClass<CEntityScript>();

    FString Error;
    EXPECT_EQ(FindClassByName("CEntityScript", CEntityScript::StaticClass(), Error), Expected) << *Error;
}

TEST(AgentAssetResolve, ANameInADifferentCaseIsFound)
{
    CClass* Expected = StaticClass<CEntityScript>();

    FString Error;
    EXPECT_EQ(FindClassByName("centityscript", CEntityScript::StaticClass(), Error), Expected) << *Error;
}

TEST(AgentAssetResolve, AnUnknownNameIsRefused)
{
    FString Error;
    EXPECT_EQ(FindClassByName("CNoSuchClass", nullptr, Error), nullptr);
    EXPECT_NE(Error.Find("No class"), Constants::kIndexNone);
}

TEST(AgentAssetResolve, AClassOutsideTheBaseIsRefused)
{
    StaticClass<CEntityScript>();
    StaticClass<CWorld>();

    FString Error;
    EXPECT_EQ(FindClassByName("CWorld", CEntityScript::StaticClass(), Error), nullptr);
    EXPECT_NE(Error.Find("does not derive"), Constants::kIndexNone);
}
