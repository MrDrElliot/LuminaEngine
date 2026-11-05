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

        // Empty when the editor has no project open.
        PROPERTY()
        FString ProjectName;

        PROPERTY()
        FString ProjectPath;

        // Most other tools need a project open.
        PROPERTY()
        bool bHasProject = false;
    };

    namespace MCP
    {
        void RegisterBuiltinTools(FStringView Owner);
    }
}
