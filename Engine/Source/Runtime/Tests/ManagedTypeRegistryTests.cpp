#include <gtest/gtest.h>

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Scripting/ManagedTypeRegistry.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
    // Records that it ran and what it was handed, which is all the ordering contract actually asserts.
    class FRecordingStage final : public IManagedTypeCompiler
    {
    public:

        FRecordingStage(const char* InName, TVector<FString>& InLog)
            : Name(InName)
            , Log(&InLog)
        {}

        const char* GetName() const override { return Name; }

        void PreUnload() override { Log->Add(FString(Name) + "::PreUnload"); }

        void UnloadAborted() override { Log->Add(FString(Name) + "::UnloadAborted"); }

        void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias>) override
        {
            Log->Emplace(Name);
            SeenCount = Definitions.Num();

            for (const FManagedTypeDefinition& Definition : Definitions)
            {
                if (Definition.Kind == EManagedTypeKind::ScriptableClass)
                {
                    ++SeenScriptable;
                }
            }
        }

        const char*      Name;
        TVector<FString>* Log;
        size_t           SeenCount = 0;
        size_t           SeenScriptable = 0;
    };

    // The registry is a process singleton, so a test puts back what it found.
    struct FScopedStages
    {
        FScopedStages()  { FManagedTypeRegistry::Get().ResetForTesting(); }
        ~FScopedStages() { FManagedTypeRegistry::Get().ResetForTesting(); RegisterBuiltInManagedTypeStages(); }
    };

    FManagedTypeDefinition MakeScriptable(const char* TypeName)
    {
        FManagedTypeDefinition Definition;
        Definition.Kind = EManagedTypeKind::ScriptableClass;
        Definition.TypeName = FName(TypeName);
        return Definition;
    }
}

// The ordering used to live in comments beside a hand-written call sequence, so nothing could check it.
TEST(ManagedTypeRegistry, StagesRunInRegistrationOrder)
{
    const FScopedStages Scope;

    TVector<FString> Log;
    FRecordingStage First("First", Log);
    FRecordingStage Second("Second", Log);
    FRecordingStage Third("Third", Log);

    FManagedTypeRegistry& Registry = FManagedTypeRegistry::Get();
    Registry.Register(&First);
    Registry.Register(&Second);
    Registry.Register(&Third);

    TVector<FManagedTypeDefinition> Definitions;
    Registry.CompileAll(Definitions);

    ASSERT_EQ(Log.Num(), 3);
    EXPECT_EQ(Log[0], FString("First"));
    EXPECT_EQ(Log[1], FString("Second"));
    EXPECT_EQ(Log[2], FString("Third"));
}

// The gather-once property: one set of definitions reaches every stage, rather than each crossing for its own.
TEST(ManagedTypeRegistry, EveryStageSeesTheSameDefinitions)
{
    const FScopedStages Scope;

    TVector<FString> Log;
    FRecordingStage First("First", Log);
    FRecordingStage Second("Second", Log);

    FManagedTypeRegistry& Registry = FManagedTypeRegistry::Get();
    Registry.Register(&First);
    Registry.Register(&Second);

    TVector<FManagedTypeDefinition> Definitions;
    Definitions.Add(MakeScriptable("Alpha"));
    Definitions.Add(MakeScriptable("Beta"));

    Registry.CompileAll(Definitions);

    EXPECT_EQ(First.SeenCount, 2u);
    EXPECT_EQ(Second.SeenCount, 2u);
    EXPECT_EQ(First.SeenScriptable, 2u);
    EXPECT_EQ(Second.SeenScriptable, 2u);
}

TEST(ManagedTypeRegistry, CompilingWithNoStagesOrNoTypesIsHarmless)
{
    const FScopedStages Scope;

    TVector<FManagedTypeDefinition> Definitions;
    Definitions.Add(MakeScriptable("Alpha"));
    FManagedTypeRegistry::Get().CompileAll(Definitions);

    TVector<FString> Log;
    FRecordingStage Only("Only", Log);
    FManagedTypeRegistry::Get().Register(&Only);

    TVector<FManagedTypeDefinition> Empty;
    FManagedTypeRegistry::Get().CompileAll(Empty);

    ASSERT_EQ(Log.Num(), 1) << "a stage must still run for a generation that declared nothing";
    EXPECT_EQ(Only.SeenCount, 0u);
}

