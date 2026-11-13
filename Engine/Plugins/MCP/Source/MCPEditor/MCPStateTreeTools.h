#pragma once

#include "AI/StateTree/StateTree.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPStateTreeTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SCreateStateTreeParams
    {
        GENERATED_BODY()

        // Folder to create it in, such as /Game/Content/AI.
        PROPERTY()
        FString Folder;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SStateTreeRefParams
    {
        GENERATED_BODY()

        // GUID or content path of the state tree.
        PROPERTY()
        FString StateTree;
    };

    REFLECT()
    struct MCPEDITOR_API SAddStateTreeStateParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString StateTree;

        // Name is required and unique; Parent names an existing state, or is empty for a top level one.
        PROPERTY()
        SStateTreeState State;
    };

    REFLECT()
    struct MCPEDITOR_API SAddStateTreeNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString StateTree;

        // A struct deriving from SStateTreeTaskBase, SStateTreeConditionBase or SStateTreeEvaluatorBase.
        PROPERTY()
        FString Type;

        // Unique in the tree, and how set_binding and binding sources name this node.
        PROPERTY()
        FString Name;

        // The state a task or condition goes on, ignored for an evaluator.
        PROPERTY()
        FString State;

        // A condition goes on this transition of State, or among its enter conditions when negative.
        PROPERTY()
        int32 Transition = -1;

        // The node's fields as a JSON object keyed by field name.
        PROPERTY(RawJson)
        FString Properties;
    };

    REFLECT()
    struct MCPEDITOR_API SSetStateTreeBindingParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString StateTree;

        // Name of the node whose instance data field the binding fills.
        PROPERTY()
        FString Node;

        // Replaces the node's binding of the same TargetField; an empty SourceField removes it.
        PROPERTY()
        SStateTreeBinding Binding;
    };

    REFLECT()
    struct MCPEDITOR_API SAddStateTreeTransitionParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString StateTree;

        // The state the transition leaves.
        PROPERTY()
        FString State;

        PROPERTY()
        SStateTreeTransition Transition;
    };

    REFLECT()
    struct MCPEDITOR_API SStateTreeAddResult
    {
        GENERATED_BODY()

        // Position in the list it joined; a transition's is what add_node takes as Transition.
        PROPERTY()
        int32 Index = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SStateTreeReport
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // The authored tree, its states, evaluators and parameters, as JSON.
        PROPERTY()
        FString Tree;

        PROPERTY()
        bool bCompiled = false;

        // One entry per problem the last compile found, empty when it compiled.
        PROPERTY()
        TVector<FString> Errors;
    };

    REFLECT()
    struct MCPEDITOR_API SStateTreeInstanceParams
    {
        GENERATED_BODY()

        // An entity with a state tree component, in the scene on show, which is the play world during Play.
        PROPERTY()
        FString Entity;
    };

    REFLECT()
    struct MCPEDITOR_API SStateTreeInstanceResult
    {
        GENERATED_BODY()

        // GUID of the tree the instance runs.
        PROPERTY()
        FString StateTree;

        PROPERTY()
        EStateTreeRunStatus Status = EStateTreeRunStatus::Running;

        // State names from the top level state down to the active leaf.
        PROPERTY()
        TVector<FString> ActivePath;

        // Sleeping until an event or a wake time, so it is not ticked.
        PROPERTY()
        bool bWaiting = false;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterStateTreeTools(FStringView Owner);
    }
}
