#include "MCPHubEditorTool.h"

#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Core/Json/Json.h"
#include "MCPServer.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace MCPHubEditorToolInternal
    {
        // The detail view shows this much of a payload, and Copy still takes all of it.
        constexpr int32 kDisplayBytes = 16 * 1024;

        FString FormatTime(const PlatformTime::FDateTime& Time)
        {
            return Format("{:02}:{:02}:{:02}.{:03}", Time.Hour, Time.Minute, Time.Second, Time.Millisecond);
        }

        FString PrettyJson(const FString& Text)
        {
            FJsonValue Parsed;
            return FJsonValue::Parse(FStringView(Text), Parsed) ? Parsed.ToString(2) : Text;
        }

        void DrawPayload(const char* Label, const FString& Text, float Width)
        {
            ImGui::BeginGroup();
            ImGui::TextColored(EditorColors::TextDim(), "%s", Label);
            ImGui::SameLine();
            ImGui::PushID(Label);
            if (ImGui::SmallButton(LE_ICON_CONTENT_COPY " Copy"))
            {
                ImGui::SetClipboardText(*Text);
            }

            const FString Shown = MCP::ClipText(FStringView(Text), kDisplayBytes);
            ImGui::InputTextMultiline("##Payload", const_cast<char*>(*Shown), static_cast<size_t>(Shown.Len()) + 1,
                ImVec2(Width, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopID();
            ImGui::EndGroup();
        }
    }

    using namespace MCPHubEditorToolInternal;

    void FMCPHubEditorTool::OnInitialize()
    {
        CreateToolWindow("MCP Hub", [&](bool)
        {
            DrawHubWindow();
        });
    }

    void FMCPHubEditorTool::DrawToolMenu(const FUpdateContext& UpdateContext)
    {
        if (Server->IsRunning())
        {
            ImGui::TextColored(EditorColors::Success(), LE_ICON_CIRCLE " Listening on 127.0.0.1:%u", Server->GetBoundPort());
        }
        else
        {
            ImGui::TextColored(EditorColors::Danger(), LE_ICON_CIRCLE_OUTLINE " Stopped");
        }

        ImGui::Separator();
        ImGui::Text("%d requests", Transactions.Num());

        if (ImGui::MenuItem(LE_ICON_DELETE " Clear"))
        {
            Server->GetLog().Clear();
        }
    }

    void FMCPHubEditorTool::DrawHubWindow()
    {
        const MCP::FLog& Log = Server->GetLog();
        if (const uint64 Revision = Log.GetRevision(); Revision != SeenRevision)
        {
            SeenRevision = Revision;
            const uint64 Recorded = Log.Snapshot(Transactions, Connections);

            Methods.Reset();
            for (const TSharedPtr<const MCP::FTransaction>& Transaction : Transactions)
            {
                if (!Methods.Contains(Transaction->Method))
                {
                    Methods.Add(Transaction->Method);
                }
            }

            bScrollToNewest = Recorded != SeenRecorded;
            SeenRecorded = Recorded;
        }

        DrawConnections();

        ImGuiX::SearchBar("##MCPFilter", TextFilter, LE_ICON_MAGNIFY " Filter by tool, arguments or error...");

        ImGui::SetNextItemWidth(180.0f * ImGuiX::GetUIScale());
        if (ImGui::BeginCombo("##Method", MethodFilter.IsEmpty() ? "All methods" : *MethodFilter))
        {
            if (ImGui::Selectable("All methods", MethodFilter.IsEmpty()))
            {
                MethodFilter.Reset();
            }
            for (const FString& Method : Methods)
            {
                if (ImGui::Selectable(*Method, Method == MethodFilter))
                {
                    MethodFilter = Method;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Errors only", &bErrorsOnly);

        const float Available = ImGui::GetContentRegionAvail().y;
        DrawTransactions(Selected ? Available * 0.5f : Available);

        if (Selected)
        {
            DrawDetails();
        }
    }

    void FMCPHubEditorTool::DrawConnections()
    {
        const FString Header = Format(LE_ICON_LAN_CONNECT " Connections ({})###Connections", Connections.Num());
        if (!ImGui::CollapsingHeader(*Header))
        {
            return;
        }

        if (!ImGui::BeginTable("##Connections", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        {
            return;
        }

        ImGui::TableSetupColumn("Client");
        ImGui::TableSetupColumn("Session");
        ImGui::TableSetupColumn("First seen");
        ImGui::TableSetupColumn("Last seen");
        ImGui::TableSetupColumn("Requests");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();

        for (const MCP::FConnection& Connection : Connections)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(*Connection.Client);
            ImGui::TableNextColumn(); ImGui::TextColored(EditorColors::TextDim(), "%s", Connection.Session.IsEmpty() ? "-" : *Connection.Session);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(*FormatTime(Connection.FirstSeen));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(*FormatTime(Connection.LastSeen));
            ImGui::TableNextColumn(); ImGui::Text("%u (%u failed)", Connection.Requests, Connection.Errors);
            ImGui::TableNextColumn();
            ImGui::TextColored(Connection.bOpen ? EditorColors::Success() : EditorColors::TextMuted(), "%s", Connection.bOpen ? "Open" : "Closed");
        }

        ImGui::EndTable();
    }

    void FMCPHubEditorTool::DrawTransactions(float Height)
    {
        constexpr ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##Transactions", 7, Flags, ImVec2(0.0f, Height)))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();

        TVector<int32> Visible;
        Visible.Reserve(Transactions.Num());
        for (int32 Index = 0; Index < Transactions.Num(); ++Index)
        {
            if (PassesFilter(*Transactions[Index]))
            {
                Visible.Add(Index);
            }
        }

        const bool bPinnedToNewest = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

        ImGuiListClipper Clipper;
        Clipper.Begin(Visible.Num());
        while (Clipper.Step())
        {
            for (int32 Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
            {
                const TSharedPtr<const MCP::FTransaction>& Transaction = Transactions[Visible[Row]];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(Visible[Row]);
                if (ImGui::Selectable(*FormatTime(Transaction->Time), Selected == Transaction, ImGuiSelectableFlags_SpanAllColumns))
                {
                    Select(Selected == Transaction ? nullptr : Transaction);
                }
                ImGui::PopID();

                ImGui::TableNextColumn(); ImGuiX::TextUnformatted(FindClient(Transaction->Session));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(*Transaction->Method);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(*Transaction->Tool);
                ImGui::TableNextColumn(); ImGui::TextColored(EditorColors::TextDim(), "%s", *Transaction->Arguments);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", Transaction->DurationMs);
                ImGui::TableNextColumn();
                if (Transaction->IsError())
                {
                    ImGui::TextColored(EditorColors::Danger(), LE_ICON_ALERT_CIRCLE " %s", *Transaction->Error);
                }
                else
                {
                    ImGui::TextColored(EditorColors::Success(), LE_ICON_CHECK " ok");
                }
            }
        }

        if (bScrollToNewest && bPinnedToNewest)
        {
            ImGui::SetScrollY(ImGui::GetScrollMaxY() + ImGui::GetTextLineHeightWithSpacing());
        }
        bScrollToNewest = false;

        ImGui::EndTable();
    }

    void FMCPHubEditorTool::DrawDetails()
    {
        const float Width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        DrawPayload("Request", SelectedRequest, Width);
        ImGui::SameLine();
        DrawPayload("Response", SelectedResponse, Width);
    }

    void FMCPHubEditorTool::Select(TSharedPtr<const MCP::FTransaction> Transaction)
    {
        Selected = Move(Transaction);
        SelectedRequest  = Selected ? PrettyJson(Selected->Request) : FString();
        SelectedResponse = Selected ? PrettyJson(Selected->Response) : FString();
    }

    bool FMCPHubEditorTool::PassesFilter(const MCP::FTransaction& Transaction) const
    {
        if ((bErrorsOnly && !Transaction.IsError()) || (!MethodFilter.IsEmpty() && Transaction.Method != MethodFilter))
        {
            return false;
        }

        return !TextFilter.IsActive()
            || ImGuiX::PassSearchFilter(TextFilter, FStringView(Transaction.Tool))
            || ImGuiX::PassSearchFilter(TextFilter, FStringView(Transaction.Arguments))
            || ImGuiX::PassSearchFilter(TextFilter, FStringView(Transaction.Error));
    }

    FStringView FMCPHubEditorTool::FindClient(const FString& Session) const
    {
        const auto Connection = Algo::FindIf(Connections, [&Session](const MCP::FConnection& Each) { return Each.Session == Session; });
        return Connection != Connections.end() ? FStringView(Connection->Client) : FStringView();
    }
}
