#include <gtest/gtest.h>

#include "Agent/AgentToolRegistry.h"
#include "World/Entity/Components/NameComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace
{
    // The Tests module has reflection off, so params come from types the engine already reflects.
    using FParams = SNameComponent;
    using FResult = SNameComponent;

    constexpr const char* GOwner = "AgentToolRegistryTests";

    // Every test shares the one registry, so each cleans up after itself rather than assuming it is empty.
    struct FScopedOwner
    {
        ~FScopedOwner() { FToolRegistry::Get().UnregisterOwner(GOwner); }
    };

    bool RegisterEcho(FStringView Name)
    {
        return FToolRegistry::Get().Register<FParams, FResult>(
            GOwner, Name, "Copies the name through.",
            EToolEffect::ReadOnly, EToolThread::Any,
            [](const FParams& In, FResult& Out)
            {
                Out.Name = In.Name;
                return FToolResult::Ok("copied");
            });
    }

    FToolResult Call(FStringView Name, const FParams& In, FResult& Out)
    {
        FTool Tool;
        if (!FToolRegistry::Get().TryGetTool(Name, Tool))
        {
            return FToolResult::Error("not registered");
        }

        return Tool.Invoke(&In, &Out);
    }
}

TEST(AgentToolRegistry, ARegisteredToolIsFoundAndCounted)
{
    FScopedOwner Cleanup;

    const int32 Before = FToolRegistry::Get().GetToolCount();
    ASSERT_TRUE(RegisterEcho("test.echo"));

    EXPECT_TRUE(FToolRegistry::Get().Contains("test.echo"));
    EXPECT_EQ(FToolRegistry::Get().GetToolCount(), Before + 1);
}

TEST(AgentToolRegistry, TheHandlerReceivesTypedParamsAndFillsTypedResults)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.echo"));

    FParams In;
    In.Name = "Torch";

    FResult Out;
    const FToolResult Result = Call("test.echo", In, Out);

    EXPECT_FALSE(Result.bIsError);
    EXPECT_EQ(Result.Text, "copied");
    EXPECT_EQ(Out.Name, FName("Torch"));
}

TEST(AgentToolRegistry, TheDescriptorCarriesTheReflectedTypesAndFlags)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.echo"));

    FTool Tool;
    ASSERT_TRUE(FToolRegistry::Get().TryGetTool("test.echo", Tool));

    EXPECT_EQ(Tool.ParamsType, SNameComponent::StaticStruct());
    EXPECT_EQ(Tool.ResultType, SNameComponent::StaticStruct());
    EXPECT_EQ(Tool.Owner, GOwner);
    EXPECT_EQ(Tool.Description, "Copies the name through.");
    EXPECT_EQ(Tool.Effect, EToolEffect::ReadOnly);
    EXPECT_EQ(Tool.Thread, EToolThread::Any);
}

// A tool with nothing structured to return still has to be callable.
TEST(AgentToolRegistry, ATextOnlyToolHasNoResultType)
{
    FScopedOwner Cleanup;

    ASSERT_TRUE(FToolRegistry::Get().Register<FParams>(
        GOwner, "test.text_only", "Answers with text alone.",
        EToolEffect::ReadOnly, EToolThread::Any,
        [](const FParams& In)
        {
            return FToolResult::Ok(FString("saw ") + In.Name.ToString().c_str());
        }));

    FTool Tool;
    ASSERT_TRUE(FToolRegistry::Get().TryGetTool("test.text_only", Tool));
    EXPECT_EQ(Tool.ResultType, nullptr);

    FParams In;
    In.Name = "Lamp";

    const FToolResult Result = Tool.Invoke(&In, nullptr);
    EXPECT_FALSE(Result.bIsError);
    EXPECT_NE(Result.Text.find("Lamp"), FString::npos);
}

TEST(AgentToolRegistry, AFailingToolReportsItselfWithoutThrowing)
{
    FScopedOwner Cleanup;

    ASSERT_TRUE(FToolRegistry::Get().Register<FParams>(
        GOwner, "test.refuse", "Always refuses.",
        EToolEffect::ReadOnly, EToolThread::Any,
        [](const FParams&) { return FToolResult::Error("that entity does not exist"); }));

    FTool Tool;
    ASSERT_TRUE(FToolRegistry::Get().TryGetTool("test.refuse", Tool));

    FParams In;
    const FToolResult Result = Tool.Invoke(&In, nullptr);

    EXPECT_TRUE(Result.bIsError);
    EXPECT_EQ(Result.Text, "that entity does not exist");
}

// Two owners silently sharing a name would leave both believing they own it.
TEST(AgentToolRegistry, ADuplicateNameIsRefusedRatherThanReplacing)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.echo"));

    EXPECT_FALSE(RegisterEcho("test.echo"));
    EXPECT_EQ(FToolRegistry::Get().GetToolCount() > 0, true);
}

