#pragma once

#include "Core/Delegates/Delegate.h"
#include "Core/Module/ModuleInterface.h"
#include "MCPEditorSessionTools.h"
#include "MCPLookDevTools.h"
#include "MCPPackageTools.h"
#include "MCPProfilerTools.h"
#include "MCPServer.h"

namespace Lumina
{
    // Stands up a loopback Model Context Protocol endpoint while the plugin is enabled.
    class MCPEDITOR_API FMCPEditorModule : public IModuleInterface
    {
    public:

        void StartupModule() override;
        void ShutdownModule() override;

        // Null until startup has run, and null again after shutdown.
        NODISCARD static MCP::FServer* GetServer();
        NODISCARD static MCP::FPackageToolsState* GetPackageTools();
        NODISCARD static MCP::FProfilerToolsState* GetProfilerTools();
        NODISCARD static MCP::FEditorInputState* GetEditorInput();
        NODISCARD static MCP::FLookDevFixtureState* GetLookDevFixture();

    private:

        MCP::FServer Server;
        MCP::FPackageToolsState PackageTools;
        MCP::FProfilerToolsState ProfilerTools;
        MCP::FEditorInputState EditorInput;
        MCP::FLookDevFixtureState LookDevFixture;
        FDelegateHandle ShutdownStartedHandle;
    };
}
