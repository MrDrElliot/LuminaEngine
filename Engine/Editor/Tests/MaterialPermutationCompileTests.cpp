#include <gtest/gtest.h>

#include "Assets/AssetTypes/Material/Material.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/MaterialTypes.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"

using namespace Lumina;

namespace
{
    FMaterialStaticSwitch MakeSwitch(const char* Name, bool bDefault, uint8 Bit)
    {
        FMaterialStaticSwitch Switch;
        Switch.ParameterName = Name;
        Switch.bDefaultValue = bDefault;
        Switch.BitIndex      = Bit;
        return Switch;
    }

    FMaterialParameter MakeScalar(const char* Name, uint16 Index)
    {
        FMaterialParameter Param;
        Param.ParameterName = Name;
        Param.Type          = EMaterialParameterType::Scalar;
        Param.Index         = Index;
        return Param;
    }

    FMaterialParameter MakeVector(const char* Name, uint16 Index)
    {
        FMaterialParameter Param;
        Param.ParameterName = Name;
        Param.Type          = EMaterialParameterType::Vector;
        Param.Index         = Index;
        return Param;
    }

    const FMaterialParameter* Find(const TVector<FMaterialParameter>& Params, const char* Name)
    {
        for (const FMaterialParameter& Param : Params)
        {
            if (Param.ParameterName == FName(Name))
            {
                return &Param;
            }
        }
        return nullptr;
    }
}

// Slot assignment is otherwise walk-order dependent, so a dropped branch would shift everything after it.
TEST(MaterialPermutationSeed, ASeededParameterKeepsTheIndexTheUniformBlockAlreadyUses)
{
    FMaterialCompiler Compiler;

    TVector<FMaterialParameter> Seed;
    Seed.push_back(MakeScalar("Roughness", 0));
    Seed.push_back(MakeScalar("Metallic", 1));
    Seed.push_back(MakeScalar("DetailStrength", 2));

    FMaterialUniforms Uniforms = {};
    Uniforms.Scalars[0] = 0.25f;
    Uniforms.Scalars[1] = 0.5f;
    Uniforms.Scalars[2] = 0.75f;

    Compiler.SeedManifest(Seed, Uniforms, {}, {});

    // Reached in a different order than the seed, which is exactly what a dropped branch causes.
    Compiler.DefineFloatParameter("N0", "Metallic", 0.0f);
    Compiler.DefineFloatParameter("N1", "Roughness", 0.0f);

    TVector<FMaterialParameter> Out;
    FMaterialUniforms           OutUniforms = {};
    Compiler.GetParameters(Out, OutUniforms);

    ASSERT_NE(Find(Out, "Roughness"), nullptr);
    ASSERT_NE(Find(Out, "Metallic"), nullptr);
    EXPECT_EQ(Find(Out, "Roughness")->Index, 0);
    EXPECT_EQ(Find(Out, "Metallic")->Index, 1);
    EXPECT_FALSE(Compiler.HasErrors());
}

// The seeded value must survive, or a permutation compile would reset the master's authored values.
TEST(MaterialPermutationSeed, ASeededParameterKeepsItsValueRatherThanTheNodeDefault)
{
    FMaterialCompiler Compiler;

    TVector<FMaterialParameter> Seed;
    Seed.push_back(MakeScalar("Roughness", 0));

    FMaterialUniforms Uniforms = {};
    Uniforms.Scalars[0] = 0.875f;

    Compiler.SeedManifest(Seed, Uniforms, {}, {});
    Compiler.DefineFloatParameter("N0", "Roughness", 0.125f);

    TVector<FMaterialParameter> Out;
    FMaterialUniforms           OutUniforms = {};
    Compiler.GetParameters(Out, OutUniforms);

    ASSERT_NE(Find(Out, "Roughness"), nullptr);
    EXPECT_FLOAT_EQ(Find(Out, "Roughness")->ScalarDefault, 0.875f);
    EXPECT_FLOAT_EQ(OutUniforms.Scalars[0], 0.875f);
}

// A branch only this permutation takes brings parameters the master never declared.
TEST(MaterialPermutationSeed, APermutationOnlyParameterAppendsPastTheSeededSlots)
{
    FMaterialCompiler Compiler;

    TVector<FMaterialParameter> Seed;
    Seed.push_back(MakeScalar("Roughness", 0));
    Seed.push_back(MakeScalar("Metallic", 1));

    FMaterialUniforms Uniforms = {};
    Compiler.SeedManifest(Seed, Uniforms, {}, {});

    Compiler.DefineFloatParameter("N0", "ParallaxDepth", 0.05f);

    TVector<FMaterialParameter> Out;
    FMaterialUniforms           OutUniforms = {};
    Compiler.GetParameters(Out, OutUniforms);

    ASSERT_NE(Find(Out, "ParallaxDepth"), nullptr);
    EXPECT_EQ(Find(Out, "ParallaxDepth")->Index, 2) << "must not land on a slot the seed already owns";
    EXPECT_EQ(Out.size(), (size_t)3) << "the manifest is the union across permutations";
}

