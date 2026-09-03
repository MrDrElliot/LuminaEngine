#include "SpriteSheetEditorTool.h"

#include "Assets/AssetTypes/Textures/Texture.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Properties/PropertyTable.h"
#include "imgui.h"

namespace Lumina
{
    static const char* SheetWindowName      = "Sheet";
    static const char* AnimationsWindowName = "Animations";
    static const char* PreviewWindowName    = "Preview";
    static const char* DetailsWindowName    = "Details";

    namespace
    {
        constexpr ImU32 kGridColor     = IM_COL32(255, 255, 255, 60);
        constexpr ImU32 kInClipColor   = IM_COL32(80, 170, 255, 90);
        constexpr ImU32 kCurrentColor  = IM_COL32(255, 200, 60, 255);
        constexpr ImU32 kOrderTextCol  = IM_COL32(255, 255, 255, 220);
    }

    FSpriteSheetEditorTool::FSpriteSheetEditorTool(IEditorToolContext* Context, CObject* InAsset)
        : FAssetEditorTool(Context, InAsset->GetName().c_str(), InAsset)
    {
    }

    void FSpriteSheetEditorTool::OnInitialize()
    {
        FAssetEditorTool::OnInitialize();

        CreateToolWindow(SheetWindowName,      [this](bool) { DrawSheetWindow(); });
        CreateToolWindow(AnimationsWindowName, [this](bool) { DrawAnimationsWindow(); });
        CreateToolWindow(PreviewWindowName,    [this](bool) { DrawPreviewWindow(); });
        CreateToolWindow(DetailsWindowName,    [this](bool) { PropertyTable.DrawTree(); });
    }

    void FSpriteSheetEditorTool::MarkEdited()
    {
        if (Asset.IsValid() && Asset->GetPackage() != nullptr)
        {
            Asset->GetPackage()->MarkDirty();
        }
        PropertyTable.MarkDirty();
    }

    SSpriteAnimation* FSpriteSheetEditorTool::GetSelectedAnimation()
    {
        CSpriteSheet* Sheet = GetAsset<CSpriteSheet>();
        if (Sheet == nullptr || SelectedAnimation < 0 || SelectedAnimation >= (int32)Sheet->Animations.size())
        {
            return nullptr;
        }
        return &Sheet->Animations[SelectedAnimation];
    }

    ImVec4 FSpriteSheetEditorTool::CellUVs(int32 Cell)
    {
        CSpriteSheet* Sheet = GetAsset<CSpriteSheet>();
        if (Sheet == nullptr)
        {
            return ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
        }

        const int32 HF = Math::Max(Sheet->HFrames, 1);
        const int32 VF = Math::Max(Sheet->VFrames, 1);
        const int32 Clamped = Math::Clamp(Cell, 0, HF * VF - 1);
        const int32 Cx = Clamped % HF;
        const int32 Cy = Clamped / HF;

        return ImVec4((float)Cx / (float)HF, (float)Cy / (float)VF,
                      (float)(Cx + 1) / (float)HF, (float)(Cy + 1) / (float)VF);
    }

    void FSpriteSheetEditorTool::DrawSheetWindow()
    {
        CSpriteSheet* Sheet = GetAsset<CSpriteSheet>();
        if (Sheet == nullptr)
        {
            return;
        }

        CTexture* Texture = Sheet->Texture.Get();
        if (Texture == nullptr || Texture->GetResourceID() < 0)
        {
            ImGui::TextDisabled("Assign a Texture in the Details panel to slice it.");
            return;
        }

        const int32 HF = Math::Max(Sheet->HFrames, 1);
        const int32 VF = Math::Max(Sheet->VFrames, 1);

        ImGui::Text("%d x %d grid, %d frames", HF, VF, HF * VF);
        ImGui::SameLine();
        ImGui::TextDisabled("(click a cell to append it to the selected animation)");
        ImGui::Separator();

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        if (Avail.x <= 0.0f || Avail.y <= 0.0f)
        {
            return;
        }

        // Fit the sheet to the pane, keeping the grid square so cell picking stays honest.
        float SheetW = (float)Texture->GetTextureResource().Mips[0].Width;
        float SheetH = (float)Texture->GetTextureResource().Mips[0].Height;
        if (SheetW <= 0.0f || SheetH <= 0.0f)
        {
            return;
        }

        const float Scale = Math::Min(Avail.x / SheetW, Avail.y / SheetH);
        const ImVec2 Size(SheetW * Scale, SheetH * Scale);
        const ImVec2 Origin = ImGui::GetCursorScreenPos();

        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        DrawList->AddImage(ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()),
                           Origin, ImVec2(Origin.x + Size.x, Origin.y + Size.y));