// Each position is a real constraint, since a struct field may name a class shell and a class may hold a struct by value.
TEST(ManagedTypeRegistry, TheBuiltInStagesAreRegisteredInTheOrderTheyMustRun)
{
    FManagedTypeRegistry::Get().ResetForTesting();
    RegisterBuiltInManagedTypeStages();

    const TSpan<IManagedTypeCompiler* const> Stages = FManagedTypeRegistry::Get().GetStages();
    ASSERT_EQ(Stages.Num(), 6);
    EXPECT_STREQ(Stages[0]->GetName(), "Redirects");
    EXPECT_STREQ(Stages[1]->GetName(), "Enums");
    EXPECT_STREQ(Stages[2]->GetName(), "ClassShells");
    EXPECT_STREQ(Stages[3]->GetName(), "Structs");
    EXPECT_STREQ(Stages[4]->GetName(), "ScriptableClasses");
    EXPECT_STREQ(Stages[5]->GetName(), "StateTrees");
}

// Reverse, because a stage built on an earlier one has to come down before the thing it was built from.
TEST(ManagedTypeRegistry, PreUnloadRunsInReverseRegistrationOrder)
{
    const FScopedStages Scope;

    TVector<FString> Log;
    FRecordingStage First("First", Log);
    FRecordingStage Second("Second", Log);
    FRecordingStage Third("Third", Log);

    FManagedTypeRegistry& Registry = FManagedTypeRegistry::Get();
    Registry.Register(&First);
    Registry.Register(&Second);
    Registry.Register(&Third);

    Registry.PreUnloadAll();

    ASSERT_EQ(Log.Num(), 3);
    EXPECT_EQ(Log[0], FString("Third::PreUnload"));
    EXPECT_EQ(Log[1], FString("Second::PreUnload"));
    EXPECT_EQ(Log[2], FString("First::PreUnload"));
}

// A failed compile keeps the previous generation alive, so the teardown has to be undone rather than
// completed. This used to be one subsystem calling itself back from inside the host's error branch.
TEST(ManagedTypeRegistry, AnAbortedUnloadPutsEveryStageBackInRegistrationOrder)
{
    const FScopedStages Scope;

    TVector<FString> Log;
    FRecordingStage First("First", Log);
    FRecordingStage Second("Second", Log);

    FManagedTypeRegistry& Registry = FManagedTypeRegistry::Get();
    Registry.Register(&First);
    Registry.Register(&Second);

    Registry.PreUnloadAll();
    Registry.UnloadAbortedAll();

    ASSERT_EQ(Log.Num(), 4);
    EXPECT_EQ(Log[0], FString("Second::PreUnload"));
    EXPECT_EQ(Log[1], FString("First::PreUnload"));
    EXPECT_EQ(Log[2], FString("First::UnloadAborted"));
    EXPECT_EQ(Log[3], FString("Second::UnloadAborted"));
}

// Nothing in the unload half may run Compile, or a failed load would rebuild against types that never loaded.
TEST(ManagedTypeRegistry, TheUnloadHalfNeverCompiles)
{
    const FScopedStages Scope;

    TVector<FString> Log;
    FRecordingStage Only("Only", Log);
    FManagedTypeRegistry::Get().Register(&Only);

    FManagedTypeRegistry::Get().PreUnloadAll();
    FManagedTypeRegistry::Get().UnloadAbortedAll();

    EXPECT_EQ(Only.SeenCount, 0u);
    for (const FString& Entry : Log)
    {
        EXPECT_NE(Entry, FString("Only")) << "Compile logs the bare name, and it must not have run";
    }
}
