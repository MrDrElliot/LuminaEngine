#include "CoreTypeCustomization.h"
#include "BonePickerContext.h"
#include "ParameterPickerContext.h"
#include "SocketPickerContext.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "InputActionCustomization.h"
#include "Renderer/MeshData.h"
#include "UI/Tools/AssetEditors/TextureEditor/TextureEditorTool.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include <Assets/AssetRegistry/AssetData.h>
#include <Assets/AssetRegistry/AssetRegistry.h>
#include "Renderer/SkeletonResource.h"


namespace Lumina
{
    namespace
    {
        // Grows the backing FString as the user types, since a shader body outgrows any fixed buffer.
        int StringResizeCallback(ImGuiInputTextCallbackData* Data)
        {
            if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                FString* Str = static_cast<FString*>(Data->UserData);
                Str->resize(Data->BufTextLen);
                Data->Buf = Str->data();
            }
            return 0;
        }

        // Under a filter matches go in flat, since a deep hierarchy is all indentation and no information.
        int32 BuildBoneTree(FTreeListView& Tree, const FSkeletonResource& Skeleton, const ImGuiTextFilter& Filter, const FName& Current)
        {
            const int32 NumBones = Skeleton.GetNumBones();
            const bool bFiltering = Filter.IsActive();
            int32 Count = 0;

            TVector<FTreeNodeID> BoneNodes;
            BoneNodes.resize(NumBones, InvalidTreeNode);

            for (int32 i = 0; i < NumBones; ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = Skeleton.GetBone(i);
                if (bFiltering && !ImGuiX::PassSearchFilter(Filter, Bone.Name.c_str()))
                {
                    continue;
                }

                // Bones are parents-before-children, so a parent's node exists before its children's.
                const FTreeNodeID Parent = (!bFiltering && Bone.ParentIndex != INDEX_NONE)
                    ? BoneNodes[Bone.ParentIndex]
                    : InvalidTreeNode;

                const FTreeNodeID Node = Tree.CreateNode(Parent, Bone.Name.c_str());
                BoneNodes[i] = Node;

                FTreeNodeState& State = Tree.Get<FTreeNodeState>(Node);
                State.bExpanded = true;
                State.bSelected = Bone.Name == Current;
                ++Count;
            }

            return Count;
        }

        // The outliner default pushes a 20-deep skeleton chain past the popup's edge.
        constexpr float kBonePickerIndent = 12.0f;

