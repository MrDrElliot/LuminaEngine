#include "MCPEditorModule.h"

#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Module/ModuleManager.h"
#include "Log/Log.h"

using namespace Lumina;

IMPLEMENT_MODULE(FMCPEditorModule, "MCPEditor");

namespace
{
    FMCPEditorModule* GModule = nullptr;

    TConsoleVar<int32> GMCPPort(
        "mcp.Port",
        8787,
        "Loopback port the Model Context Protocol endpoint listens on. Zero picks a free one.");

    // Enabling this opt-in plugin is the opt-in, and the listener never leaves loopback.
    TConsoleVar<bool> GMCPAutoStart(
        "mcp.AutoStart",
        true,
        "Whether the Model Context Protocol endpoint opens as soon as the plugin loads.");
}

namespace Lumina
{
    MCP::FServer* FMCPEditorModule::GetServer()
    {
        return GModule != nullptr ? &GModule->Server : nullptr;
    }

    void FMCPEditorModule::StartupModule()
    {
        GModule = this;

        // A single dash clusters into character flags, so the switch has to be the double dash form.
        const bool bSwitched = GCommandLine != nullptr && GCommandLine->Has("mcp");

        if (!bSwitched && !GMCPAutoStart.GetValue())
        {
            LOG_INFO("[MCP] No endpoint is open because mcp.AutoStart is off.");
            return;
        }

        int32 Port = GMCPPort.GetValue();
        if (GCommandLine != nullptr)
        {
            if (const TOptional<int> Override = GCommandLine->GetInt("mcpport"))
            {
                Port = *Override;
            }
        }

        if (Port < 0 || Port > 0xFFFF)
        {
            LOG_ERROR("[MCP] mcp.Port is {}, which is not a port number.", Port);
            return;
        }

        MCP::FServerSettings Settings;
        Settings.Port = static_cast<uint16>(Port);

        Server.Start(Settings);
    }

    void FMCPEditorModule::ShutdownModule()
    {
        // The transport owns a thread that calls back into this DLL, so it has to stop before unload.
        Server.Stop();

        GModule = nullptr;
    }
}
