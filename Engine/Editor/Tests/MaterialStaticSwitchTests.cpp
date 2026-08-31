#include <gtest/gtest.h>

#include "Containers/Format.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Renderer/MaterialTypes.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_StaticSwitch.h"

using namespace Lumina;

namespace
{
    FName IndexedName(const char* Prefix, uint32 Index)
    {
        return FName(Format("{}{}", Prefix, Index));
    }
}

TEST(MaterialStaticSwitch, AnUnnamedSwitchTakesItsDefaultAndClaimsNoKeyBit)
{
    FMaterialCompiler Compiler;

    EXPECT_TRUE(Compiler.ResolveStaticSwitch(NAME_None, true, nullptr));
    EXPECT_FALSE(Compiler.ResolveStaticSwitch(NAME_None, false, nullptr));

    TVector<FMaterialStaticSwitch> Switches;
    Compiler.GetStaticSwitches(Switches);
    EXPECT_TRUE(Switches.empty());
    EXPECT_FALSE(Compiler.HasErrors());
}

TEST(MaterialStaticSwitch, ANamedSwitchTakesTheOverrideWhenThePermutationSuppliesOne)
{
    FMaterialCompiler Compiler;

    THashMap<FName, bool> Overrides;
    Overrides["Detail"] = false;
    Compiler.SetStaticSwitchOverrides(Overrides);

    EXPECT_FALSE(Compiler.ResolveStaticSwitch("Detail", true, nullptr));
    EXPECT_TRUE(Compiler.ResolveStaticSwitch("Untouched", true, nullptr));
    EXPECT_FALSE(Compiler.HasErrors());
}

TEST(MaterialStaticSwitch, TheManifestIsOrderedByNameSoTheKeySurvivesANodeReorder)
{
    FMaterialCompiler Compiler;
    Compiler.ResolveStaticSwitch("Zeta", true, nullptr);
    Compiler.ResolveStaticSwitch("Alpha", false, nullptr);
    Compiler.ResolveStaticSwitch("Mid", true, nullptr);

    TVector<FMaterialStaticSwitch> Switches;
    Compiler.GetStaticSwitches(Switches);

    ASSERT_EQ(Switches.size(), (size_t)3);
    EXPECT_EQ(Switches[0].ParameterName, FName("Alpha"));
    EXPECT_EQ(Switches[1].ParameterName, FName("Mid"));
    EXPECT_EQ(Switches[2].ParameterName, FName("Zeta"));
    EXPECT_EQ(Switches[0].BitIndex, 0);
    EXPECT_EQ(Switches[1].BitIndex, 1);
    EXPECT_EQ(Switches[2].BitIndex, 2);
    EXPECT_FALSE(Switches[0].bDefaultValue);
}

TEST(MaterialStaticSwitch, TwoSwitchesSharingANameMustAgreeOnTheirDefault)
{
    FMaterialCompiler Compiler;
    Compiler.ResolveStaticSwitch("Shared", true, nullptr);
    EXPECT_FALSE(Compiler.HasErrors());

    Compiler.ResolveStaticSwitch("Shared", false, nullptr);
    EXPECT_TRUE(Compiler.HasErrors());

    TVector<FMaterialStaticSwitch> Switches;
    Compiler.GetStaticSwitches(Switches);
    EXPECT_EQ(Switches.size(), (size_t)1);
}

TEST(MaterialStaticSwitch, TheKeyIsAUint64SoTheSixtyFifthSwitchIsRefused)
{
    FMaterialCompiler Compiler;
    for (uint32 i = 0; i < kMaxStaticSwitches; ++i)
    {
        Compiler.ResolveStaticSwitch(IndexedName("Switch", i), true, nullptr);
    }
    EXPECT_FALSE(Compiler.HasErrors());

    Compiler.ResolveStaticSwitch("OneTooMany", true, nullptr);
    EXPECT_TRUE(Compiler.HasErrors());

    TVector<FMaterialStaticSwitch> Switches;
    Compiler.GetStaticSwitches(Switches);
    EXPECT_EQ(Switches.size(), (size_t)kMaxStaticSwitches);
}

TEST(MaterialStaticSwitch, ReachabilityDropsTheBranchTheSwitchDidNotTake)
{
    // Any expression node would do; a switch is the one class this suite can construct across modules.
    CMaterialExpression_StaticSwitch* Switch  = NewObject<CMaterialExpression_StaticSwitch>();
    CMaterialExpression_StaticSwitch* OnTrue  = NewObject<CMaterialExpression_StaticSwitch>();
    CMaterialExpression_StaticSwitch* OnFalse = NewObject<CMaterialExpression_StaticSwitch>();
    ASSERT_NE(Switch, nullptr);

    Switch->BuildNode();
    OnTrue->BuildNode();
    OnFalse->BuildNode();

    Switch->True->AddConnection(OnTrue->Output);
    OnTrue->Output->AddConnection(Switch->True);
    Switch->False->AddConnection(OnFalse->Output);
    OnFalse->Output->AddConnection(Switch->False);

    Switch->SetResolvedValue(true);
    THashSet<CEdGraphNode*> Reachable;
    GraphAlgorithms::CollectReachableFromRoot(Switch, Reachable);
    EXPECT_NE(Reachable.find(OnTrue), Reachable.end());
    EXPECT_EQ(Reachable.find(OnFalse), Reachable.end());

    Switch->SetResolvedValue(false);
    Reachable.clear();
    GraphAlgorithms::CollectReachableFromRoot(Switch, Reachable);
    EXPECT_NE(Reachable.find(OnFalse), Reachable.end());
    EXPECT_EQ(Reachable.find(OnTrue), Reachable.end());
}
