#include "TextureStreamingEditorTool.h"
#include <bit>

#include "Core/Console/ConsoleVariable.h"
#include "Renderer/RenderResource.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // Fraction of the full chain that is resident, for the per-texture bar.
        float ResidentFraction(const FTextureStreamingManager::FTextureSnapshot& Row)
        {
            return Row.FullBytes > 0 ? (float)((double)Row.ResidentBytes / (double)Row.FullBytes) : 1.0f;
        }

        // A screen of red while the camera is close to those surfaces means feedback is not arriving.
        ImVec4 ResidencyColor(const FTextureStreamingManager::FTextureSnapshot& Row)
        {
            if (Row.ResidentFirstMip == 0)
            {
                return ImVec4(0.45f, 0.85f, 0.5f, 1.0f);
            }
            if (Row.ResidentFirstMip >= Row.TailFirstMip)
            {
                return ImVec4(0.9f, 0.45f, 0.4f, 1.0f);
            }
            return ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
        }

        const char* MipStateLabel(const FTextureStreamingManager::FTextureSnapshot& Row)
        {
            if (Row.bLoadInFlight)                            { return "loading"; }
            if (Row.PinCount > 0)                             { return "pinned"; }
            if (Row.BudgetedFirstMip < Row.ResidentFirstMip)  { return "streaming in"; }
            if (Row.BudgetedFirstMip > Row.ResidentFirstMip)  { return "trimming"; }

            // The budget had to give up mips quality asked for, so this texture pays for a pool that is too small.
            if (Row.BudgetedFirstMip > Row.WantedFirstMip)    { return "budget-capped"; }
            return "settled";
        }
    }

    void FTextureStreamingEditorTool::OnInitialize()
    {
        CreateToolWindow("Texture Streaming", [&](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FTextureStreamingEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FTextureStreamingEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("What this shows",
            "Every texture registered with the streamer. Textures load holding only their inline tail "
            "(mips at or below 256px) and are promoted from there on demand -- by an editor tab pinning "
            "them, or by the renderer reporting how much of the screen they cover.");
        DrawHelpTextRow("Resident / Full",
            "Resident is what the GPU image currently costs; Full is what the whole mip chain would cost. "
            "The gap between the two totals is what streaming is saving you.");
        DrawHelpTextRow("First mip",
            "0 means fully resident. Higher means the top mips are on disk -- 'Tail' is the floor, the "
            "most streamed-out this texture can get. Green = full, amber = partial, red = at the tail.");
        DrawHelpTextRow("Coverage",
            "Last reported on-screen size in pixels. A texture that is visibly large but shows 0 coverage "
            "means the renderer feedback is not reaching the streamer -- that is a bug, not a policy.");
        DrawHelpTextRow("Is it working?",
            "Promotions and demotions should both climb as you fly the camera around. Zero promotions "
            "with textures sitting at their tail means nothing is demanding them. Bytes read should track "
            "promotions; failed loads should stay at zero.");
        DrawHelpTextRow("Settings",
            "File > Settings > Rendering > Texture Streaming. Pool Size MB sets the budget, Resolution "
            "Bias trades sharpness for memory, Max Loads In Flight bounds the IO, and unticking Enabled "
            "promotes everything and stops trimming (the pre-streaming behavior) if you need to rule the "
            "streamer out of a bug. Edits apply on the next frame and persist to the project's "
            "/Config/GameSettings.json.");
    }

    void FTextureStreamingEditorTool::DrawWindow(bool bIsFocused)
    {
        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr)
        {
            ImGui::TextDisabled("No texture streaming manager (headless, or the renderer is not up).");
            return;
        }

        const FTextureStreamingManager::FStats Stats = Streaming->GetStats();
        Streaming->GetSnapshot(Snapshot, Pending);

        ResidentHistory[HistoryCursor] = (float)((double)Stats.ResidentBytes / (1024.0 * 1024.0));
        HistoryCursor = (HistoryCursor + 1) % kHistory;

        DrawSummary(Stats);

        ImGui::Separator();

        ImGui::SetNextItemWidth(240.0f);
        ImGui::InputTextWithHint("##Filter", LE_ICON_MAGNIFY " Filter (name / package)", Filter, sizeof(Filter));
        ImGui::SameLine();
        ImGui::Checkbox("Streamed only", &bStreamedOnly);
        ImGui::SameLine();
        ImGui::Checkbox("Pinned only", &bPinnedOnly);

        if (!Pending.empty())
        {
            DrawPendingTable();
        }

        DrawTextureTable();
    }

    void FTextureStreamingEditorTool::DrawSummary(const FTextureStreamingManager::FStats& Stats)
    {
        const uint64 Saved = Stats.FullyResidentBytes > Stats.ResidentBytes
            ? Stats.FullyResidentBytes - Stats.ResidentBytes
            : 0;

        const float Fraction = Stats.BudgetBytes > 0
            ? (float)((double)Stats.ResidentBytes / (double)Stats.BudgetBytes)
            : 0.0f;

        if (ImGui::BeginTable("##StreamingSummary", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            ImGui::Text("Pool");
            ImGui::SameLine();

            // Over-budget is a real state, since pinned textures are exempt from eviction and push through it.
            const bool bOver = Fraction > 1.0f;
            if (bOver)
            {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.35f, 0.3f, 1.0f));
            }
            ImGui::ProgressBar(Fraction > 1.0f ? 1.0f : Fraction, ImVec2(260.0f, 0.0f),
                Format("{} / {}",
                    ImGuiX::FormatSize(Stats.ResidentBytes),
                    ImGuiX::FormatSize(Stats.BudgetBytes)).c_str());
            if (bOver)
            {
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.45f, 1.0f), "over budget");
            }

            ImGui::Text("Streamable textures: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "%u", Stats.NumTextures);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("pinned: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%u", Stats.NumPinned);

            ImGui::Text("Fully resident would cost: ");
            ImGui::SameLine();
            ImGui::TextUnformatted(ImGuiX::FormatSize(Stats.FullyResidentBytes).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("->");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s saved", ImGuiX::FormatSize(Saved).c_str());

            ImGui::TableSetColumnIndex(1);

            ImGui::PlotLines("##ResidentHistory", ResidentHistory, kHistory, HistoryCursor,
                "Resident MiB", 0.0f, FLT_MAX, ImVec2(-1.0f, 60.0f));

            ImGui::Text("In flight: ");
            ImGui::SameLine();
            ImGui::TextColored(Stats.NumLoadsInFlight > 0 ? ImVec4(0.5f, 0.9f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "%u", Stats.NumLoadsInFlight);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("last frame: +%u / -%u", Stats.NumPromotedLastFrame, Stats.NumDemotedLastFrame);

            ImGui::Text("Total: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%llu promoted", (unsigned long long)Stats.TotalPromotions);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "%llu demoted", (unsigned long long)Stats.TotalDemotions);
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("%s read", ImGuiX::FormatSize(Stats.TotalBytesRead).c_str());

            // Loud, since a failed load means a bulk ref did not resolve rather than a policy outcome.
            if (Stats.TotalFailedLoads > 0)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.35f, 1.0f),
                    LE_ICON_ALERT " %llu failed load(s) -- see the log", (unsigned long long)Stats.TotalFailedLoads);
            }

            ImGui::EndTable();
        }

        if (Stats.NumTextures == 0)
        {
            ImGui::TextDisabled("Nothing registered. Assets saved before the PACKAGE_BULK_DATA version store "
                                "every mip inline and are not streamable -- re-save or re-cook them.");
        }
        else if (Stats.TotalPromotions == 0 && Stats.FrameCounter > 600)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f),
                LE_ICON_ALERT " No texture has been promoted yet. If textures look blurry, the renderer's "
                "coverage feedback is not reaching the streamer.");
        }
    }

    void FTextureStreamingEditorTool::DrawPendingTable()
    {
        if (!ImGui::CollapsingHeader("In-flight requests", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        if (ImGui::BeginTable("##Pending", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("From mip", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("To mip", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Staged", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();

            for (const FTextureStreamingManager::FPendingSnapshot& Row : Pending)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(Row.Name.IsNone() ? "<destroyed>" : Row.Name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", Row.SourceFirstMip);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", Row.TargetFirstMip);

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(ImGuiX::FormatSize(Row.Bytes).c_str());

                ImGui::TableSetColumnIndex(4);
                if (Row.bComplete)
                {
                    // Complete but still listed means residency is applied on the game thread at the next Update.
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "read, applying");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "reading");
                }
            }

            ImGui::EndTable();
        }
    }

    void FTextureStreamingEditorTool::DrawTextureTable()
    {
        const FStringView FilterView(Filter);

        if (!ImGui::BeginTable("##StreamingTextures", 10,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SizingFixedFit))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Residency", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Resident", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Full", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("Coverage", ImGuiTableColumnFlags_WidthFixed, 85.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        // The question this answers most often is what is eating the pool, which is the top of the list.
        TVector<const FTextureStreamingManager::FTextureSnapshot*> Rows;
        Rows.reserve(Snapshot.size());

        for (const FTextureStreamingManager::FTextureSnapshot& Row : Snapshot)
        {
            if (bPinnedOnly && Row.PinCount == 0)
            {
                continue;
            }
            if (bStreamedOnly && Row.ResidentFirstMip == 0)
            {
                continue;
            }
            if (!FilterView.empty())
            {
                const FString Name = Row.Name.ToString();
                if (Name.find(Filter) == FString::npos && Row.PackagePath.find(Filter) == FString::npos)
                {
                    continue;
                }
            }
            Rows.push_back(&Row);
        }

        Algo::Sort(Rows.begin(), Rows.end(),
            [](const FTextureStreamingManager::FTextureSnapshot* A, const FTextureStreamingManager::FTextureSnapshot* B)
            {
                return A->ResidentBytes > B->ResidentBytes;
            });

        for (const FTextureStreamingManager::FTextureSnapshot* Row : Rows)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(Row->Name.c_str());
            if (ImGui::IsItemHovered() && !Row->PackagePath.empty())
            {
                ImGui::SetTooltip("%s\nbindless slot %d", Row->PackagePath.c_str(), Row->ResourceID);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u x %u", Row->Width, Row->Height);

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(RHI::Format::Info(Row->Format).Name);

            ImGui::TableSetColumnIndex(3);
            // Reads as the image holding mips 4 through 12 of a 13-mip chain.
            ImGui::TextColored(ResidencyColor(*Row), "%u..%u of %u",
                Row->ResidentFirstMip, Row->NumMips > 0 ? Row->NumMips - 1u : 0u, Row->NumMips);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "resident from mip %u\n"
                    "wanted   mip %u   (quality target, ignores the pool)\n"
                    "budgeted mip %u   (what the pool allows)\n"
                    "tail     mip %u   (most streamed out it can get)\n"
                    "%s",
                    Row->ResidentFirstMip, Row->WantedFirstMip, Row->BudgetedFirstMip, Row->TailFirstMip,
                    Row->BudgetedFirstMip > Row->WantedFirstMip
                        ? "budget is holding this below its quality target"
                        : "pool is not constraining this texture");
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::ProgressBar(ResidentFraction(*Row), ImVec2(-1.0f, 0.0f), "");

            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(ImGuiX::FormatSize(Row->ResidentBytes).c_str());

            ImGui::TableSetColumnIndex(6);
            ImGui::TextDisabled("%s", ImGuiX::FormatSize(Row->FullBytes).c_str());

            ImGui::TableSetColumnIndex(7);
            // CPU bytes far above resident means the streamer freed the image but kept its mip pixels.
            const bool bCpuHeavy = Row->CpuBytes > Row->ResidentBytes + (Row->ResidentBytes / 2);
            if (bCpuHeavy)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "%s", ImGuiX::FormatSize(Row->CpuBytes).c_str());
            }
            else
            {
                ImGui::TextDisabled("%s", ImGuiX::FormatSize(Row->CpuBytes).c_str());
            }

            ImGui::TableSetColumnIndex(8);
            // GPU feedback wins over the CPU estimate wherever it exists, so show what actually decided.
            if (Row->bFeedbackValid)
            {
                if (Row->FeedbackMask == 0u)
                {
                    ImGui::TextDisabled("idle");
                }
                else
                {
                    const uint32 Finest = (uint32)Math::CountTrailingZeros64(Row->FeedbackMask);
                    if (Finest == 0u)      { ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "want finer"); }
                    else if (Finest == 1u) { ImGui::Text("exact"); }
                    else                   { ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "+%u coarse", Finest - 1u); }
                }
            }
            else
            {
                // No bindless slot, so no shader can name it and no feedback can exist for it.
                ImGui::TextDisabled("-");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("The mip the shaders that sampled this texture asked for, relative to what "
                                  "is resident:\n'want finer' promotes one, '+N coarse' gives one back, "
                                  "'idle' means nothing sampled it.\n\n%llu frame(s) since anything demanded it",
                    (unsigned long long)Row->FramesSinceDemand);
            }

            ImGui::TableSetColumnIndex(9);
            if (Row->PinCount > 0)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), LE_ICON_PIN " %u", Row->PinCount);
            }
            else
            {
                ImGui::TextUnformatted(MipStateLabel(*Row));
            }
        }

        ImGui::EndTable();
    }
}
