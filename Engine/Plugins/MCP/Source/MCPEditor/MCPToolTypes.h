#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPToolTypes.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SAssetInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Content path, which is what the editor shows in the content browser.
        PROPERTY()
        FString Path;

        // Asset class, so a field wanting a CStaticMesh can be matched against it.
        PROPERTY()
        FString AssetClass;

        // Pass this to any component field that takes an asset.
        PROPERTY()
        FString Guid;
    };

    REFLECT()
    struct MCPEDITOR_API SGraphPinConnection
    {
        GENERATED_BODY()

        PROPERTY()
        int64 Node = 0;

        PROPERTY()
        FString Pin;
    };

    REFLECT()
    struct MCPEDITOR_API SGraphPinInfo
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Either Input or Output.
        PROPERTY()
        FString Direction;

        // Every pin this one is wired to; an output can feed several inputs.
        PROPERTY()
        TVector<SGraphPinConnection> Connections;
    };

    REFLECT()
    struct MCPEDITOR_API SGraphNodeInfo
    {
        GENERATED_BODY()

        // Id to pass to any tool taking a node.
        PROPERTY()
        int64 Id = 0;

        PROPERTY()
        FString Type;

        PROPERTY()
        FString DisplayName;

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
    struct MCPEDITOR_API SVector3Param
    {
        GENERATED_BODY()

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        PROPERTY()
        float Z = 0.0f;
    };
}
