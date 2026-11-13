#include <gtest/gtest.h>

#include "Core/UpdateContext.h"
#include "ImGuiTestHarness.h"
#include "MCPHubEditorTool.h"
#include "MCPServer.h"

using namespace Lumina;

namespace Lumina
{
    struct FMCPHubEditorToolTestAccess
    {
        static void SetMethodFilter(FMCPHubEditorTool& Tool, const FString& Method) { Tool.MethodFilter = Method; }
        static const FString& GetMethodFilter(const FMCPHubEditorTool& Tool) { return Tool.MethodFilter; }
        static bool PassesFilter(const FMCPHubEditorTool& Tool, const MCP::FTransaction& Transaction) { return Tool.PassesFilter(Transaction); }
    };
}

namespace
{
    // Clicks the "Clear" menu item, which is the last widget DrawToolMenu draws.
    void ClickClear(FMCPHubEditorTool& Tool)
    {
        const FUpdateContext Context;
        ImVec2 ItemCenter(0.0f, 0.0f);

        // End() restores the item rect to what it was before Begin(), so it must be read before End().
        auto Frame = [&]
        {
            FImGuiTestHarness::BeginFrame();
            Tool.DrawToolMenu(Context);
            ItemCenter = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
            FImGuiTestHarness::EndFrame();
        };

        Frame();
        ImGui::GetIO().AddMousePosEvent(ItemCenter.x, ItemCenter.y);
        Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        Frame();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        Frame();
    }
}

TEST(MCPHubEditorTool, ClearResetsTheMethodFilterSoALaterTransactionStaysVisible)
{
    FImGuiTestHarness Harness;

    MCP::FServer Server;
    (void)Server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", "");

    FMCPHubEditorTool Tool(nullptr, &Server);
    FMCPHubEditorToolTestAccess::SetMethodFilter(Tool, "tools/list");

    ClickClear(Tool);
    EXPECT_TRUE(FMCPHubEditorToolTestAccess::GetMethodFilter(Tool).IsEmpty());

    (void)Server.HandleMessage(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"clientInfo":{"name":"tester","version":"1"}}})", "");
    TVector<TSharedPtr<const MCP::FTransaction>> Transactions;
    TVector<MCP::FConnection> Connections;
    Server.GetLog().Snapshot(Transactions, Connections);

    ASSERT_FALSE(Transactions.IsEmpty());
    const MCP::FTransaction& Later = *Transactions[Transactions.Num() - 1];
    ASSERT_EQ(Later.Method, "initialize");
    EXPECT_TRUE(FMCPHubEditorToolTestAccess::PassesFilter(Tool, Later));
}
