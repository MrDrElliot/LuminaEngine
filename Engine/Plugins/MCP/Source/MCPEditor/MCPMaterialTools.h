#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "MCPToolTypes.h"

#include "MCPMaterialTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SMaterialNodeTypeInfo
    {
        GENERATED_BODY()

        // Pass this to material.node as NodeType.
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;

        PROPERTY()
        FString Description;

        // JSON Schema of the fields material.set_node_property can set.
        PROPERTY()
        FString ParameterSchema;
    };

    REFLECT()
    struct MCPEDITOR_API SListMaterialNodeTypesParams
    {
        GENERATED_BODY()

        // Substring of the type name or category. Empty lists every type.
        PROPERTY()
        FString Contains;

        // Include each type's ParameterSchema, which is large.
        PROPERTY()
        bool bIncludeSchema = false;
    };

    REFLECT()
    struct MCPEDITOR_API SListMaterialNodeTypesResult
    {
        GENERATED_BODY()

        PROPERTY()
        TVector<SMaterialNodeTypeInfo> Types;
    };

    REFLECT()
    struct MCPEDITOR_API SMaterialRefParams
    {
        GENERATED_BODY()

        // GUID or content path of the material or material function.
        PROPERTY()
        FString Material;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeMaterialResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        PROPERTY()
        TVector<SGraphNodeInfo> Nodes;

        // False for a material function, or while shaders are compiling or after they failed.
        PROPERTY()
        bool bReadyForRender = false;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateMaterialParams
    {
        GENERATED_BODY()

        // Folder to create it in, such as /Game/Content/Materials.
        PROPERTY()
        FString Folder;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateMaterialResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Guid;

        PROPERTY()
        FString Path;

        // Id of the output node the graph is seeded with.
        PROPERTY()
        int64 OutputNode = 0;
    };

    REFLECT()
    enum class EMaterialNodeAction : uint8
    {
        Add,
        Remove,
    };

    REFLECT()
    struct MCPEDITOR_API SMaterialNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        EMaterialNodeAction Action = EMaterialNodeAction::Add;

        PROPERTY()
        FString Material;

        // Add only. Node type name, from material.list_node_types.
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
    struct MCPEDITOR_API SMaterialNodeResult
    {
        GENERATED_BODY()

        // Add only. Id of the new node.
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        TVector<SGraphPinInfo> Pins;
    };

    REFLECT()
    enum class EMaterialLinkAction : uint8
    {
        Connect,
        Disconnect,
    };

    REFLECT()
    struct MCPEDITOR_API SMaterialLinkParams
    {
        GENERATED_BODY()

        PROPERTY()
        EMaterialLinkAction Action = EMaterialLinkAction::Connect;

        PROPERTY()
        FString Material;

        // Connect only. Node the value comes from.
        PROPERTY()
        int64 FromNode = 0;

        // Connect only. Output pin name on FromNode, as material.describe reports it.
        PROPERTY()
        FString FromPin;

        // Connect only.
        PROPERTY()
        int64 ToNode = 0;

        // Connect only. Input pin on ToNode; any existing link on it is replaced.
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
    struct MCPEDITOR_API SSetMaterialNodePropertyParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;

        PROPERTY()
        int64 Node = 0;

        // Field path on the node, such as Value, Value.X or ParameterName.
        PROPERTY()
        FString Path;

        // New value as JSON: a number, a quoted string or an object.
        PROPERTY(RawJson)
        FString Value;
    };

    REFLECT()
    struct MCPEDITOR_API SMaterialPropertyResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString Previous;

        PROPERTY()
        FString Current;
    };

    REFLECT()
    struct MCPEDITOR_API SCompileMaterialResult
    {
        GENERATED_BODY()

        PROPERTY()
        bool bSucceeded = false;

        PROPERTY()
        TVector<FString> Errors;

        PROPERTY()
        TVector<FString> Warnings;
    };

    namespace MCP
    {
        void RegisterMaterialTools(FStringView Owner);
    }
}
