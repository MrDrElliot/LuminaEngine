#pragma once

#include "Containers/String.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPBuiltinTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SEngineStatusParams
    {
        GENERATED_BODY()
    };

    REFLECT()
    struct MCPEDITOR_API SEngineStatusResult
    {
        GENERATED_BODY()

        /** Name of the loaded project, or empty when the editor has none open. */
        PROPERTY()
        FString ProjectName;

        /** Directory the loaded project lives in. */
        PROPERTY()
        FString ProjectPath;

        /** Whether a project is open, which most other tools need before they can do anything. */
        PROPERTY()
        bool bHasProject = false;
    };

    namespace MCP
    {
        // Registers the tools this plugin owns. Anything else may add its own to the same registry.
        void RegisterBuiltinTools(FStringView Owner);
    }
}
