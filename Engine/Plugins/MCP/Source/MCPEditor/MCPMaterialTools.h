#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPMaterialTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SMaterialNodeTypeInfo
    {
        GENERATED_BODY()

        /** Type name to pass to material.add_node. */
        PROPERTY()
        FString Name;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        FString Category;

        PROPERTY()
        FString Description;

        /** JSON Schema of the settable fields on this node, for material.set_node_property. */
        PROPERTY()
        FString ParameterSchema;
    };

    REFLECT()
    struct MCPEDITOR_API SListMaterialNodeTypesParams
    {
        GENERATED_BODY()

        /** Only types whose name or category contains this. Empty lists every one of them. */
        PROPERTY()
        FString Contains;

        /** Include each type's parameter schema, which is large. Off by default. */
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
    struct MCPEDITOR_API SMaterialPinInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        /** Either Input or Output. */
        PROPERTY()
        FString Direction;

        /** Node id this pin is wired to, or 0 when nothing is connected. */
        PROPERTY()
        int64 ConnectedNode = 0;

        PROPERTY()
        FString ConnectedPin;
    };

    REFLECT()
    struct MCPEDITOR_API SMaterialNodeInfo
    {
        GENERATED_BODY()

        /** Id to pass to any tool taking a node. */
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        FString Type;

        PROPERTY()
        FString DisplayName;

        PROPERTY()
        TVector<SMaterialPinInfo> Pins;

        /** Current values of this node's settable fields, as JSON. */
        PROPERTY()
        FString Values;
    };

    REFLECT()
    struct MCPEDITOR_API SDescribeMaterialParams
    {
        GENERATED_BODY()

        /** GUID of the material, from assets.search. */
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
        TVector<SMaterialNodeInfo> Nodes;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateMaterialParams
    {
        GENERATED_BODY()

        /** Folder to create it in, such as /Game/Content/Materials. */
        PROPERTY()
        FString Folder;

        PROPERTY()
        FString Name;
    };

    REFLECT()
    struct MCPEDITOR_API SCreateMaterialResult
    {
        GENERATED_BODY()

        /** GUID to pass to every other material tool. */
        PROPERTY()
        FString Guid;

        PROPERTY()
        FString Path;

        /** Id of the output node the graph is seeded with. */
        PROPERTY()
        int64 OutputNode = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SAddMaterialNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;

        /** Node type name, from material.list_node_types. */
        PROPERTY()
        FString NodeType;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SAddMaterialNodeResult
    {
        GENERATED_BODY()

        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        TVector<SMaterialPinInfo> Pins;
    };

    REFLECT()
    struct MCPEDITOR_API SRemoveMaterialNodeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;

        PROPERTY()
        int64 Node = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SConnectMaterialParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;

        /** Node the value comes from. */
        PROPERTY()
        int64 FromNode = 0;

        /** Output pin name on FromNode, as material.describe reports it. */
        PROPERTY()
        FString FromPin;

        PROPERTY()
        int64 ToNode = 0;

        /** Input pin name on ToNode. Any existing link on it is replaced. */
        PROPERTY()
        FString ToPin;
    };

    REFLECT()
    struct MCPEDITOR_API SDisconnectMaterialParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;

        PROPERTY()
        int64 Node = 0;

        /** Pin to clear, in either direction. */
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

        /** Field path on the node, such as Value or Value.X or ParameterName. */
        PROPERTY()
        FString Path;

        /** The new value as JSON, so a bare number, a quoted name, or a braced object. */
        PROPERTY()
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
    struct MCPEDITOR_API SCompileMaterialParams
    {
        GENERATED_BODY()

        PROPERTY()
        FString Material;
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
