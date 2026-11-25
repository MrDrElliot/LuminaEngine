#include <gtest/gtest.h>

#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/ObjectCore.h"
#include "World/Entity/Components/AnimationGraphComponent.h"
#include "World/Entity/Components/CharacterComponent.h"

using namespace Lumina;

namespace
{
    TStrongObjectPtr<CAnimationGraph> MakeGraphReading(std::initializer_list<std::pair<const char*, const char*>> Inputs)
    {
        RegisterPendingTypes();
        TStrongObjectPtr<CAnimationGraph> Graph = NewObject<CAnimationGraph>();
        for (const auto& [Name, Component] : Inputs)
        {
            SAnimGraphInput& Input = Graph->Inputs.EmplaceGetRef();
            Input.Name      = FName(Name);
            Input.Component = FName(Component);
        }
        Graph->LinkParameters();
        return Graph;
    }

    FString ResolveError(const CAnimationGraph& Graph, const char* Path, bool bWantObject)
    {
        FAnimGraphParamBinding Binding;
        FString Error;
        EXPECT_FALSE(Graph.ResolveParameterPath(FName(Path), bWantObject, Binding, &Error)) << Path;
        return Error;
    }
}

// A path reads a field of an input component in place, through nested structs, at the offset the component lays it out.
TEST(AnimGraphInputs, PathsReadNestedFieldsOfAnInputInPlace)
{
    TStrongObjectPtr<CAnimationGraph> Graph = MakeGraphReading({ { "Physics", "SCharacterPhysicsComponent" } });
    ASSERT_EQ(Graph->InputStructs.Num(), 1);
    ASSERT_NE(Graph->InputStructs[0], nullptr);

    SCharacterPhysicsComponent Physics;
    Physics.Radius = 0.42f;
    Physics.CollisionProfile.Layer = ECollisionProfiles::Static;
    Physics.bCollideWithCharacters = false;
    const uint8* Bases[kMaxAnimGraphInputs] = { reinterpret_cast<const uint8*>(&Physics) };

    const auto Read = [&](const char* Path)
    {
        FAnimGraphParamBinding Binding;
        FString Error;
        EXPECT_TRUE(Graph->ResolveParameterPath(FName(Path), false, Binding, &Error)) << Path << " " << *Error;
        EXPECT_EQ(Binding.Input, 0) << Path;
        const uint8* Base = ResolveAnimParamBase(Binding, nullptr, Bases);
        return Base != nullptr ? ReadAnimParamScalar(Base, Binding) : -1.0f;
    };

    EXPECT_FLOAT_EQ(Read("Physics.Radius"), 0.42f);
    EXPECT_FLOAT_EQ(Read("Physics.CollisionProfile.Layer"), (float)ECollisionProfiles::Static);
    EXPECT_FLOAT_EQ(Read("Physics.bCollideWithCharacters"), 0.0f);
}

// One asset reference may be followed, so a component pointing at an asset reads that asset's fields.
TEST(AnimGraphInputs, APathFollowsOneAssetReference)
{
    TStrongObjectPtr<CAnimationGraph> Graph = MakeGraphReading({ { "Anim", "SAnimationGraphComponent" } });
    TStrongObjectPtr<CAnimationGraph> Target = NewObject<CAnimationGraph>();
    Target->bGpuCrowd = false;

    SAnimationGraphComponent Component;
    Component.Graph = Target;
    const uint8* Bases[kMaxAnimGraphInputs] = { reinterpret_cast<const uint8*>(&Component) };

    FAnimGraphParamBinding Binding;
    FString Error;
    ASSERT_TRUE(Graph->ResolveParameterPath(FName("Anim.Graph.bGpuCrowd"), false, Binding, &Error)) << *Error;
    EXPECT_NE(Binding.HopOffset, kAnimNoHop);
    const uint8* Base = ResolveAnimParamBase(Binding, nullptr, Bases);
    ASSERT_EQ(Base, reinterpret_cast<const uint8*>(Target.Get()));
    EXPECT_FLOAT_EQ(ReadAnimParamScalar(Base, Binding), 0.0f);

    Target->bGpuCrowd = true;
    EXPECT_FLOAT_EQ(ReadAnimParamScalar(ResolveAnimParamBase(Binding, nullptr, Bases), Binding), 1.0f);

    FAnimGraphParamBinding AssetBinding;
    ASSERT_TRUE(Graph->ResolveParameterPath(FName("Anim.Graph"), true, AssetBinding, &Error)) << *Error;
    EXPECT_EQ(ReadAnimParamObject(ResolveAnimParamBase(AssetBinding, nullptr, Bases), AssetBinding), Target.Get());

    // A missing asset leaves nothing to read, which the update turns into the parameter's default.
    Component.Graph = nullptr;
    EXPECT_EQ(ResolveAnimParamBase(Binding, nullptr, Bases), nullptr);
}

// Every path that cannot be read says why, which the compiler shows on the node that names it.
TEST(AnimGraphInputs, UnreadablePathsSayWhy)
{
    TStrongObjectPtr<CAnimationGraph> Graph = MakeGraphReading({ { "Anim", "SAnimationGraphComponent" }, { "Gone", "SNoSuchComponent" } });

    EXPECT_TRUE(ResolveError(*Graph, "Anim.NoSuchField", false).Contains("has no field 'NoSuchField'"));
    EXPECT_TRUE(ResolveError(*Graph, "Anim.Graph", false).Contains("is an asset, not a value"));
    EXPECT_TRUE(ResolveError(*Graph, "Anim.bPlaying", true).Contains("is a value, not an asset"));
    EXPECT_TRUE(ResolveError(*Graph, "Anim.Graph.Skeleton.Bones", false).Contains("a path follows only one"));
    EXPECT_TRUE(ResolveError(*Graph, "Gone.Value", false).Contains("which does not exist"));
    EXPECT_TRUE(ResolveError(*Graph, "Nobody.Value", false).Contains("names no input"));
}