        const float CellW = Size.x / (float)HF;
        const float CellH = Size.y / (float)VF;

        const SSpriteAnimation* Clip = GetSelectedAnimation();

        // Cells already in the clip get a wash plus their play order, so a sheet reads as a storyboard.
        if (Clip != nullptr)
        {
            for (int32 Slot = 0; Slot < (int32)Clip->Frames.size(); ++Slot)
            {
                const int32 Cell = Math::Clamp(Clip->Frames[Slot], 0, HF * VF - 1);
                const ImVec2 Min(Origin.x + (Cell % HF) * CellW, Origin.y + (Cell / HF) * CellH);
                const ImVec2 Max(Min.x + CellW, Min.y + CellH);

                DrawList->AddRectFilled(Min, Max, kInClipColor);

                char Order[8];
                std::snprintf(Order, sizeof(Order), "%d", Slot);
                DrawList->AddText(ImVec2(Min.x + 3.0f, Min.y + 2.0f), kOrderTextCol, Order);
            }
        }

        for (int32 Column = 0; Column <= HF; ++Column)
        {
            const float X = Origin.x + Column * CellW;
            DrawList->AddLine(ImVec2(X, Origin.y), ImVec2(X, Origin.y + Size.y), kGridColor);
        }
        for (int32 Row = 0; Row <= VF; ++Row)
        {
            const float Y = Origin.y + Row * CellH;
            DrawList->AddLine(ImVec2(Origin.x, Y), ImVec2(Origin.x + Size.x, Y), kGridColor);
        }

        // The frame the preview is on, so playback and the sheet always agree.
        if (Clip != nullptr && !Clip->Frames.empty())
        {
            const int32 Slot = Math::Clamp(Preview.Frame, 0, (int32)Clip->Frames.size() - 1);
            const int32 Cell = Math::Clamp(Clip->Frames[Slot], 0, HF * VF - 1);
            const ImVec2 Min(Origin.x + (Cell % HF) * CellW, Origin.y + (Cell / HF) * CellH);
            DrawList->AddRect(Min, ImVec2(Min.x + CellW, Min.y + CellH), kCurrentColor, 0.0f, 0, 2.0f);
        }

        ImGui::InvisibleButton("##SheetCanvas", Size);
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const ImVec2 Mouse = ImGui::GetMousePos();
            const int32 Column = Math::Clamp((int32)((Mouse.x - Origin.x) / CellW), 0, HF - 1);
            const int32 Row    = Math::Clamp((int32)((Mouse.y - Origin.y) / CellH), 0, VF - 1);

