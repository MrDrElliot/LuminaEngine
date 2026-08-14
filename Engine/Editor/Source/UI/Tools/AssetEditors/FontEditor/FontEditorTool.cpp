#include "FontEditorTool.h"

#include "Assets/AssetTypes/Font/Font.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiFonts.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    const char* FontPreviewName = "FontPreview";
    const char* FontDetailsName  = "FontDetails";

    void FFontEditorTool::OnInitialize()
    {
        CFont* Font = Cast<CFont>(Asset.Get());
        if (Font && Font->IsValid())
        {
            // Dynamic atlas (ImGui 1.92): the face can be added live and rasterized
            // at any pushed size. Bytes are owned by the asset, not the atlas.
            ImFontConfig Config;
            Config.FontDataOwnedByAtlas = false;
            PreviewFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                const_cast<uint8*>(Font->GetFontData().data()),
                (int)Font->GetFontData().size(),
                0.0f,
                &Config);
        }

        CreateToolWindow(FontPreviewName, [this](bool /*bFocused*/)
        {
            CFont* FontAsset = Cast<CFont>(Asset.Get());
            if (!FontAsset)
            {
                return;
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
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", FontAsset->GetName().c_str());
            ImGuiX::Font::PopFont();

            ImGui::Spacing();
            ImGui::SeparatorText("Font Information");
            ImGui::Spacing();

            if (ImGui::BeginTable("##FontInfo", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto Row = [](const char* Label, const FString& Value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(Label);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(Value.c_str());
                };

                Row("Family", FontAsset->FamilyName.empty() ? "Unknown" : FontAsset->FamilyName);
                Row("Style", FontAsset->StyleName.empty() ? "Unknown" : FontAsset->StyleName);
                Row("Glyphs", eastl::to_string(FontAsset->NumGlyphs));
                Row("Scalable", FontAsset->bIsScalable ? "Yes" : "No");
                Row("Kerning", FontAsset->bHasKerning ? "Yes" : "No");

                const size_t Bytes = FontAsset->GetFontData().size();
                FString SizeStr = Bytes >= 1024 * 1024
                    ? eastl::to_string(Bytes / (1024 * 1024)) + " MB"
                    : eastl::to_string(Bytes / 1024) + " KB";
                Row("File Size", SizeStr);

                if (!FontAsset->SourcePath.empty())
                {
                    Row("Source", FontAsset->SourcePath);
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
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.3f, &RightDockID, &LeftDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(FontPreviewName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(FontDetailsName).c_str(), RightDockID);
    }
}
