#include "CoreTypeCustomization.h"
#include "BonePickerContext.h"
#include "ParameterPickerContext.h"
#include "SocketPickerContext.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "Input/InputActionMap.h"
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
        // Grows the backing FString as the user types, so a multiline field isn't capped by a fixed
        // buffer (a shader body outgrows any reasonable one, and the overflow would be dropped).
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

        // Rebuilds the picker tree from a skeleton. While the filter is active, matches are added
        // as a flat list (a deep hierarchy under a filter is all indentation and no information);
        // otherwise the full hierarchy is built expanded, with the current value's row selected.
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
                if (bFiltering && !Filter.PassFilter(Bone.Name.c_str()))
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

        // Compact indent for picker popups; the outliner default pushes 20+-deep skeleton
        // chains past the popup's edge.
        constexpr float kBonePickerIndent = 12.0f;

        // "W, S, Mouse X" -- what actually triggers an action, shown as the picker row's tooltip so the
        // author doesn't have to open the Input settings to remember what a name is bound to.
        FString DescribeActionBindings(const SInputAction& Action)
        {
            FString Result;
            for (const SInputActionBinding& Binding : Action.Bindings)
            {
                FString Token;
                switch (Binding.Source)
                {
                case EInputAxisSource::MouseX:     Token = "Mouse X"; break;
                case EInputAxisSource::MouseY:     Token = "Mouse Y"; break;
                case EInputAxisSource::MouseWheel: Token = "Mouse Wheel"; break;
                default:
                    if (Binding.Key.IsMouse())
                    {
                        Token = "Mouse " + FInputActionMap::MouseButtonToString(Binding.Key.MouseButton);
                    }
                    else if (Binding.Key.IsKeyboard())
                    {
                        if (Binding.Key.bCtrl)  { Token += "Ctrl+"; }
                        if (Binding.Key.bShift) { Token += "Shift+"; }
                        if (Binding.Key.bAlt)   { Token += "Alt+"; }
                        Token += FInputActionMap::KeyToString(Binding.Key.Key);
                    }
                    break;
                }

                if (Token.empty())
                {
                    continue;
                }
                if (!Result.empty())
                {
                    Result += ", ";
                }
                Result += Token;
            }
            return Result.empty() ? FString("Unbound") : Result;
        }

        const char* ActionTypeLabel(EInputActionType Type)
        {
            switch (Type)
            {
            case EInputActionType::Axis1D: return "Axis";
            case EInputActionType::Axis2D: return "Axis 2D";
            default:                       return "Digital";
            }
        }

        // A searchable dropdown of the project's authored input actions (Settings > Engine > Input). Used
        // by any Name or String property tagged InputAction, which is what a C# InputAction / InputAxis
        // field mints as. Returns true when the user picked a value, writing it into Value.
        bool DrawInputActionCombo(const char* Id, FString& Value, ImGuiTextFilter& Filter)
        {
            bool bPicked = false;
            const TVector<SInputAction>& Actions = FInputActionMap::Get().GetAllActions();

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (!ImGui::BeginCombo(Id, Value.empty() ? "(none)" : Value.c_str()))
            {
                return false;
            }

            if (ImGui::IsWindowAppearing())
            {
                Filter.Clear();
                ImGui::SetKeyboardFocusHere();
            }
            Filter.Draw("##ActionFilter", 220.0f * ImGuiX::GetUIScale());
            ImGui::Separator();

            if (ImGui::Selectable("(none)", Value.empty()))
            {
                Value.clear();
                bPicked = true;
            }

            int32 Shown = 0;
            for (const SInputAction& Action : Actions)
            {
                if (Action.Name.IsNone())
                {
                    continue;
                }
                const FString ActionName(Action.Name.c_str());
                if (!Filter.PassFilter(ActionName.c_str()))
                {
                    continue;
                }

                ++Shown;
                if (ImGui::Selectable(ActionName.c_str(), Value == ActionName))
                {
                    Value = ActionName;
                    bPicked = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGuiX::TextTooltip_Internal((FString(ActionTypeLabel(Action.Type)) + ":  "
                        + DescribeActionBindings(Action)).c_str());
                }
            }

            if (Shown == 0)
            {
                ImGui::TextDisabled(Actions.empty()
                    ? "No input actions. Add them in Settings > Engine > Input."
                    : "No matching actions.");
            }

            ImGui::EndCombo();
            return bPicked;
        }
    }

    EPropertyChangeOp FNamePropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property)
    {
        // PROPERTY(Editable, InputAction): the name is one of the project's input actions, so pick it from
        // the authored list instead of typing it and finding out at runtime that it never matched.
        if (Property->Property->HasMetadata("InputAction"))
        {
            FString Picked = DisplayValue.IsNone() ? FString() : FString(DisplayValue.c_str());
            if (DrawInputActionCombo("##InputAction", Picked, SearchFilter))
            {
                DisplayValue = Picked.empty() ? FName() : FName(Picked.c_str());
                return EPropertyChangeOp::Updated;
            }
            return EPropertyChangeOp::None;
        }

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

                        // Only scalar keys can drive a value parameter; list the rest disabled so a
                        // missing key reads as "wrong type" rather than "gone".
                        if (!IsScalarBlackboardKey(Key.Type))
                        {
                            ImGui::TextDisabled("%s (%s)", Key.Name.c_str(), BlackboardKeyTypeLabel(Key.Type));
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
                    // None entry first: lets the user clear the selection.
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
        // The string form of the action picker: what a C# InputAction / InputAxis [Property] field mints as.
        if (Property->Property->HasMetadata("InputAction"))
        {
            if (DrawInputActionCombo("##InputAction", DisplayValue, SearchFilter))
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
            // Edited in place through the resize callback rather than a stack buffer -- the text can be
            // arbitrarily long, and the callback keeps the string sized to it.
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
        CachedValue = DisplayValue;
        Property->SetValue(DisplayValue);
    }

    void FStringPropertyCustomization::HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property)
    {
        FString ActualValue;
        Property->GetValue(&ActualValue);

        // Only adopt the property's value when it changed behind our back. This runs every frame, and an
        // unconditional copy would throw away the in-progress edit -- the commit only happens when the
        // field is deactivated, several frames after the last keystroke wrote DisplayValue.
        if (CachedValue != ActualValue)
        {
            CachedValue = DisplayValue = ActualValue;
        }
    }
}