            if (SSpriteAnimation* Target = GetSelectedAnimation())
            {
                Target->Frames.push_back(Row * HF + Column);
                SelectedFrameSlot = (int32)Target->Frames.size() - 1;
                MarkEdited();
            }
        }
    }

    void FSpriteSheetEditorTool::DrawAnimationsWindow()
    {
        CSpriteSheet* Sheet = GetAsset<CSpriteSheet>();
        if (Sheet == nullptr)
        {
            return;
        }

        if (ImGui::Button(LE_ICON_PLUS " Add"))
        {
            SSpriteAnimation& Added = Sheet->Animations.emplace_back();
            Added.Name = FName("NewAnimation");
            SelectedAnimation = (int32)Sheet->Animations.size() - 1;
            SelectedFrameSlot = -1;
            MarkEdited();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(GetSelectedAnimation() == nullptr);
        if (ImGui::Button(LE_ICON_MINUS " Remove"))
        {
            Sheet->Animations.erase(Sheet->Animations.begin() + SelectedAnimation);
            SelectedAnimation = Math::Max(SelectedAnimation - 1, 0);
            SelectedFrameSlot = -1;
            MarkEdited();
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        const float ListHeight = ImGui::GetContentRegionAvail().y * 0.35f;
        if (ImGui::BeginChild("##Clips", ImVec2(0.0f, ListHeight), ImGuiChildFlags_Borders))
        {
            for (int32 Index = 0; Index < (int32)Sheet->Animations.size(); ++Index)
            {
                const SSpriteAnimation& Animation = Sheet->Animations[Index];

                char Label[160];
                std::snprintf(Label, sizeof(Label), "%s  (%d frames)##Clip%d",
                              Animation.Name.IsNone() ? "(unnamed)" : Animation.Name.c_str(),
                              (int32)Animation.Frames.size(), Index);

                if (ImGui::Selectable(Label, Index == SelectedAnimation))
                {
                    SelectedAnimation = Index;
                    SelectedFrameSlot = -1;
                    Preview = FSpritePlayback{};
                }
            }
        }
        ImGui::EndChild();

        SSpriteAnimation* Clip = GetSelectedAnimation();
        if (Clip == nullptr)
        {
            ImGui::TextDisabled("Add an animation to start building it.");
            return;
        }

        ImGui::Separator();

        // Committed on enter or focus loss, since FName interns every string it is handed.
        char NameBuffer[128];
        std::snprintf(NameBuffer, sizeof(NameBuffer), "%s", Clip->Name.IsNone() ? "" : Clip->Name.c_str());
        const bool bNameSubmitted = ImGui::InputText("Name", NameBuffer, sizeof(NameBuffer),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        if (bNameSubmitted || ImGui::IsItemDeactivatedAfterEdit())
        {
            Clip->Name = FName(NameBuffer);
            MarkEdited();
        }

        if (ImGui::DragFloat("FPS", &Clip->FPS, 0.25f, 0.0f, 240.0f, "%.2f"))
        {
            MarkEdited();
        }

        if (ImGui::Checkbox("Loop", &Clip->bLoop))
        {
            MarkEdited();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Frames");

        ImGui::BeginDisabled(SelectedFrameSlot < 0);
        if (ImGui::Button(LE_ICON_ARROW_LEFT "##MoveLeft") && SelectedFrameSlot > 0)
        {
            const int32 Temp = Clip->Frames[SelectedFrameSlot - 1];
            Clip->Frames[SelectedFrameSlot - 1] = Clip->Frames[SelectedFrameSlot];
            Clip->Frames[SelectedFrameSlot] = Temp;
            --SelectedFrameSlot;
            MarkEdited();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_ARROW_RIGHT "##MoveRight") && SelectedFrameSlot + 1 < (int32)Clip->Frames.size())
        {
            const int32 Temp = Clip->Frames[SelectedFrameSlot + 1];
            Clip->Frames[SelectedFrameSlot + 1] = Clip->Frames[SelectedFrameSlot];
            Clip->Frames[SelectedFrameSlot] = Temp;
            ++SelectedFrameSlot;
            MarkEdited();
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_DELETE " Remove Frame"))
        {
            Clip->Frames.erase(Clip->Frames.begin() + SelectedFrameSlot);
            SelectedFrameSlot = Math::Min(SelectedFrameSlot, (int32)Clip->Frames.size() - 1);
            MarkEdited();
        }
        ImGui::EndDisabled();

        CTexture* Texture = Sheet->Texture.Get();
        const bool bHasTexture = Texture != nullptr && Texture->GetResourceID() >= 0;

        if (ImGui::BeginChild("##FrameStrip", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            constexpr float kThumb = 56.0f;

            for (int32 Slot = 0; Slot < (int32)Clip->Frames.size(); ++Slot)
            {
                ImGui::PushID(Slot);

                const ImVec4 UV = CellUVs(Clip->Frames[Slot]);
                const bool bSelected = Slot == SelectedFrameSlot;

                if (bHasTexture)
                {
                    if (ImGui::ImageButton("##Frame",
                                           ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()),
                                           ImVec2(kThumb, kThumb),
                                           ImVec2(UV.x, UV.y), ImVec2(UV.z, UV.w),
                                           ImVec4(0, 0, 0, 0),
                                           bSelected ? ImVec4(1.0f, 0.78f, 0.24f, 1.0f) : ImVec4(1, 1, 1, 1)))
                    {
                        SelectedFrameSlot = Slot;
                    }
                }
                else if (ImGui::Button("##Frame", ImVec2(kThumb, kThumb)))
                {
                    SelectedFrameSlot = Slot;
                }

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Slot %d, cell %d", Slot, Clip->Frames[Slot]);
                }

                ImGui::PopID();

                if (Slot + 1 < (int32)Clip->Frames.size())
                {
                    ImGui::SameLine();
                }
            }
        }
        ImGui::EndChild();
    }

    void FSpriteSheetEditorTool::DrawPreviewWindow()
    {
        CSpriteSheet* Sheet = GetAsset<CSpriteSheet>();
        const SSpriteAnimation* Clip = GetSelectedAnimation();

        if (Sheet == nullptr || Clip == nullptr || Clip->Frames.empty())
        {
            ImGui::TextDisabled("Select an animation with at least one frame.");
            return;
        }

        if (ImGui::Button(bPreviewPlaying ? LE_ICON_PAUSE " Pause" : LE_ICON_PLAY " Play"))
        {
            bPreviewPlaying = !bPreviewPlaying;
        }
        ImGui::SameLine();
        if (ImGui::Button(LE_ICON_SKIP_PREVIOUS " Restart"))
        {
            Preview = FSpritePlayback{};
        }
        ImGui::SameLine();
        ImGui::Text("Frame %d / %d", Preview.Frame + 1, (int32)Clip->Frames.size());

        // The same stepper the runtime system uses, so the preview cannot drift from playback.
        if (bPreviewPlaying)
        {
            SpriteAnimation::Advance(Preview, (int32)Clip->Frames.size(), Clip->FPS, Clip->bLoop,
                                     1.0f, ImGui::GetIO().DeltaTime);
        }

        CTexture* Texture = Sheet->Texture.Get();
        if (Texture == nullptr || Texture->GetResourceID() < 0)
        {
            return;
        }

        const int32 Slot = Math::Clamp(Preview.Frame, 0, (int32)Clip->Frames.size() - 1);
        const ImVec4 UV  = CellUVs(Clip->Frames[Slot]);

        const ImVec2 Avail = ImGui::GetContentRegionAvail();
        const float  Side  = Math::Max(Math::Min(Avail.x, Avail.y), 16.0f);

        ImGui::Image(ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()),
                     ImVec2(Side, Side), ImVec2(UV.x, UV.y), ImVec2(UV.z, UV.w));
    }

    void FSpriteSheetEditorTool::InitializeDockingLayout(ImGuiID InDockspaceID, const ImVec2&) const
    {
        ImGui::DockBuilderRemoveNodeChildNodes(InDockspaceID);

        ImGuiID LeftDockID = 0, RightDockID = 0, BottomLeftID = 0, TopRightID = 0;
        ImGui::DockBuilderSplitNode(InDockspaceID, ImGuiDir_Right, 0.30f, &RightDockID, &LeftDockID);
        ImGui::DockBuilderSplitNode(LeftDockID, ImGuiDir_Down, 0.40f, &BottomLeftID, &LeftDockID);
        ImGui::DockBuilderSplitNode(RightDockID, ImGuiDir_Up, 0.40f, &TopRightID, &RightDockID);

        ImGui::DockBuilderDockWindow(GetToolWindowName(SheetWindowName).c_str(), LeftDockID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(AnimationsWindowName).c_str(), BottomLeftID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(PreviewWindowName).c_str(), TopRightID);
        ImGui::DockBuilderDockWindow(GetToolWindowName(DetailsWindowName).c_str(), RightDockID);
    }
}
