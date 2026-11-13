#pragma once

#include "StateTreeCompiler.h"
#include "StateTreeTypes.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "StateTree.generated.h"

namespace Lumina
{
    class CStateTree;

    // Bumped when the authoring layout changes; a tree saved by a newer build fails to compile.
    inline constexpr int32 kStateTreeVersion = 1;

    REFLECT()
    struct RUNTIME_API SStateTreeTransition
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnCompleted;

        // An event matches when it is this tag or a descendant of it.
        PROPERTY(Editable)
        SHierarchicalTag EventTag;

        PROPERTY(Editable)
        EStateTreeTransitionTarget Target = EStateTreeTransitionTarget::State;

        PROPERTY(Editable)
        FName TargetState;

        // Higher wins; a tie goes to the deeper state, then to the earlier transition.
        PROPERTY(Editable)
        int32 Priority = 0;

        PROPERTY(Editable)
        float Delay = 0.0f;

        // The delay is drawn from Delay plus or minus this.
        PROPERTY(Editable)
        float DelayRandomRange = 0.0f;

        PROPERTY(Editable)
        TVector<TInstancedStruct<SStateTreeConditionBase>> Conditions;
    };

    // States are listed flat; Parent names the containing state and list order is sibling order.
    REFLECT()
    struct RUNTIME_API SStateTreeState
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName Name;

        // None for a top level state.
        PROPERTY(Editable)
        FName Parent;

        PROPERTY(Editable)
        EStateTreeStateKind Kind = EStateTreeStateKind::State;

        PROPERTY(Editable)
        TStrongObjectPtr<CStateTree> LinkedSubtree;

        // Values for the linked tree's parameter struct, its defaults when empty.
        PROPERTY(Editable)
        TInstancedStruct<SStateTreeParametersBase> LinkedParameters;

        // Copied into fields of the linked tree's parameters whenever this state enters or ticks.
        PROPERTY(Editable)
        TVector<SStateTreeBinding> ParameterBindings;

        PROPERTY(Editable)
        TVector<TInstancedStruct<SStateTreeConditionBase>> EnterConditions;

        PROPERTY(Editable)
        TVector<TInstancedStruct<SStateTreeTaskBase>> Tasks;

        PROPERTY(Editable)
        TVector<SStateTreeTransition> Transitions;
    };

    // Authored form, saved as is and compiled into a flat FStateTreeRuntime on load and on every edit.
    REFLECT(MinimalAPI)
    class CStateTree : public CObject
    {
        GENERATED_BODY()
    public:

        PROPERTY(Hidden)
        int32 Version = kStateTreeVersion;

        PROPERTY(Editable)
        TVector<SStateTreeState> States;

        PROPERTY(Editable)
        TVector<TInstancedStruct<SStateTreeEvaluatorBase>> Evaluators;

        // Default values an instance starts from unless it passes its own struct of the same type.
        PROPERTY(Editable)
        TInstancedStruct<SStateTreeParametersBase> Parameters;

        bool IsAsset() const override { return true; }

        RUNTIME_API void PostLoad() override;

        RUNTIME_API void PostPropertyChange(FProperty* ChangedProperty) override;

        // Rebuilds this runtime and every loaded tree linking this one, returning whether this tree compiled.
        RUNTIME_API bool Compile(bool bLogErrors = true);

        NODISCARD TSharedPtr<const FStateTreeRuntime> GetRuntime() const { return Runtime; }

        NODISCARD const TVector<FString>& GetCompileErrors() const { return CompileErrors; }

    private:

        // Rebuilds the runtime, leaving it empty when the tree does not compile.
        bool CompileSelf(bool bLogErrors);

        // Each compile makes a new runtime; running instances keep the one whose block layout they were started with.
        TSharedPtr<FStateTreeRuntime> Runtime = MakeShared<FStateTreeRuntime>();
        TVector<FString> CompileErrors;
    };
}