        void GatherGraphParameters(CAnimationGraph* Graph, bool bObjectValued, TVector<FName>& Out)
        {
            CStruct* Struct = (Graph != nullptr) ? Graph->GetParameterStruct() : nullptr;
            if (Struct == nullptr)
            {
                return;
            }

            Struct->ForEachProperty<FProperty>([&](FProperty* Property)
            {
                const EAnimParamValueType Type = AnimParamValueTypeFromProperty(Property);
                if (Type == EAnimParamValueType::Unresolved || Property->HasSetterOrGetter())
                {
                    return;
                }
                if ((Type == EAnimParamValueType::Object) != bObjectValued)
                {
                    return;
                }
                Out.push_back(Property->Name);
            });
        }
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawNameCombo(const char* StrId, const TVector<FName>& Choices,
                                                                const char* ItemIcon, const char* StaleHint, const char* EmptyHint,
                                                                bool bAllowCreate)
    {
        // "None" occupies row 0, so every choice sits one slot further along.
        constexpr int32 NoneOffset = 1;
        const int32 Count = (int32)Choices.size() + NoneOffset;

        int32 CurrentIndex = DisplayValue.IsNone() ? 0 : INDEX_NONE;
        for (int32 i = 0; i < (int32)Choices.size(); ++i)
        {
            if (Choices[i] == DisplayValue)
            {
                CurrentIndex = i + NoneOffset;
                break;
            }
        }

        // A name the graph no longer offers still shows, flagged, rather than silently reading as None.
        const bool bStale = CurrentIndex == INDEX_NONE;
        FFixedString Preview;
        if (bStale)
        {
            Preview = LE_ICON_ALERT_CIRCLE_OUTLINE "  ";
        }
        Preview += DisplayValue.IsNone() ? "None" : DisplayValue.c_str();

        FFixedString Created;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        const int32 Picked = ImGuiX::SearchableCombo(StrId, Preview.c_str(), Count, CurrentIndex,
            [&Choices](int32 Index)
            {
                return (Index < NoneOffset) ? FFixedString("None") : FFixedString(Choices[Index - NoneOffset].c_str());
            }, ItemIcon, bAllowCreate ? &Created : nullptr);
        ImGui::PopItemWidth();

        if (!Created.empty())
        {
            DisplayValue = FName(Created.c_str());
            return EPropertyChangeOp::Updated;
        }

        const char* Hint = bStale ? StaleHint : (Choices.empty() ? EmptyHint : nullptr);
        if (Hint != nullptr)
        {
            ImGuiX::TextTooltip_Internal(Hint);
        }

        if (Picked != INDEX_NONE && Picked != CurrentIndex)
        {
            DisplayValue = (Picked < NoneOffset) ? FName() : Choices[Picked - NoneOffset];
            return EPropertyChangeOp::Updated;
        }

        return EPropertyChangeOp::None;
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawParameterCombo(CAnimationGraph* Graph, bool bObjectValued)
    {
        TVector<FName> Parameters;
        GatherGraphParameters(Graph, bObjectValued, Parameters);

        const char* EmptyHint = "No graph context";
        if (Graph != nullptr)
        {
            EmptyHint = (Graph->GetParameterStruct() == nullptr)
                ? "No parameter struct assigned; set one on the graph asset"
                : "The parameter struct declares no field of this type";
        }

        return DrawNameCombo("##ParamPick", Parameters, bObjectValued ? LE_ICON_CUBE_OUTLINE : LE_ICON_VARIABLE,
                             "Not declared on this graph's parameter struct", EmptyHint, false);
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawCurveCombo(CAnimationGraph* Graph)
    {
        static const TVector<FName> NoCurves;
        const TVector<FName>& Curves = (Graph != nullptr) ? Graph->CurveNames : NoCurves;

        // A Set Curve node names a curve no clip carries yet, so the typed text has to be enterable.
        return DrawNameCombo("##CurvePick", Curves, LE_ICON_CHART_BELL_CURVE_CUMULATIVE,
                             "Not authored on any clip this graph references; compile to publish it",
                             "No curves yet; author one on a clip, or type a name to create it", true);
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        // Picked from the authored list, rather than typed and found wrong at runtime.
        if (Property->Property->HasMetadata("InputAction"))
        {
            FString Picked = DisplayValue.IsNone() ? FString() : FString(DisplayValue.c_str());
            if (InputActionPicker::DrawCombo("##InputAction", Picked, SearchFilter))
            {
                DisplayValue = Picked.empty() ? FName() : FName(Picked.c_str());
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

        const bool bBonePicker = Property->Property->HasMetadata("BonePicker");
        const bool bObjectParamPicker = Property->Property->HasMetadata("ObjectParameterPicker");
        const bool bParamPicker = bObjectParamPicker || Property->Property->HasMetadata("ParameterPicker");
        const bool bCurvePicker = Property->Property->HasMetadata("CurvePicker");
        const bool bSocketPicker = Property->Property->HasMetadata("SocketPicker");
        const FSkeletonResource* Skeleton = bBonePicker ? BonePickerContext::GetActiveSkeleton() : nullptr;
        CAnimationGraph* PickerGraph = (bParamPicker || bCurvePicker) ? ParameterPickerContext::GetActiveGraph() : nullptr;
        const SocketPickerContext::FSocketPickerData* SocketData = bSocketPicker ? SocketPickerContext::GetActive() : nullptr;

        if (bParamPicker)
        {
            return DrawParameterCombo(PickerGraph, bObjectParamPicker);
        }

        if (bCurvePicker)
        {
            return DrawCurveCombo(PickerGraph);
        }

        EPropertyChangeOp Result = EPropertyChangeOp::None;

        const float ButtonWidth = (bBonePicker || bSocketPicker) ? ImGui::GetFrameHeight() : 0.0f;

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
                if (ImGui::IsWindowAppearing())
                {
                    BoneTree.MarkTreeDirty();
                    ImGui::SetKeyboardFocusHere();
                }
                if (BoneFilter.Draw("##Filter", 320.0f))
                {
                    BoneTree.MarkTreeDirty();
                }

                if (Skeleton != nullptr)
                {
                    // A None entry first, so the user can clear the selection.
                    if (ImGui::Selectable("(none)", DisplayValue.IsNone()))
                    {
                        DisplayValue = FName();
                        Result = EPropertyChangeOp::Updated;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Separator();

                    bool bPicked = false;

                    FTreeListViewContext TreeContext;
                    TreeContext.IndentPerDepth = kBonePickerIndent;
                    TreeContext.RebuildTreeFunction = [&](FTreeListView& Tree)
                    {
                        LastBuiltBoneCount = BuildBoneTree(Tree, *Skeleton, BoneFilter, DisplayValue);
                    };
                    TreeContext.ItemSelectedFunction = [&](FTreeListView& Tree, FTreeNodeID Item, bool)
                    {
                        if (Tree.IsValid(Item))
                        {
                            DisplayValue = FName(Tree.Get<FTreeNodeDisplay>(Item).DisplayName.c_str());
                            bPicked = true;
                        }
                    };

                    if (ImGui::BeginChild("##BoneTree", ImVec2(360, 400)))
                    {
                        BoneTree.Draw(TreeContext);
                        if (LastBuiltBoneCount == 0)
                        {
                            ImGui::TextDisabled("No matching bones.");
                        }
                    }
                    ImGui::EndChild();

                    if (bPicked)
                    {
                        Result = EPropertyChangeOp::Updated;
                        ImGui::CloseCurrentPopup();
                    }
                }
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
                if (ImGui::IsWindowAppearing())
                {
                    BoneTree.MarkTreeDirty();
                    ImGui::SetKeyboardFocusHere();
                }
                if (BoneFilter.Draw("##Filter", 320.0f))
                {
                    BoneTree.MarkTreeDirty();
                }

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
                        if (Socket.IsNone() || !ImGuiX::PassSearchFilter(BoneFilter, Socket.c_str()))
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

                    bool bPicked = false;

                    FTreeListViewContext TreeContext;
                    TreeContext.IndentPerDepth = kBonePickerIndent;
                    TreeContext.RebuildTreeFunction = [&](FTreeListView& Tree)
                    {
                        LastBuiltBoneCount = BuildBoneTree(Tree, *SocketData->Skeleton, BoneFilter, DisplayValue);
                    };
                    TreeContext.ItemSelectedFunction = [&](FTreeListView& Tree, FTreeNodeID Item, bool)
                    {
                        if (Tree.IsValid(Item))
                        {
                            DisplayValue = FName(Tree.Get<FTreeNodeDisplay>(Item).DisplayName.c_str());
                            bPicked = true;
                        }
                    };

                    if (ImGui::BeginChild("##SocketBoneTree", ImVec2(360, 400)))
                    {
                        BoneTree.Draw(TreeContext);
                        if (LastBuiltBoneCount == 0)
                        {
                            ImGui::TextDisabled("No matching bones.");
                        }
                    }
                    ImGui::EndChild();

                    if (bPicked)
                    {
                        Result = EPropertyChangeOp::Updated;
                        ImGui::CloseCurrentPopup();
                    }
                }
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
        // The string form of the picker, matching what a C# InputAction property field mints.
        if (Property->Property->HasMetadata("InputAction"))
        {
            if (InputActionPicker::DrawCombo("##InputAction", DisplayValue, SearchFilter))
            {
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

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
            // Edited in place through the resize callback, since the text can be arbitrarily long.
            ImGui::InputTextMultiline("##ParamName", DisplayValue.data(), DisplayValue.capacity() + 1, Size,
                                      ImGuiInputTextFlags_CallbackResize, StringResizeCallback, &DisplayValue);
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
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere(-1);
                }
                if (ImGui::BeginChild("##PathList", ImVec2(300, 300)))
                {
                    TVector<FAssetData*> Assets = FAssetRegistry::Get().FindByPredicate([](const FAssetData&) { return true; });
                    for (const FAssetData* Asset : Assets)
                    {
                        if (!ImGuiX::PassSearchFilter(SearchFilter, Asset->Path.c_str()))
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
        CachedValue = DisplayValue;
        Property->SetValue(DisplayValue);
    }

    void FStringPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FString ActualValue;
        Property->GetValue(&ActualValue);

        // An unconditional copy would throw away an in-progress edit, which commits only on deactivate.
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
