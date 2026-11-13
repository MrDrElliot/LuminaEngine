#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"

#include "MCPAnimGraphTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SAnimGraphCanvasParams
    {
        GENERATED_BODY()

        // GUID or content path of the animation graph.
        PROPERTY()
        FString AnimGraph;

        // Node ids leading to the canvas: a State Machine id enters its states, a State id its blend tree. Empty is root.
        PROPERTY()
        TVector<int64> GraphPath;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeTypeInfo
    {
        GENERATED_BODY()

        // Pass this to animgraph.node as NodeType.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;

        PROPERTY()
        FString Description;
    };

    REFLECT()
    struct MCPEDITOR_API SListAnimGraphNodeTypesParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        // Canvas whose node types to list; blend trees and state canvases take different ones.
        PROPERTY()
        TVector<int64> GraphPath;

        // Substring of the type name or category. Empty lists every type.
        PROPERTY()
        FString Contains;
    };

    REFLECT()
    struct MCPEDITOR_API SListAnimGraphNodeTypesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SAnimGraphNodeTypeInfo> Types;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeInfo
    {
        GENERATED_BODY()

        // Id for any tool taking a node, and for GraphPath to enter its canvas.
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        FString Type;

        // Title shown on the canvas, which carries a state or machine name.
        PROPERTY()
        FString Title;

        // True when the node owns a canvas; State Machine and State nodes create one on first write.
        PROPERTY()
        bool bHasCanvas = false;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;

        // Current values of this node's settable fields, as JSON.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphTransitionInfo
    {
        GENERATED_BODY()

        // A State, or the Any State node for an edge checked from every state.
        PROPERTY()
        int64 FromNode = 0;

        PROPERTY()
        int64 ToNode = 0;

        // Conditions, blend duration and priority, as JSON.
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeAnimGraphResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // CAnimationGraphNodeGraph for a blend tree, CAnimStateMachineGraph for a state canvas.
        PROPERTY()
        FString CanvasClass;

        PROPERTY()
        TVector<SAnimGraphNodeInfo> Nodes;

        // State-to-state edges, present only on a state machine canvas.
        PROPERTY()
        TVector<SAnimGraphTransitionInfo> Transitions;
    };

    REFLECT()
    enum class EAnimGraphNodeAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        EAnimGraphNodeAction Action = EAnimGraphNodeAction::Add;

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        // Add only. Node type name, from animgraph.list_node_types.
        PROPERTY()
        FString NodeType;

        // Add only.
        PROPERTY()
        float X = 0.0f;

        // Add only.
        PROPERTY()
        float Y = 0.0f;

        // Remove only.
        PROPERTY()
        int64 Node = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeResult
    {
        GENERATED_BODY()

        // Add only. Id of the new node.
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;
    };

    REFLECT()
    enum class EAnimGraphLinkAction : uint8
    {
        Connect,
        Disconnect,
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphLinkParams
    {
        GENERATED_BODY()

        PROPERTY()
        EAnimGraphLinkAction Action = EAnimGraphLinkAction::Connect;

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        // Connect only. Node the value comes from; on a state canvas, the state a transition leaves.
        PROPERTY()
        int64 FromNode = 0;

        // Connect only. Output pin name on FromNode, as animgraph.describe reports it.
        PROPERTY()
        FString FromPin;

        // Connect only.
        PROPERTY()
        int64 ToNode = 0;

        // Connect only. Input pin name on ToNode.
        PROPERTY()
        FString ToPin;

        // Disconnect only.
        PROPERTY()
        int64 Node = 0;

        // Disconnect only. Pin whose links are cleared, in either direction.
        PROPERTY()
        FString Pin;
    };

    REFLECT()
    struct MCPEDITOR_API SSetAnimGraphNodePropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        PROPERTY()
        int64 Node = 0;

        // Field path on the node, such as StateName, Animation or PinDefaults.
        PROPERTY()
        FString Path;

        // New value as JSON: a number, a quoted string or an object.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSetAnimGraphTransitionPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        // Path to the state machine canvas holding the transition.
        PROPERTY()
        TVector<int64> GraphPath;

        PROPERTY()
        int64 FromNode = 0;

        PROPERTY()
        int64 ToNode = 0;

        // Field path on the transition, such as Conditions, BlendDuration or Priority.
        PROPERTY()
        FString Path;

        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphPropertyResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Previous;

        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SCompileAnimGraphParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphCompileResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bSucceeded = false;

        // Each as "[Name] Description (node Id)". Node ids are local to the canvas that raised them.
        PROPERTY()
        TVector<FString> Errors;

        PROPERTY()
        TVector<FString> Warnings;
    };

    namespace MCP
    {
        MCPEDITOR_API void RegisterAnimGraphTools(FStringView Owner);
    }
}
