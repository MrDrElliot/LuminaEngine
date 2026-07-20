#include "CoreTypeCustomization.h"
#include "BonePickerContext.h"
#include "ParameterPickerContext.h"
#include "SocketPickerContext.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Renderer/MeshData.h"
#include "UI/Tools/AssetEditors/TextureEditor/TextureEditorTool.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>


namespace Lumina
{
    namespace
    {
        // Recursive draw of a single bone node and its children. Returns true if
        // a bone was clicked, with OutSelected set to its name.
        bool DrawBoneTreeNode(const FSkeletonResource& Skeleton, int32 BoneIndex, ImGuiTextFilter& Filter, const FName& Current, FName& OutSelected)
        {
            const FSkeletonResource::FBoneInfo& Bone = Skeleton.GetBone(BoneIndex);
            const char* Label = Bone.Name.c_str();
            const TVector<int32> Children = Skeleton.GetChildBones(BoneIndex);

            // The filter hides non-matching leaves but keeps a node visible if
            // any descendant matches, so users can drill down by partial name.
            const bool bSelfMatches = Filter.PassFilter(Label);
            bool bAnyChildVisible = false;
            for (int32 Child : Children)
            {
                if (Filter.PassFilter(Skeleton.GetBone(Child).Name.c_str()))
                {
                    bAnyChildVisible = true;
                    break;
                }
            }
            if (!bSelfMatches && !bAnyChildVisible && Filter.IsActive())
            {
                bool bDeepMatch = false;
                for (int32 Child : Children)
                {
                    FName Throwaway;
                    if (DrawBoneTreeNode(Skeleton, Child, Filter, Current, Throwaway))
                    {
                        bDeepMatch = true;
                        OutSelected = Throwaway;
                    }
                }
                return bDeepMatch;
            }

            ImGuiTreeNodeFlags Flags =  ImGuiTreeNodeFlags_OpenOnArrow | 
                                        ImGuiTreeNodeFlags_OpenOnDoubleClick | 
                                        ImGuiTreeNodeFlags_SpanAvailWidth | 
                                        ImGuiTreeNodeFlags_DefaultOpen;
            
            if (Children.empty())
            {
                Flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            if (Bone.Name == Current)
            {
                Flags |= ImGuiTreeNodeFlags_Selected;
            }
            if (Filter.IsActive())
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            }

            const bool bOpen = ImGui::TreeNodeEx((const void*)(intptr_t)BoneIndex, Flags, "%s", Label);

            bool bClicked = false;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
            {
                OutSelected = Bone.Name;
                bClicked = true;
            }

            if (bOpen && !(Flags & ImGuiTreeNodeFlags_NoTreePushOnOpen))
            {
                for (int32 Child : Children)
                {
                    FName ChildSelected;
                    if (DrawBoneTreeNode(Skeleton, Child, Filter, Current, ChildSelected))
                    {
                        OutSelected = ChildSelected;
                        bClicked = true;
                    }
                }
                ImGui::TreePop();
            }

            return bClicked;
        }
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        const bool bBonePicker = Property->Property->HasMetadata("BonePicker");
        const bool bParamPicker = Property->Property->HasMetadata("ParameterPicker");
        const bool bSocketPicker = Property->Property->HasMetadata("SocketPicker");
        const FSkeletonResource* Skeleton = bBonePicker ? BonePickerContext::GetActiveSkeleton() : nullptr;
        CAnimationGraph* PickerGraph = bParamPicker ? ParameterPickerContext::GetActiveGraph() : nullptr;
        const SocketPickerContext::FSocketPickerData* SocketData = bSocketPicker ? SocketPickerContext::GetActive() : nullptr;

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        const float ButtonWidth = (bBonePicker || bParamPicker || bSocketPicker) ? ImGui::GetFrameHeight() : 0.0f;

        char Buffer[256];
        strncpy(Buffer, DisplayValue.c_str(), sizeof(Buffer));
        Buffer[sizeof(Buffer) - 1] = '\0';

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
        if (ImGui::InputText("##ParamName", Buffer, sizeof(Buffer)))
        {
            DisplayValue = FName(Buffer);
        }
        ImGui::PopItemWidth();

        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Updated;
        }

