#pragma once

#include "MCPLog.h"
#include "UI/Tools/EditorTool.h"

namespace Lumina::MCP
{
    class FServer;
}

namespace Lumina
{
    // Every MCP connection, request and tool call in one place, read from the server's log.
    class MCPEDITOR_API FMCPHubEditorTool : public FEditorTool
    {
    public:

        friend struct FMCPHubEditorToolTestAccess;

        LUMINA_SINGLETON_EDITOR_TOOL(FMCPHubEditorTool)

        FMCPHubEditorTool(IEditorToolContext* Context, MCP::FServer* InServer)
            : FEditorTool(Context, "MCP Hub", nullptr)
            , Server(InServer)
        {}

        bool IsSingleWindowTool() const override { return true; }
        const char* GetTitlebarIcon() const override { return LE_ICON_ROBOT; }

        void OnInitialize() override;
        void OnDeinitialize(const FUpdateContext& UpdateContext) override {}
        void DrawToolMenu(const FUpdateContext& UpdateContext) override;

    private:

        void DrawHubWindow();
        void DrawConnections();
        void DrawTransactions(float Height);
        void DrawDetails();

        void Select(TSharedPtr<const MCP::FTransaction> Transaction);

        NODISCARD bool PassesFilter(const MCP::FTransaction& Transaction) const;
        NODISCARD FStringView FindClient(const FString& Session) const;

        MCP::FServer* Server = nullptr;

        uint64 SeenRevision = ~0ull;
        uint64 SeenRecorded = 0;
        TVector<TSharedPtr<const MCP::FTransaction>> Transactions;
        TVector<MCP::FConnection> Connections;
        TVector<FString> Methods;

        TSharedPtr<const MCP::FTransaction> Selected;
        FString SelectedRequest;
        FString SelectedResponse;

        ImGuiTextFilter TextFilter;
        FString MethodFilter;
        bool bErrorsOnly = false;
        bool bScrollToNewest = false;
    };
}
