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

        void Compile(TSpan<const FManagedTypeDefinition> Definitions) override
        {
            Log->push_back(FString(Name));
            SeenCount = Definitions.size();

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

    ASSERT_EQ(Log.size(), 3u);
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
    Definitions.push_back(MakeScriptable("Alpha"));
    Definitions.push_back(MakeScriptable("Beta"));

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
    Definitions.push_back(MakeScriptable("Alpha"));
    FManagedTypeRegistry::Get().CompileAll(Definitions);

    TVector<FString> Log;
    FRecordingStage Only("Only", Log);
    FManagedTypeRegistry::Get().Register(&Only);

    TVector<FManagedTypeDefinition> Empty;
    FManagedTypeRegistry::Get().CompileAll(Empty);

    ASSERT_EQ(Log.size(), 1u) << "a stage must still run for a generation that declared nothing";
    EXPECT_EQ(Only.SeenCount, 0u);
}

// The engine's own sequence: classes, then data structs, then renderers. Asserted here because the reason
// for each position is a real constraint, not a preference.
TEST(ManagedTypeRegistry, TheBuiltInStagesAreRegisteredInTheOrderTheyMustRun)
{
    FManagedTypeRegistry::Get().ResetForTesting();
    RegisterBuiltInManagedTypeStages();

    const TSpan<IManagedTypeCompiler* const> Stages = FManagedTypeRegistry::Get().GetStages();
    ASSERT_EQ(Stages.size(), 3u);
    EXPECT_STREQ(Stages[0]->GetName(), "ScriptableClasses");
    EXPECT_STREQ(Stages[1]->GetName(), "DataStructs");
    EXPECT_STREQ(Stages[2]->GetName(), "RenderScenes");
}