// Scalars and vectors index into separate arrays, so seeding one must not consume the other.
TEST(MaterialPermutationSeed, ScalarAndVectorSlotsAreCountedSeparately)
{
    FMaterialCompiler Compiler;

    TVector<FMaterialParameter> Seed;
    Seed.push_back(MakeScalar("Roughness", 0));
    Seed.push_back(MakeScalar("Metallic", 1));
    Seed.push_back(MakeVector("BaseTint", 0));

    FMaterialUniforms Uniforms = {};
    Compiler.SeedManifest(Seed, Uniforms, {}, {});

    float Value[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    Compiler.DefineFloat4Parameter("N0", "EmissiveTint", Value);
    Compiler.DefineFloatParameter("N1", "ParallaxDepth", 0.05f);

    TVector<FMaterialParameter> Out;
    FMaterialUniforms           OutUniforms = {};
    Compiler.GetParameters(Out, OutUniforms);

    ASSERT_NE(Find(Out, "EmissiveTint"), nullptr);
    ASSERT_NE(Find(Out, "ParallaxDepth"), nullptr);
    EXPECT_EQ(Find(Out, "EmissiveTint")->Index, 1);
    EXPECT_EQ(Find(Out, "ParallaxDepth")->Index, 2);
}

// A parameter past the block budget cannot be seeded, since nothing in the block describes it.
TEST(MaterialPermutationSeed, AnOverBudgetSeedEntryIsDropped)
{
    FMaterialCompiler Compiler;

    TVector<FMaterialParameter> Seed;
    Seed.push_back(MakeScalar("Corrupt", (uint16)(MAX_SCALARS + 4)));

    FMaterialUniforms Uniforms = {};
    Compiler.SeedManifest(Seed, Uniforms, {}, {});

    TVector<FMaterialParameter> Out;
    FMaterialUniforms           OutUniforms = {};
    Compiler.GetParameters(Out, OutUniforms);

    EXPECT_TRUE(Out.empty());
}

// The key is what an instance carries, so the compile has to be able to read switch values back out of it.
TEST(MaterialPermutationTarget, AKeyInvertsBackIntoTheSwitchValuesThatBuiltIt)
{
    CMaterial* Material = NewObject<CMaterial>();
    ASSERT_NE(Material, nullptr);

    Material->StaticSwitches.push_back(MakeSwitch("Detail", false, 0));
    Material->StaticSwitches.push_back(MakeSwitch("Parallax", true, 1));
    Material->StaticSwitches.push_back(MakeSwitch("Tint", false, 2));

    THashMap<FName, bool> Values;
    Values["Detail"] = true;
    Values["Tint"]   = true;
    const uint64 Key = Material->MakeStaticSwitchKey(Values);

    FMaterialCompileTarget Target;
    ASSERT_TRUE(MakeMaterialPermutationTarget(Material, Key, Target));

    EXPECT_TRUE(Target.bPermutation);
    EXPECT_EQ(Target.Key, Key);
    EXPECT_EQ(Target.StaticSwitchOverrides.size(), (size_t)3) << "every switch is pinned, defaults included";
    EXPECT_TRUE(Target.StaticSwitchOverrides["Detail"]);
    EXPECT_TRUE(Target.StaticSwitchOverrides["Parallax"]) << "unnamed in Values, so it keeps its default";
    EXPECT_TRUE(Target.StaticSwitchOverrides["Tint"]);

    // Round-trips, which is what keeps the compiled permutation and the requested key in agreement.
    EXPECT_EQ(Material->MakeStaticSwitchKey(Target.StaticSwitchOverrides), Key);
}

TEST(MaterialPermutationTarget, AMaterialWithNoSwitchesHasNoPermutationToBuild)
{
    CMaterial* Material = NewObject<CMaterial>();
    ASSERT_NE(Material, nullptr);

    FMaterialCompileTarget Target;
    EXPECT_FALSE(MakeMaterialPermutationTarget(Material, 0ull, Target));
    EXPECT_FALSE(MakeMaterialPermutationTarget(nullptr, 0ull, Target));
    EXPECT_FALSE(Target.bPermutation);
}
