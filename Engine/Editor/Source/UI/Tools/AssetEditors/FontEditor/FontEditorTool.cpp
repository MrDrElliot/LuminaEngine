#include "FontEditorTool.h"
#include "UI/InfoTable.h"

#include "Assets/AssetTypes/Font/Font.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    const char* FontPreviewName = "FontPreview";
    const char* FontDetailsName  = "FontDetails";

    void FFontEditorTool::RebuildPreviewFont()
    {
        if (PreviewFont != nullptr)
        {
            ImGui::GetIO().Fonts->RemoveFont(PreviewFont);
            PreviewFont = nullptr;
        }

        CFont* Font = Cast<CFont>(Asset.Get());
        if (Font == nullptr || !Font->IsValid())
        {
            PreviewFontBytes.Reset();
            return;
        }

        PreviewFontBytes = Font->GetFontData();
        ImFontConfig Config;
        Config.FontDataOwnedByAtlas = false;
        PreviewFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(PreviewFontBytes.GetData(), PreviewFontBytes.Num(), 0.0f, &Config);
    }

    void FFontEditorTool::OnAssetDataChangedExternally()
    {
        FAssetEditorTool::OnAssetDataChangedExternally();
        bPreviewFontStale = true;
    }

    void FFontEditorTool::OnInitialize()
    {
        RebuildPreviewFont();

        CreateToolWindow(FontPreviewName, [this](bool /*bFocused*/)
        {
            CFont* FontAsset = Cast<CFont>(Asset.Get());
            if (!FontAsset)
            {
                return;
            }

            if (bPreviewFontStale)
            {
                bPreviewFontStale = false;
                RebuildPreviewFont();
            }

            ImGui::SetNextItemWidth(240.0f);
            ImGui::SliderFloat("Size", &PreviewSize, 8.0f, 256.0f, "%.0f px");
            ImGui::Spacing();
            ImGui::TextUnformatted("Sample Text");
            ImGui::InputTextMultiline("##SampleText", SampleText, IM_ARRAYSIZE(SampleText), ImVec2(-1, 70.0f));

            ImGui::Separator();
            ImGui::Spacing();

            if (PreviewFont == nullptr)
            {
                ImGui::TextDisabled("Font face could not be rasterized.");
                return;
            }

            if (ImGui::BeginChild("##Canvas", ImVec2(0, 0), ImGuiChildFlags_Borders))
            {
                ImGui::PushFont(PreviewFont, PreviewSize);
                ImGui::TextWrapped("%s", SampleText);
                ImGui::PopFont();
            }
            ImGui::EndChild();
        });

        CreateToolWindow(FontDetailsName, [this](bool /*bFocused*/)
        {
            CFont* FontAsset = Cast<CFont>(Asset.Get());
            if (!FontAsset)
            {
                return;
            }

            ImGuiX::Font::PushFont(ImGuiX::Font::EFont::Large);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", *FontAsset->GetName());
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            ImGui::SeparatorText("Font Information");
            ImGui::Spacing();

            if (ImGui::BeginTable("##FontInfo", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                InfoTable::Row("Family", FontAsset->FamilyName.IsEmpty() ? "Unknown" : FontAsset->FamilyName);
                InfoTable::Row("Style", FontAsset->StyleName.IsEmpty() ? "Unknown" : FontAsset->StyleName);
                InfoTable::Row("Glyphs", Format("{}", FontAsset->NumGlyphs));
                InfoTable::Row("Scalable", FontAsset->bIsScalable ? "Yes" : "No");
                InfoTable::Row("Kerning", FontAsset->bHasKerning ? "Yes" : "No");

                InfoTable::Row("File Size", ImGuiX::FormatSize(FontAsset->GetFontData().Num()));

                if (!FontAsset->SourcePath.IsEmpty())
                {
                    InfoTable::Row("Source", FontAsset->SourcePath);
                }

                ImGui::EndTable();
            }
        });
    }

    void FFontEditorTool::OnDeinitialize(const FUpdateContext& UpdateContext)
    {
        if (PreviewFont != nullptr)
        {
            ImGui::GetIO().Fonts->RemoveFont(PreviewFont);
            PreviewFont = nullptr;
        }
    }

    void FFontEditorTool::DrawHelpMenu()
    {
        DrawHelpTextRow("Preview",
            "The Size slider rasterizes the embedded face at any pixel size. Edit the Sample "
            "Text box to preview arbitrary strings.");
        DrawHelpTextRow("Details",
            "Family, style and glyph count are read from the face at import time. The raw font "
            "bytes are embedded in the asset, so cooked builds need no source file.");
    }

    void FFontEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2& InDockspaceSize) const
    {
        SplitDockingLayoutLeftRight(InDockspaceID, FontPreviewName, FontDetailsName, 0.3f);
    }
}
