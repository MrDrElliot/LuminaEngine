#include "TextureHeapEditorTool.h"
#include "Renderer/RHICore.h"
#include "Renderer/RenderResource.h"
#include "Renderer/TextureStreamingManager.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    static const char* HeapTextureTypeName(RHI::ETextureType Type)
    {
        switch (Type)
        {
        case RHI::ETextureType::Tex1D:        return "1D";
        case RHI::ETextureType::Tex2D:        return "2D";
        case RHI::ETextureType::Tex3D:        return "3D";
        case RHI::ETextureType::TexCube:      return "Cube";
        case RHI::ETextureType::Tex2DArray:   return "2D Array";
        case RHI::ETextureType::TexCubeArray: return "Cube Array";
        }
        return "?";
    }

    static FString UsageString(RHI::EImageUsageFlags Usage)
    {
        FString Out;
        auto Append = [&](RHI::EImageUsageFlags Flag, const char* Name)
        {
            if (EnumHasAnyFlags(Usage, Flag))
            {
                if (!Out.empty()) { Out += " | "; }
                Out += Name;
            }
        };
        Append(RHI::EImageUsageFlags::Sampled,         "Sampled");
        Append(RHI::EImageUsageFlags::Storage,         "Storage");
        Append(RHI::EImageUsageFlags::ColorAttachment, "Color");
        Append(RHI::EImageUsageFlags::DepthAttachment, "Depth");
        Append(RHI::EImageUsageFlags::TransferSrc,     "TSrc");
        Append(RHI::EImageUsageFlags::TransferDst,     "TDst");
        return Out;
    }

    // Whole-mip-chain estimate; block-compressed formats count blocks, not texels.
    static uint64 EstimateTextureBytes(const RHI::FTextureDesc& Desc)
    {
        const FFormatInfo& Info = RHI::Format::Info(Desc.Format);
        const uint64 Block = Math::Max<uint64>(Info.BlockSize, 1);

        uint64 Total = 0;
        uint64 W = Math::Max<uint32>(Desc.Dimension.x, 1);
        uint64 H = Math::Max<uint32>(Desc.Dimension.y, 1);
        uint64 D = Math::Max<uint32>(Desc.Dimension.z, 1);
        for (uint32 Mip = 0; Mip < Math::Max<uint32>(Desc.MipCount, 1); ++Mip)
        {
            const uint64 BlocksX = (W + Block - 1) / Block;
            const uint64 BlocksY = (H + Block - 1) / Block;
            Total += BlocksX * BlocksY * D * Info.BytesPerBlock;
            W = Math::Max<uint64>(W >> 1, 1);
            H = Math::Max<uint64>(H >> 1, 1);
            D = Math::Max<uint64>(D >> 1, 1);
        }

        const uint64 Layers = Math::Max<uint32>(Desc.LayerCount, 1) * (Desc.Type == RHI::ETextureType::TexCube || Desc.Type == RHI::ETextureType::TexCubeArray ? 6ull : 1ull);
        return Total * Layers * Math::Max<uint32>(Desc.SampleCount, 1);
    }

    // ImGui samples the heap as Texture2D, so only single-sample 2D slots can be previewed.
    static bool CanPreview(const RHI::FTextureDesc& Desc)
    {
        return Desc.Type == RHI::ETextureType::Tex2D && Desc.SampleCount <= 1;
    }

    void FTextureHeapEditorTool::OnInitialize()
    {
        CreateToolWindow("Texture Heap", [&](bool bIsFocused)
        {
            DrawWindow(bIsFocused);
        });
    }

    void FTextureHeapEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FTextureHeapEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Heap",
            "Every texture currently registered in the global bindless heap, by sampled slot index. "
            "This is exactly what shaders see through gTextures2D[] / SampleTexture2D.");
        DrawHelpTextRow("Preview",
            "Click a thumbnail (or row) to open it enlarged in the inspector panel. Cube, 3D, array "
            "and MSAA textures can't be drawn through ImGui's 2D sampler and show no thumbnail.");
        DrawHelpTextRow("Filter",
            "Matches against format name, type and slot number.");
    }

    void FTextureHeapEditorTool::DrawStreamingStats()
    {
        FTextureStreamingManager* Streaming = FTextureStreamingManager::TryGet();
        if (Streaming == nullptr)
        {
            return;
        }

        const FTextureStreamingManager::FStats Stats = Streaming->GetStats();
        if (Stats.NumTextures == 0)
        {
            ImGui::TextDisabled("No streamable textures registered (assets saved before PACKAGE_BULK_DATA "
                                "store every mip inline; re-save or re-cook them to enable streaming).");
            return;
        }

        const float Fraction = Stats.BudgetBytes > 0
            ? (float)((double)Stats.ResidentBytes / (double)Stats.BudgetBytes)
            : 0.0f;

        // The interesting number is the gap between what these cost now and fully resident.
        const uint64 Saved = Stats.FullyResidentBytes > Stats.ResidentBytes
            ? Stats.FullyResidentBytes - Stats.ResidentBytes
            : 0;

        ImGui::Separator();
        ImGui::Text("Streaming pool");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::ProgressBar(Fraction, ImVec2(220.0f, 0.0f),
            Format("{} / {}",
                ImGuiX::FormatSize(Stats.ResidentBytes),
                ImGuiX::FormatSize(Stats.BudgetBytes)).c_str());

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%u streamable", Stats.NumTextures);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%u pinned", Stats.NumPinned);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s saved", ImGuiX::FormatSize(Saved).c_str());

        if (Stats.NumLoadsInFlight > 0 || Stats.NumPromotedLastFrame > 0 || Stats.NumDemotedLastFrame > 0)
        {
            ImGui::TextDisabled("%u load(s) in flight, +%u promoted / -%u demoted last frame",
                Stats.NumLoadsInFlight, Stats.NumPromotedLastFrame, Stats.NumDemotedLastFrame);
        }
    }

    void FTextureHeapEditorTool::DrawWindow(bool bIsFocused)
    {
        TVector<RHI::FHeapTextureInfo> Textures;
        RHI::GetTextureHeapTextures(RHI::Core::GetGlobalHeap(), Textures);

        uint64 TotalBytes = 0;
        for (const RHI::FHeapTextureInfo& Info : Textures)
        {
            TotalBytes += EstimateTextureBytes(Info.Desc);
        }

        ImGui::Text("%u sampled slots in use", (uint32)Textures.size());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("~%s estimated", ImGuiX::FormatSize(TotalBytes).c_str());

        DrawStreamingStats();

        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##Filter", LE_ICON_MAGNIFY " Filter (format / type / slot)", Filter, sizeof(Filter));
        ImGui::Separator();

        const bool bInspector = SelectedSlot != RHI::kInvalidHeapSlot;
        const float InspectorWidth = 360.0f;
        const float TableWidth = bInspector ? ImGui::GetContentRegionAvail().x - InspectorWidth - ImGui::GetStyle().ItemSpacing.x : 0.0f;

        if (ImGui::BeginChild("##HeapTable", ImVec2(TableWidth, 0), ImGuiChildFlags_None))
        {
            DrawTextureTable(Textures);
        }
        ImGui::EndChild();

        if (bInspector)
        {
            ImGui::SameLine();
            if (ImGui::BeginChild("##HeapInspector", ImVec2(InspectorWidth, 0), ImGuiChildFlags_Borders))
            {
                DrawInspector(Textures);
            }
            ImGui::EndChild();
        }
    }

    void FTextureHeapEditorTool::DrawTextureTable(const TVector<RHI::FHeapTextureInfo>& Textures)
    {
        constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##Heap", 7, TableFlags))
        {
            return;
        }

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Slot",    ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Mips",    ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Memory",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        const FStringView FilterView(Filter);

        for (const RHI::FHeapTextureInfo& Info : Textures)
        {
            const RHI::FTextureDesc& Desc = Info.Desc;
            const char* FormatName = RHI::Format::Info(Desc.Format).Name;

            if (!FilterView.empty())
            {
                char SlotText[16];
                snprintf(SlotText, sizeof(SlotText), "%u", Info.Slot);
                const bool bMatch = FString(FormatName).find(Filter) != FString::npos
                                 || FString(HeapTextureTypeName(Desc.Type)).find(Filter) != FString::npos
                                 || FStringView(SlotText) == FilterView;
                if (!bMatch)
                {
                    continue;
                }
            }

            ImGui::PushID((int)Info.Slot);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const bool bSelected = Info.Slot == SelectedSlot;
            char SlotLabel[16];
            snprintf(SlotLabel, sizeof(SlotLabel), "%u", Info.Slot);
            // Sized to the thumbnail so the hover highlight covers the full row.
            if (ImGui::Selectable(SlotLabel, bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, 48.0f)))
            {
                SelectedSlot = bSelected ? RHI::kInvalidHeapSlot : Info.Slot;
            }

            ImGui::TableNextColumn();
            if (CanPreview(Desc))
            {
                ImGui::Image(ImGuiX::ToImTextureRef(Info.Slot), ImVec2(48.0f, 48.0f));
                if (ImGui::IsItemClicked())
                {
                    SelectedSlot = bSelected ? RHI::kInvalidHeapSlot : Info.Slot;
                }
            }
            else
            {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            if (Desc.Dimension.z > 1)
            {
                ImGui::Text("%ux%ux%u", Desc.Dimension.x, Desc.Dimension.y, Desc.Dimension.z);
            }
            else
            {
                ImGui::Text("%ux%u", Desc.Dimension.x, Desc.Dimension.y);
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatName);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(HeapTextureTypeName(Desc.Type));

            ImGui::TableNextColumn();
            ImGui::Text("%u", Desc.MipCount);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ImGuiX::FormatSize(EstimateTextureBytes(Desc)).c_str());

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    void FTextureHeapEditorTool::DrawInspector(const TVector<RHI::FHeapTextureInfo>& Textures)
    {
        const RHI::FHeapTextureInfo* Selected = nullptr;
        for (const RHI::FHeapTextureInfo& Info : Textures)
        {
            if (Info.Slot == SelectedSlot)
            {
                Selected = &Info;
                break;
            }
        }

        // Slot was freed (or its texture destroyed) since selection.
        if (Selected == nullptr)
        {
            SelectedSlot = RHI::kInvalidHeapSlot;
            return;
        }

        const RHI::FTextureDesc& Desc = Selected->Desc;

        ImGui::Text(LE_ICON_IMAGE " Slot %u", Selected->Slot);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        if (ImGui::SmallButton(LE_ICON_CLOSE))
        {
            SelectedSlot = RHI::kInvalidHeapSlot;
            return;
        }
        ImGui::Separator();

        if (CanPreview(Desc))
        {
            // Fit into a fixed box regardless of texture resolution.
            constexpr float PreviewBox = 320.0f;
            const float W = (float)Math::Max<uint32>(Desc.Dimension.x, 1);
            const float H = (float)Math::Max<uint32>(Desc.Dimension.y, 1);
            const float Scale = Math::Min(PreviewBox / W, PreviewBox / H);
            const ImVec2 PreviewSize(W * Scale, H * Scale);

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Math::Max(0.0f, (ImGui::GetContentRegionAvail().x - PreviewSize.x) * 0.5f));
            ImGui::Image(ImGuiX::ToImTextureRef(Selected->Slot), PreviewSize);
        }
        else
        {
            ImGui::TextDisabled("No 2D preview for %s textures.", HeapTextureTypeName(Desc.Type));
        }

        ImGui::Spacing();

        if (ImGui::BeginTable("##Details", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            auto Row = [](const char* Label, const char* Fmt, auto... Args)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Label);
                ImGui::TableNextColumn();
                ImGui::Text(Fmt, Args...);
            };

            Row("Dimensions", "%u x %u x %u", Desc.Dimension.x, Desc.Dimension.y, Desc.Dimension.z);
            Row("Format",     "%s", RHI::Format::Info(Desc.Format).Name);
            Row("Type",       "%s", HeapTextureTypeName(Desc.Type));
            Row("Mips",       "%u", Desc.MipCount);
            Row("Layers",     "%u", Desc.LayerCount);
            Row("Samples",    "%u", Desc.SampleCount);
            Row("Usage",      "%s", UsageString(Desc.Usage).c_str());
            Row("Memory",     "~%s", ImGuiX::FormatSize(EstimateTextureBytes(Desc)).c_str());

            ImGui::EndTable();
        }
    }
}
