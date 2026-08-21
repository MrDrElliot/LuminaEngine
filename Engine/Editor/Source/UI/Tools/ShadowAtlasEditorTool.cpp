#include "ShadowAtlasEditorTool.h"

#include "World/World.h"
#include "World/WorldManager.h"
#include "World/WorldContext.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    void FShadowAtlasEditorTool::OnInitialize()
    {
        CreateToolWindow("Shadow Atlas", [&] (bool bIsFocused)
        {
            DrawAtlasWindow(bIsFocused);
        });
    }

    void FShadowAtlasEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
    }

    void FShadowAtlasEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Atlas",
            "All shadow-casting lights share one big depth atlas. Each light is allocated a tile from a "
            "quad-tree allocator sized by the light's screen-space importance.");
        DrawHelpTextRow("Color Coding",
            "Bigger tiles (more important / closer lights) shade toward red; small tiles toward blue. "
            "The eye is drawn to the expensive allocations, usually what you care about.");
        DrawHelpTextRow("Stats",
            "Utilization % is the fraction of atlas pixels in use. Bucket histogram shows tile-size "
            "distribution; lots of tiny tiles can mean far-away lights deserve culling.");
        DrawHelpTextRow("Tile Table",
            "Per-tile entry: owning light, size, position. Click to highlight on the canvas. Lets you "
            "diagnose 'why is this light blurry' (= got a small tile).");
        DrawHelpTextRow("Configuration",
            "Atlas size + per-light cap come from Project Settings > Renderer > Shadows. "
            "Increase atlas size if utilization regularly hits 100%.");
    }

    // A warmer hue for bigger tiles, so expensive allocations stand out under atlas pressure.
    static ImU32 ColorForSize(uint32 SizePixels, uint32 MinSize, uint32 MaxSize)
    {
        const float LogMin = std::log2((float)MinSize);
        const float LogMax = std::log2((float)MaxSize);
        const float LogCur = std::log2((float)SizePixels);
        const float T = LogMax > LogMin ? Math::Clamp((LogCur - LogMin) / (LogMax - LogMin), 0.0f, 1.0f) : 0.0f;

        // Simple blue->green->red lerp through HSV-like hues.
        const float R = T;
        const float G = 1.0f - std::abs(T - 0.5f) * 2.0f;
        const float B = 1.0f - T;
        return IM_COL32((int)(R * 255), (int)(G * 255), (int)(B * 255), 160);
    }

    void FShadowAtlasEditorTool::DrawAtlasWindow(bool bIsFocused)
    {
        CWorld* TargetWorld = nullptr;
        if (GWorldManager != nullptr)
        {
            for (const TUniquePtr<FWorldContext>& Context : GWorldManager->GetContexts())
            {
                if (Context && Context->Type == EWorldType::Editor && Context->World.IsValid())
                {
                    TargetWorld = Context->World.Get();
                    break;
                }
            }
        }

        if (TargetWorld == nullptr || TargetWorld->GetRenderer() == nullptr)
        {
            ImGui::TextDisabled("No active world / render scene.");
            return;
        }

        const FShadowAtlas* Atlas = TargetWorld->GetRenderer()->GetShadowAtlas();
        if (Atlas == nullptr)
        {
            ImGui::TextDisabled("Active render scene has no shadow atlas.");
            return;
        }

        DrawStats(*Atlas);

        ImGui::Separator();
        ImGui::Checkbox("Show Grid", &bShowGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Show Labels", &bShowLabels);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Canvas Size", &CanvasSize, 256.0f, 1024.0f, "%.0f px");

        ImGui::Separator();
        DrawAtlasCanvas(*Atlas);

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Allocated Tiles", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawTileTable(*Atlas);
        }
    }

    void FShadowAtlasEditorTool::DrawStats(const FShadowAtlas& Atlas)
    {
        const FShadowAtlasConfig& Cfg = Atlas.GetConfig();
        const TVector<FShadowTile>& Tiles = Atlas.GetAllocatedTiles();

        const uint64 AtlasPixels = (uint64)Cfg.AtlasResolution * (uint64)Cfg.AtlasResolution;
        uint64 UsedPixels = 0;
        for (const FShadowTile& Tile : Tiles)
        {
            const uint32 Size = (uint32)(Tile.UVScale.x * (float)Cfg.AtlasResolution + 0.5f);
            UsedPixels += (uint64)Size * (uint64)Size;
        }

        const float UsedPct = AtlasPixels > 0 ? (float)((double)UsedPixels / (double)AtlasPixels * 100.0) : 0.0f;

        ImGui::Text("Atlas: %u x %u", Cfg.AtlasResolution, Cfg.AtlasResolution);
        ImGui::Text("Tiles allocated: %u   Utilization: %.1f%% (%llu / %llu px)",
            (uint32)Tiles.size(), UsedPct, (unsigned long long)UsedPixels, (unsigned long long)AtlasPixels);

        // Counts only power-of-two buckets the allocator can produce, keeping the UI compact.
        ImGui::Spacing();
        if (ImGui::BeginTable("##SizeHistogram", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Tile Size");
            ImGui::TableSetupColumn("Count");
            ImGui::TableSetupColumn("Atlas Area");
            ImGui::TableHeadersRow();

            for (uint32 Size = Cfg.MaxTileResolution; Size >= Cfg.MinTileResolution; Size >>= 1)
            {
                uint32 Count = 0;
                for (const FShadowTile& Tile : Tiles)
                {
                    const uint32 TileSize = (uint32)(Tile.UVScale.x * (float)Cfg.AtlasResolution + 0.5f);
                    if (TileSize == Size)
                    {
                        ++Count;
                    }
                }

                const uint64 BucketPixels = (uint64)Count * (uint64)Size * (uint64)Size;
                const float BucketPct = AtlasPixels > 0 ? (float)((double)BucketPixels / (double)AtlasPixels * 100.0) : 0.0f;

                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                const ImU32 Color = ColorForSize(Size, Cfg.MinTileResolution, Cfg.MaxTileResolution);
                ImVec4 ColorV = ImGui::ColorConvertU32ToFloat4(Color);
                ColorV.w = 1.0f;
                ImGui::ColorButton("##c", ColorV, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel, ImVec2(14, 14));
                ImGui::SameLine();
                ImGui::Text("%u x %u", Size, Size);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", Count);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f%%", BucketPct);
            }

            ImGui::EndTable();
        }
    }

    void FShadowAtlasEditorTool::DrawAtlasCanvas(const FShadowAtlas& Atlas)
    {
        const FShadowAtlasConfig& Cfg = Atlas.GetConfig();
        const TVector<FShadowTile>& Tiles = Atlas.GetAllocatedTiles();

        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const ImVec2 Size(CanvasSize, CanvasSize);
        ImDrawList* DL = ImGui::GetWindowDrawList();

        // Background + border.
        DL->AddRectFilled(Origin, ImVec2(Origin.x + Size.x, Origin.y + Size.y), IM_COL32(20, 20, 22, 255));
        DL->AddRect(Origin, ImVec2(Origin.x + Size.x, Origin.y + Size.y), IM_COL32(120, 120, 120, 255));

        // Matches the top-level free-list grid, which makes fragmentation obvious.
        if (bShowGrid)
        {
            const uint32 GridDivs = Cfg.AtlasResolution / Cfg.MaxTileResolution;
            for (uint32 i = 1; i < GridDivs; ++i)
            {
                const float F = (float)i / (float)GridDivs;
                DL->AddLine(ImVec2(Origin.x + F * Size.x, Origin.y),
                            ImVec2(Origin.x + F * Size.x, Origin.y + Size.y),
                            IM_COL32(60, 60, 60, 255));
                DL->AddLine(ImVec2(Origin.x, Origin.y + F * Size.y),
                            ImVec2(Origin.x + Size.x, Origin.y + F * Size.y),
                            IM_COL32(60, 60, 60, 255));
            }
        }

        int32 HoveredTile = -1;
        const ImVec2 Mouse = ImGui::GetMousePos();

        for (int32 i = 0; i < (int32)Tiles.size(); ++i)
        {
            const FShadowTile& Tile = Tiles[i];
            const uint32 TileSize = (uint32)(Tile.UVScale.x * (float)Cfg.AtlasResolution + 0.5f);

            const ImVec2 TileMin(Origin.x + Tile.UVOffset.x * Size.x,
                                 Origin.y + Tile.UVOffset.y * Size.y);
            const ImVec2 TileMax(TileMin.x + Tile.UVScale.x * Size.x,
                                 TileMin.y + Tile.UVScale.y * Size.y);

            const ImU32 FillColor = ColorForSize(TileSize, Cfg.MinTileResolution, Cfg.MaxTileResolution);
            const ImU32 BorderColor = (FillColor & 0x00FFFFFFu) | 0xFF000000u;

            DL->AddRectFilled(TileMin, TileMax, FillColor);
            DL->AddRect(TileMin, TileMax, BorderColor, 0.0f, 0, 1.0f);

            if (bShowLabels && (TileMax.x - TileMin.x) > 28.0f)
            {
                char Label[16];
                snprintf(Label, sizeof(Label), "%u", TileSize);
                DL->AddText(ImVec2(TileMin.x + 2.0f, TileMin.y + 1.0f), IM_COL32(240, 240, 240, 230), Label);
            }

            if (Mouse.x >= TileMin.x && Mouse.x <= TileMax.x &&
                Mouse.y >= TileMin.y && Mouse.y <= TileMax.y)
            {
                HoveredTile = i;
            }
        }

        // Reserve the canvas area so subsequent widgets flow below it.
        ImGui::Dummy(Size);

        if (HoveredTile >= 0 && ImGui::IsWindowHovered())
        {
            const FShadowTile& Tile = Tiles[HoveredTile];
            const uint32 TileSize = (uint32)(Tile.UVScale.x * (float)Cfg.AtlasResolution + 0.5f);
            const uint32 PxX = (uint32)(Tile.UVOffset.x * (float)Cfg.AtlasResolution + 0.5f);
            const uint32 PxY = (uint32)(Tile.UVOffset.y * (float)Cfg.AtlasResolution + 0.5f);

            ImGui::BeginTooltip();
            ImGui::Text("Tile #%d", HoveredTile);
            ImGui::Text("Size:  %u x %u", TileSize, TileSize);
            ImGui::Text("Pixel: (%u, %u)", PxX, PxY);
            ImGui::Text("UV:    (%.3f, %.3f)  +  (%.3f, %.3f)",
                Tile.UVOffset.x, Tile.UVOffset.y, Tile.UVScale.x, Tile.UVScale.y);
            ImGui::EndTooltip();
        }
    }

    void FShadowAtlasEditorTool::DrawTileTable(const FShadowAtlas& Atlas)
    {
        const FShadowAtlasConfig& Cfg = Atlas.GetConfig();
        const TVector<FShadowTile>& Tiles = Atlas.GetAllocatedTiles();

        if (Tiles.empty())
        {
            ImGui::TextDisabled("No tiles allocated.");
            return;
        }

        if (!ImGui::BeginTable("##ShadowTiles", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                ImVec2(0.0f, 220.0f)))
        {
            return;
        }

        ImGui::TableSetupColumn("Index",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Pixel",  ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("UV Offset", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("UV Scale",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int32 i = 0; i < (int32)Tiles.size(); ++i)
        {
            const FShadowTile& Tile = Tiles[i];
            const uint32 TileSize = (uint32)(Tile.UVScale.x * (float)Cfg.AtlasResolution + 0.5f);
            const uint32 PxX = (uint32)(Tile.UVOffset.x * (float)Cfg.AtlasResolution + 0.5f);
            const uint32 PxY = (uint32)(Tile.UVOffset.y * (float)Cfg.AtlasResolution + 0.5f);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u x %u", TileSize, TileSize);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("(%u, %u)", PxX, PxY);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("(%.3f, %.3f)", Tile.UVOffset.x, Tile.UVOffset.y);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("(%.3f, %.3f)", Tile.UVScale.x, Tile.UVScale.y);
        }

        ImGui::EndTable();
    }
}