        if (bParamPicker)
        {
            ImGui::SameLine(0, 0);
            const bool bHasGraph = PickerGraph != nullptr;
            ImGui::BeginDisabled(!bHasGraph);
            if (ImGui::Button(LE_ICON_MENU_DOWN "##ParamPick", ImVec2(ButtonWidth, 0)))
            {
                ImGui::OpenPopup("##ParameterPicker");
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal(bHasGraph ? "Pick existing parameter" : "No graph context");
            }

            if (ImGui::BeginPopup("##ParameterPicker"))
            {
                if (ImGui::Selectable("(none)", DisplayValue.IsNone()))
                {
                    DisplayValue = FName();
                    Result = EPropertyChangeOp::Updated;
                    ImGui::CloseCurrentPopup();
                }
                CBlackboard* Blackboard = (PickerGraph != nullptr) ? PickerGraph->Blackboard.Get() : nullptr;
                if (Blackboard == nullptr)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("No blackboard assigned.");
                    ImGui::TextDisabled("Set one on the graph asset.");
                }
                else if (Blackboard->Keys.empty())
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("Blackboard has no keys.");
                    ImGui::TextDisabled("Add keys in the Blackboard editor.");
                }
                else
                {
                    ImGui::Separator();
                    for (const FBlackboardKey& Key : Blackboard->Keys)
                    {
                        if (Key.Name.IsNone() || EnumHasAnyFlags(Key.Flags, EBlackboardKeyFlags::Hidden))
                        {
                            continue;
                        }
                        const bool bSelected = (Key.Name == DisplayValue);
                        if (ImGui::Selectable(Key.Name.c_str(), bSelected))
                        {
                            DisplayValue = Key.Name;
                            Result = EPropertyChangeOp::Updated;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }

        if (bBonePicker)
        {
            ImGui::SameLine(0, 0);
            const bool bHasSkeleton = Skeleton != nullptr && Skeleton->GetNumBones() > 0;
            ImGui::BeginDisabled(!bHasSkeleton);
            if (ImGui::Button(LE_ICON_BONE "##BonePick", ImVec2(ButtonWidth, 0)))
            {
                BoneFilter.Clear();
                ImGui::OpenPopup("##BonePicker");
            }
            ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal(bHasSkeleton ? "Pick bone from skeleton" : "No skeleton assigned on the asset");
            }

            if (ImGui::BeginPopup("##BonePicker"))
            {
                BoneFilter.Draw("##Filter", 320.0f);

                if (ImGui::BeginChild("##BoneTree", ImVec2(340, 400), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14.0f);
                    if (Skeleton != nullptr)
                    {
                        // None entry first: lets the user clear the selection.
                        if (ImGui::Selectable("(none)", DisplayValue.IsNone()))
                        {
                            DisplayValue = FName();
                            Result = EPropertyChangeOp::Updated;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::Separator();

                        const TVector<int32> Roots = Skeleton->GetRootBones();
                        for (int32 Root : Roots)
                        {
                            FName Selected;
                            if (DrawBoneTreeNode(*Skeleton, Root, BoneFilter, DisplayValue, Selected))
                            {
                                DisplayValue = Selected;
                                Result = EPropertyChangeOp::Updated;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }
                    ImGui::PopStyleVar();
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }

        if (bSocketPicker)
        {
            ImGui::SameLine(0, 0);
            const bool bHasSockets = SocketData != nullptr &&
                (!SocketData->Sockets.empty() || (SocketData->Skeleton != nullptr && SocketData->Skeleton->GetNumBones() > 0));
            ImGui::BeginDisabled(!bHasSockets);
            if (ImGui::Button(LE_ICON_MENU_DOWN "##SocketPick", ImVec2(ButtonWidth, 0)))
            {
                BoneFilter.Clear();
                ImGui::OpenPopup("##SocketPicker");
            }
            ImGui::EndDisabled();

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal(bHasSockets ? "Pick a socket (or bone) on the parent's mesh"
                                                         : "Parent has no mesh sockets");
            }

            if (ImGui::BeginPopup("##SocketPicker"))
            {
                BoneFilter.Draw("##Filter", 320.0f);

                if (ImGui::BeginChild("##SocketList", ImVec2(340, 400), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14.0f);

                    if (ImGui::Selectable("(none)", DisplayValue.IsNone()))
                    {
                        DisplayValue = FName();
                        Result = EPropertyChangeOp::Updated;
                        ImGui::CloseCurrentPopup();
                    }

                    if (SocketData != nullptr && !SocketData->Sockets.empty())
                    {
                        ImGui::SeparatorText("Sockets");
                        for (const FName& Socket : SocketData->Sockets)
                        {
                            if (Socket.IsNone() || !BoneFilter.PassFilter(Socket.c_str()))
                            {
                                continue;
                            }
                            if (ImGui::Selectable(Socket.c_str(), Socket == DisplayValue))
                            {
                                DisplayValue = Socket;
                                Result = EPropertyChangeOp::Updated;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    if (SocketData != nullptr && SocketData->Skeleton != nullptr)
                    {
                        ImGui::SeparatorText("Bones");
                        for (int32 Root : SocketData->Skeleton->GetRootBones())
                        {
                            FName Selected;
                            if (DrawBoneTreeNode(*SocketData->Skeleton, Root, BoneFilter, DisplayValue, Selected))
                            {
                                DisplayValue = Selected;
                                Result = EPropertyChangeOp::Updated;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    ImGui::PopStyleVar();
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }

        return Result;
    }

    void FNamePropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        CachedValue = DisplayValue;
        Property->SetValue(CachedValue);
    }

    void FNamePropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FName ActualValue;
        Property->GetValue(&ActualValue);
        
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }

    EPropertyChangeOp FStringPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        // "FilePath" meta turns the field into an asset-path picker ("..." button, searchable).
        const bool bFilePath = Property->Property->HasMetadata("FilePath");
        // "Multiline" meta turns the field into a wrapping multi-line box (newlines allowed).
        const bool bMultiline = Property->Property->HasMetadata("Multiline");
        const float ButtonWidth = bFilePath ? ImGui::GetFrameHeight() : 0.0f;

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        char Buffer[1024];
        strncpy(Buffer, DisplayValue.c_str(), sizeof(Buffer));
        Buffer[sizeof(Buffer) - 1] = '\0';

        if (bMultiline)
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f);
            if (ImGui::InputTextMultiline("##ParamName", Buffer, sizeof(Buffer), Size))
            {
                DisplayValue = Buffer;
            }
        }
        else
        {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - ButtonWidth);
            if (ImGui::InputText("##ParamName", Buffer, sizeof(Buffer)))
            {
                DisplayValue = Buffer;
            }
            ImGui::PopItemWidth();
        }

        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            Result = EPropertyChangeOp::Updated;
        }

        if (bFilePath)
        {
            ImGui::SameLine(0, 0);
            if (ImGui::Button(LE_ICON_DOTS_HORIZONTAL "##FilePathPick", ImVec2(ButtonWidth, 0)))
            {
                ImGui::OpenPopup("##FilePathPicker");
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                ImGuiX::TextTooltip_Internal("Pick asset path");
            }

            if (ImGui::BeginPopup("##FilePathPicker"))
            {
                SearchFilter.Draw("##Search", 250.0f);
                if (ImGui::BeginChild("##PathList", ImVec2(300, 300)))
                {
                    TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; });
                    for (const FAssetData* Asset : Assets)
                    {
                        if (!SearchFilter.PassFilter(Asset->Path.c_str()))
                        {
                            continue;
                        }

                        if (ImGui::Selectable(Asset->Path.c_str()))
                        {
                            DisplayValue = Asset->Path.c_str();
                            Result = EPropertyChangeOp::Updated;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }

        return Result;
    }

    
    void FStringPropertyCustomization::UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property)
    {
        Property->SetValue(DisplayValue);
    }

    void FStringPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FString ActualValue;
        Property->GetValue(&ActualValue);
        
        DisplayValue = ActualValue;
    }
}
