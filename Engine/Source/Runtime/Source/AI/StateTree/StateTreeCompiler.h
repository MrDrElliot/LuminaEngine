#pragma once

#include "StateTreeTypes.h"
#include "Containers/Name.h"
#include "Containers/Optional.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CStateTree;
    class CStruct;
    class FFunction;
    class FProperty;
    struct SStateTreeState;

    inline constexpr uint16 kStateTreeNoIndex   = 0xFFFF;
    inline constexpr int32  kStateTreeMaxDepth  = 8;
    inline constexpr int32  kStateTreeMaxEvents = 8;

    // One binding resolved to bytes, both offsets relative to the start of an instance's data block.
    struct FStateTreeCopy
    {
        uint32 SrcOffset = 0;
        uint32 DstOffset = 0;
        uint32 Size      = 0;
        // Runs before the copy, whose source is then the call's return slot.
        uint16 Call      = kStateTreeNoIndex;
    };

    // A function binding, its frame preallocated in the instance block and its arguments copied in before each call.
    struct FStateTreeCall
    {
        const FFunction* Function      = nullptr;
        // The struct a member function runs on, ignored for a static one.
        uint32           SelfOffset    = 0;
        uint32           FrameOffset   = 0;
        uint16           FirstArgument = 0;
        uint16           NumArguments  = 0;
    };

    struct FStateTreeNode
    {
        // A task, condition or evaluator, the table referencing the node says which.
        FInstancedStruct Definition;
        // Held, since a reload that reshapes a C# node frees its old type while instances still hold data laid out by it.
        TStrongObjectPtr<CStruct> InstanceDataType;
        uint32           InstanceDataOffset = 0;
        uint16           FirstCopy          = 0;
        uint16           NumCopies          = 0;
        // Index into FStateTreeRuntime::Parameters of the tree this node was authored in.
        uint16           Scope              = 0;
        // A C# node, which is its own instance data and runs through the script bridge.
        bool             bScript            = false;
    };

    // Stored in depth first order, so a state's subtree follows it.
    struct FStateTreeRuntimeState
    {
        FName               Name;
        EStateTreeStateKind Kind               = EStateTreeStateKind::State;
        uint16              Parent             = kStateTreeNoIndex;
        uint16              FirstChild         = kStateTreeNoIndex;
        uint16              NextSibling        = kStateTreeNoIndex;
        uint16              FirstEnterCondition = 0;
        uint16              NumEnterConditions = 0;
        uint16              FirstTask          = 0;
        uint16              NumTasks           = 0;
        uint16              FirstTransition    = 0;
        uint16              NumTransitions     = 0;
        // A linked state's batch into its linked tree's parameters.
        uint16              FirstCopy          = 0;
        uint16              NumCopies          = 0;
        // Index into FStateTreeRuntime::Parameters of the tree this state was authored in.
        uint16              Scope              = 0;
        uint8               Depth              = 0;
        // A task here needs ticking or a transition here fires on tick, so the instance cannot sleep.
        bool                bNeedsTick         = false;
    };

    struct FStateTreeRuntimeTransition
    {
        EStateTreeTransitionTrigger Trigger          = EStateTreeTransitionTrigger::OnCompleted;
        // NextSibling and Parent are resolved, so this is State, Succeeded or Failed.
        EStateTreeTransitionTarget  Target           = EStateTreeTransitionTarget::State;
        // For Succeeded and Failed inside a linked tree, the linking state they complete.
        uint16                      TargetState      = kStateTreeNoIndex;
        uint16                      FirstCondition   = 0;
        uint16                      NumConditions    = 0;
        uint32                      EventTag         = 0;
        int32                       Priority         = 0;
        float                       Delay            = 0.0f;
        float                       DelayRandomRange = 0.0f;
    };

    // One tree's parameter struct in the instance block.
    struct FStateTreeParameters
    {
        FInstancedStruct Defaults;
        uint32           Offset = 0;
    };

    // Everything an instance runs from; node ranges index Nodes and every node's data lives in one block.
    struct FStateTreeRuntime
    {
        TVector<FStateTreeRuntimeState>      States;
        TVector<FStateTreeRuntimeTransition> Transitions;
        TVector<FStateTreeNode>              Nodes;
        TVector<FStateTreeCopy>              Copies;
        TVector<FStateTreeCall>              Calls;

        uint16 NumEvaluators = 0;

        // SStateTreeContextData sits at offset 0. One entry per tree, the root tree first and then every linked tree.
        TVector<FStateTreeParameters> Parameters;

        uint32 InstanceDataSize      = 0;
        uint32 InstanceDataAlignment = 1;

        // The managed entry every C# node runs through, null when the tree has none.
        void* ScriptDispatch = nullptr;

        // Set when the tree recompiles, so instances still running this layout stop instead of ticking on.
        bool bSuperseded = false;

        NODISCARD bool IsValid() const { return !States.IsEmpty(); }
    };

    class FStateTreeCompiler
    {
    public:

        // Out is left empty when any error is found.
        RUNTIME_API bool Compile(const CStateTree& Tree, FStateTreeRuntime& Out);

        NODISCARD const TVector<FString>& GetErrors() const { return Errors; }

        // Whether a binding can copy Source into Target, the same test the editor's binding picker filters by.
        RUNTIME_API static bool CanBind(const FProperty& Target, const FProperty& Source);

    private:

        void Error(FString Message);
        FString DescribeNode(uint16 NodeIndex) const;

        // Places a struct or a call frame in the instance block and returns its offset.
        uint32 Allocate(uint32 Size, uint32 Alignment);
        uint32 Allocate(const CStruct* Type);

        // Who reads a binding, where its copy lands, and the point visibility is judged from.
        struct FBindingReader
        {
            FString        Description;
            uint16         Node   = 0;
            uint16         State  = kStateTreeNoIndex;
            uint16         Scope  = 0;
            const CStruct* Type   = nullptr;
            uint32         Offset = 0;
        };

        // Adds Tree's states below RuntimeParent, recursing into every tree it links.
        void AddStates(const CStateTree& Tree, const FInstancedStruct& Parameters, uint16 RuntimeParent, uint8 Depth);
        void AddNode(const FInstancedStruct& Definition, uint16 State, uint16 Scope, const char* Slot);
        void AddNodes();
        void AddTransitions();
        void ResolveBindings();
        // Argument copies go ahead of the batch, so the batch stays one contiguous range.
        void ResolveBindings(const TVector<SStateTreeBinding>& Bindings, const FBindingReader& Reader, uint16& FirstCopy, uint16& NumCopies);

        // An evaluator before the reader, or a task on the reader's active path that runs before it.
        bool IsVisibleSource(uint16 Source, const FBindingReader& Reader, EStateTreeBindingSource Kind) const;
        // The struct Path reads from and its offset in the block, null when it is missing or not visible.
        const CStruct* FindSource(const FBindingReader& Reader, const SStateTreeBindingPath& Path, uint32& OutOffset) const;
        TOptional<FStateTreeCopy> ResolveBinding(const FBindingReader& Reader, const SStateTreeBinding& Binding);
        TOptional<uint16> AddCall(const FBindingReader& Reader, const SStateTreeBinding& Binding, const FFunction& Function, uint32 SelfOffset);

        FStateTreeRuntime* Runtime = nullptr;
        TVector<FString>   Errors;

        // Authored state of each runtime state.
        TVector<const SStateTreeState*> SourceStates;
        // Runtime state of each node, kStateTreeNoIndex for evaluators.
        TVector<uint16> NodeStates;
        // The tree of each scope, and the trees being added right now for cycle detection.
        TVector<const CStateTree*> ScopeTrees;
        TVector<const CStateTree*> TreeStack;
    };
}