TEST(AgentToolRegistry, AnEmptyNameIsRefused)
{
    FScopedOwner Cleanup;
    EXPECT_FALSE(RegisterEcho(""));
}

TEST(AgentToolRegistry, ANullHandlerIsRefused)
{
    FScopedOwner Cleanup;

    EXPECT_FALSE(FToolRegistry::Get().Register<FParams>(
        GOwner, "test.null", "Has no body.",
        EToolEffect::ReadOnly, EToolThread::Any,
        TFunction<FToolResult(const FParams&)>()));

    EXPECT_FALSE(FToolRegistry::Get().Contains("test.null"));
}

TEST(AgentToolRegistry, UnregisteringOneToolLeavesTheRest)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.first"));
    ASSERT_TRUE(RegisterEcho("test.second"));

    EXPECT_TRUE(FToolRegistry::Get().Unregister("test.first"));
    EXPECT_FALSE(FToolRegistry::Get().Unregister("test.first"));
    EXPECT_FALSE(FToolRegistry::Get().Contains("test.first"));
    EXPECT_TRUE(FToolRegistry::Get().Contains("test.second"));
}

// Plugin unload and script reload lean on this, so an owner takes exactly its own tools away.
TEST(AgentToolRegistry, UnregisteringAnOwnerDropsOnlyItsTools)
{
    FScopedOwner Cleanup;

    ASSERT_TRUE(RegisterEcho("test.mine"));

    ASSERT_TRUE(FToolRegistry::Get().Register<FParams>(
        "SomeOtherOwner", "test.theirs", "Belongs to someone else.",
        EToolEffect::ReadOnly, EToolThread::Any,
        [](const FParams&) { return FToolResult::Ok("ok"); }));

    EXPECT_EQ(FToolRegistry::Get().UnregisterOwner(GOwner), 1);
    EXPECT_FALSE(FToolRegistry::Get().Contains("test.mine"));
    EXPECT_TRUE(FToolRegistry::Get().Contains("test.theirs"));

    EXPECT_EQ(FToolRegistry::Get().UnregisterOwner("SomeOtherOwner"), 1);
}

TEST(AgentToolRegistry, ForEachVisitsRegisteredTools)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.visited"));

    bool bSaw = false;
    FToolRegistry::Get().ForEachTool([&bSaw](const FTool& Tool)
    {
        bSaw = bSaw || Tool.Name == "test.visited";
    });

    EXPECT_TRUE(bSaw);
}

TEST(AgentToolRegistry, AMissingToolIsNotHandedBack)
{
    FTool Tool;
    EXPECT_FALSE(FToolRegistry::Get().TryGetTool("test.nothing_here", Tool));
    EXPECT_FALSE(FToolRegistry::Get().Contains("test.nothing_here"));
}

// The instance is what the transport fills from JSON, so it has to be live memory of the right shape.
TEST(AgentToolRegistry, AStructInstanceConstructsAndDestroysItsType)
{
    FStructInstance Instance(SNameComponent::StaticStruct());

    ASSERT_TRUE(Instance.IsValid());
    EXPECT_EQ(Instance.GetType(), SNameComponent::StaticStruct());

    SNameComponent* Typed = static_cast<SNameComponent*>(Instance.Get());
    Typed->Name = "Constructed";
    EXPECT_EQ(Typed->Name, FName("Constructed"));
}

TEST(AgentToolRegistry, AStructInstanceOfNothingIsInvalidRatherThanCrashing)
{
    FStructInstance Instance(nullptr);
    EXPECT_FALSE(Instance.IsValid());
    EXPECT_EQ(Instance.Get(), nullptr);
}

// An instance built blind from the descriptor is what tools/call actually does.
TEST(AgentToolRegistry, AToolRunsAgainstInstancesBuiltFromItsDescriptor)
{
    FScopedOwner Cleanup;
    ASSERT_TRUE(RegisterEcho("test.blind"));

    FTool Tool;
    ASSERT_TRUE(FToolRegistry::Get().TryGetTool("test.blind", Tool));

    FStructInstance Params(Tool.ParamsType);
    FStructInstance Result(Tool.ResultType);

    ASSERT_TRUE(Params.IsValid());
    ASSERT_TRUE(Result.IsValid());

    static_cast<SNameComponent*>(Params.Get())->Name = "Blind";

    const FToolResult Outcome = Tool.Invoke(Params.Get(), Result.Get());

    EXPECT_FALSE(Outcome.bIsError);
    EXPECT_EQ(static_cast<SNameComponent*>(Result.Get())->Name, FName("Blind"));
}
