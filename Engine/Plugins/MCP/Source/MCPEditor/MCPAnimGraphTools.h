#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPMaterialTools.h"

#include "MCPAnimGraphTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SAnimGraphCanvasParams
    {
        GENERATED_BODY()

        /** GUID of the animation graph asset, from assets.search. */
        PROPERTY()
        FString AnimGraph;

        /** Node ids leading from the root blend tree to the canvas to act on. A State Machine node's id
         *  enters its state canvas, and a State's id there enters that state's blend tree. Empty is the root. */
        PROPERTY()
        TVector<int64> GraphPath;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeTypeInfo
    {
        GENERATED_BODY()

        /** Type name to pass to animgraph.add_node. */
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

        /** Canvas whose node types to list, since blend trees and state canvases take different ones. */
        PROPERTY()
        TVector<int64> GraphPath;

        /** Only types whose name or category contains this. Empty lists every one of them. */
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

        /** Id to pass to any tool taking a node, and to GraphPath to enter its canvas. */
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        FString Type;

        /** Title shown on the canvas, which carries a state's or machine's name. */
        PROPERTY()
        FString Title;

        /** True when this node owns a canvas already; State Machine and State nodes create one on first write. */
        PROPERTY()
        bool bHasCanvas = false;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;

        /** Current values of this node's settable fields, as JSON. */
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphTransitionInfo
    {
        GENERATED_BODY()

        /** Source node id: a State, or the Any State node for an edge checked from every state. */
        PROPERTY()
        int64 FromNode = 0;

        PROPERTY()
        int64 ToNode = 0;

        /** Conditions, blend duration and priority, as JSON. */
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeAnimGraphResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        /** CAnimationGraphNodeGraph for a blend tree, CAnimStateMachineGraph for a state canvas. */
        PROPERTY()
        FString CanvasClass;

        PROPERTY()
        TVector<SAnimGraphNodeInfo> Nodes;

        /** State-to-state edges, present only on a state machine canvas. */
        PROPERTY()
        TVector<SAnimGraphTransitionInfo> Transitions;
    };

    REFLECT()
    struct MCPEDITOR_API SAddAnimGraphNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        /** Node type name, from animgraph.list_node_types. */
        PROPERTY()
        FString NodeType;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SAddAnimGraphNodeResult
    {
        GENERATED_BODY()

        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;
    };

    REFLECT()
    struct MCPEDITOR_API SAnimGraphNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        PROPERTY()
        int64 Node = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SConnectAnimGraphParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        /** Node the value comes from; on a state canvas, the state a transition leaves. */
        PROPERTY()
        int64 FromNode = 0;

        /** Output pin name on FromNode, as animgraph.describe reports it. */
        PROPERTY()
        FString FromPin;

        PROPERTY()
        int64 ToNode = 0;

        /** Input pin name on ToNode. */
        PROPERTY()
        FString ToPin;
    };

    REFLECT()
    struct MCPEDITOR_API SDisconnectAnimGraphParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        PROPERTY()
        TVector<int64> GraphPath;

        PROPERTY()
        int64 Node = 0;

        /** Pin to clear, in either direction. */
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

        /** Field path on the node, such as StateName, Animation or PinDefaults. */
        PROPERTY()
        FString Path;

        /** The new value as JSON, so a bare number, a quoted name, or a braced object. */
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SSetAnimGraphTransitionPropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString AnimGraph;

        /** Path to the state machine canvas holding the transition. */
        PROPERTY()
        TVector<int64> GraphPath;

        PROPERTY()
        int64 FromNode = 0;

        PROPERTY()
        int64 ToNode = 0;

        /** Field path on the transition, such as Conditions, BlendDuration or Priority. */
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
    struct MCPEDITOR_API SAnimGraphEditResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bSucceeded = false;

        /** Each as "[Name] Description (node Id)". Node ids are local to the canvas that raised them. */
        PROPERTY()
        TVector<FString> Errors;

        PROPERTY()
        TVector<FString> Warnings;
    };

    namespace MCP
    {
        void RegisterAnimGraphTools(FStringView Owner);
    }
}
