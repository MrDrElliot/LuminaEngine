#include "EditorPCH.h"
#include "InputActionCustomization.h"
#include "UI/Properties/NamePicker.h"
#include "UI/Properties/PropertyEditContexts.h"

#include "Core/Object/Cast.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_State.h"
#include "Renderer/SkeletonResource.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Tools/UI/ImGui/ImGuiX.h"

namespace Lumina
{
    namespace
    {
        // The outliner default pushes a 20-deep skeleton chain past the popup's edge.
        constexpr float kBonePickerIndent = 12.0f;

        constexpr ImVec2 kBoneTreeSize(360.0f, 400.0f);

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

        FFixedString MakePreview(const FName& Current, bool bStale)
        {
            FFixedString Preview;
            if (bStale)
            {
                Preview = LE_ICON_ALERT_CIRCLE_OUTLINE "  ";
            }
            Preview += Current.IsNone() ? "None" : Current.c_str();
            return Preview;
        }

        bool DrawNoneRow(const FName& Current, FNamePickerResult& OutResult)
        {
            if (ImGui::Selectable("None", Current.IsNone()))
            {
                OutResult = { true, FName() };
                ImGui::CloseCurrentPopup();
                return true;
            }
            return false;
        }

        // Shared by the two tree pickers, whose only difference is what sits above the bones.
        FNamePickerResult DrawBoneTree(const FNamePickerArgs& Args, const FSkeletonResource& Skeleton)
        {
            FNamePickerResult Result;

            FTreeListViewContext TreeContext;
            TreeContext.IndentPerDepth = kBonePickerIndent;
            TreeContext.RebuildTreeFunction = [&](FTreeListView& Tree)
            {
                Args.State->LastBuiltCount = BuildBoneTree(Tree, Skeleton, Args.State->Filter, Args.Current);
            };
            TreeContext.ItemSelectedFunction = [&](FTreeListView& Tree, FTreeNodeID Item, bool)
            {
                if (Tree.IsValid(Item))
                {
                    Result = { true, FName(Tree.Get<FTreeNodeDisplay>(Item).DisplayName.c_str()) };
                }
            };

            if (ImGui::BeginChild("##BoneTree", kBoneTreeSize))
            {
                Args.State->Tree.Draw(TreeContext);
                if (Args.State->LastBuiltCount == 0)
                {
                    ImGui::TextDisabled("No matching bones.");
                }
            }
            ImGui::EndChild();

            if (Result.bChanged)
            {
                ImGui::CloseCurrentPopup();
            }

            return Result;
        }

        bool BeginPickerCombo(const FNamePickerArgs& Args, bool bStale)
        {
            const FFixedString Preview = MakePreview(Args.Current, bStale);

            // Not PushItemWidth, whose matching pop would land on the popup window while the combo is open.
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const bool bOpen = ImGui::BeginCombo(Args.StrId, Preview.c_str(), ImGuiComboFlags_HeightLargest);

            if (bOpen && ImGui::IsWindowAppearing())
            {
                Args.State->Tree.MarkTreeDirty();
                ImGui::SetKeyboardFocusHere();
            }

            return bOpen;
        }

        void DrawFilterRow(const FNamePickerArgs& Args)
        {
            if (Args.State->Filter.Draw("##Filter", kBoneTreeSize.x))
            {
                Args.State->Tree.MarkTreeDirty();
            }
        }

        class FBonePickerSource : public INamePickerSource
        {
        public:

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                const FSkeletonResource* Skeleton = GetSkeleton(Context);
                if (Skeleton == nullptr)
                {
                    return;
                }

                Out.reserve(Skeleton->GetNumBones());
                for (int32 i = 0; i < Skeleton->GetNumBones(); ++i)
                {
                    Out.push_back(Skeleton->GetBone(i).Name);
                }
            }

            bool WantsCustomBody() const override { return true; }

            FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const override
            {
                const FSkeletonResource* Skeleton = GetSkeleton(Args.Context);
                const bool bStale = Skeleton == nullptr
                    ? !Args.Current.IsNone()
                    : (!Args.Current.IsNone() && Skeleton->FindBoneIndex(Args.Current) == INDEX_NONE);

                FNamePickerResult Result;

                if (BeginPickerCombo(Args, bStale))
                {
                    DrawFilterRow(Args);
                    ImGui::Separator();

                    if (!DrawNoneRow(Args.Current, Result) && Skeleton != nullptr)
                    {
                        Result = DrawBoneTree(Args, *Skeleton);
                    }
                    else if (Skeleton == nullptr)
                    {
                        ImGui::TextDisabled("This editor does not provide a skeleton.");
                    }

                    ImGui::EndCombo();
                }

                if (Skeleton == nullptr)
                {
                    ImGuiX::TextTooltip_Internal("This editor does not provide a skeleton to pick bones from");
                }
                else if (bStale)
                {
                    ImGuiX::TextTooltip_Internal("No bone of this name on the skeleton");
                }

                return Result;
            }

        private:

            static const FSkeletonResource* GetSkeleton(const FPropertyEditContext& Context)
            {
                const FSkeletonEditContext* Skel = Context.Get<FSkeletonEditContext>();
                return (Skel != nullptr) ? Skel->Skeleton : nullptr;
            }
        };

        class FSocketPickerSource : public INamePickerSource
        {
        public:

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                if (const FSocketEditContext* Sockets = Context.Get<FSocketEditContext>())
                {
                    Out = Sockets->Sockets;
                }
            }

            bool WantsCustomBody() const override { return true; }

            FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const override
            {
                const FSocketEditContext* Sockets = Args.Context.Get<FSocketEditContext>();
                const bool bHasAny = Sockets != nullptr && (!Sockets->Sockets.empty() || Sockets->Skeleton != nullptr);
                const bool bStale = bHasAny && !Args.Current.IsNone() && !Contains(*Sockets, Args.Current);

                FNamePickerResult Result;

                if (BeginPickerCombo(Args, bStale))
                {
                    DrawFilterRow(Args);
                    ImGui::Separator();

                    DrawNoneRow(Args.Current, Result);

                    if (Sockets != nullptr && !Sockets->Sockets.empty())
                    {
                        ImGui::SeparatorText("Sockets");
                        for (const FName& Socket : Sockets->Sockets)
                        {
                            if (Socket.IsNone() || !ImGuiX::PassSearchFilter(Args.State->Filter, Socket.c_str()))
                            {
                                continue;
                            }
                            if (ImGui::Selectable(Socket.c_str(), Socket == Args.Current))
                            {
                                Result = { true, Socket };
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    if (Sockets != nullptr && Sockets->Skeleton != nullptr)
                    {
                        ImGui::SeparatorText("Bones");
                        const FNamePickerResult FromTree = DrawBoneTree(Args, *Sockets->Skeleton);
                        if (FromTree.bChanged)
                        {
                            Result = FromTree;
                        }
                    }

                    if (!bHasAny)
                    {
                        ImGui::TextDisabled("The attach target has no sockets.");
                    }

                    ImGui::EndCombo();
                }

                if (!bHasAny)
                {
                    ImGuiX::TextTooltip_Internal("Nothing here provides sockets, so this attaches at the origin");
                }
                else if (bStale)
                {
                    ImGuiX::TextTooltip_Internal("No socket or bone of this name on the attach target");
                }

                return Result;
            }

        private:

            static bool Contains(const FSocketEditContext& Sockets, const FName& Name)
            {
                for (const FName& Socket : Sockets.Sockets)
                {
                    if (Socket == Name)
                    {
                        return true;
                    }
                }
                return Sockets.Skeleton != nullptr && Sockets.Skeleton->FindBoneIndex(Name) != INDEX_NONE;
            }
        };

        void GatherStructParameters(CAnimationGraph* Graph, bool bObjectValued, TVector<FName>& Out)
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

        class FAnimParameterSource : public INamePickerSource
        {
        public:

            explicit FAnimParameterSource(bool bInObjectValued) : bObjectValued(bInObjectValued) {}

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                const FAnimGraphEditContext* Anim = Context.Get<FAnimGraphEditContext>();
                GatherStructParameters((Anim != nullptr) ? Anim->Graph : nullptr, bObjectValued, Out);
            }

            const char* GetItemIcon() const override { return bObjectValued ? LE_ICON_CUBE_OUTLINE : LE_ICON_VARIABLE; }

            const char* GetStaleHint() const override { return "Not declared on this graph's parameter struct"; }

            const char* GetUnavailableHint(const FPropertyEditContext& Context) const override
            {
                const FAnimGraphEditContext* Anim = Context.Get<FAnimGraphEditContext>();
                if (Anim == nullptr || Anim->Graph == nullptr)
                {
                    return "This editor does not provide a graph to pick parameters from";
                }
                return (Anim->Graph->GetParameterStruct() == nullptr)
                    ? "No parameter struct assigned; set one on the graph asset"
                    : "The parameter struct declares no field of this type";
            }

        private:

            bool bObjectValued = false;
        };

        class FAnimCurveSource : public INamePickerSource
        {
        public:

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                const FAnimGraphEditContext* Anim = Context.Get<FAnimGraphEditContext>();
                if (Anim != nullptr && Anim->Graph != nullptr)
                {
                    Out = Anim->Graph->CurveNames;
                }
            }

            const char* GetItemIcon() const override { return LE_ICON_CHART_BELL_CURVE_CUMULATIVE; }

            // A Set Curve node names a curve no clip carries yet.
            bool AllowsCustomNames() const override { return true; }

            const char* GetStaleHint() const override
            {
                return "Not authored on any clip this graph references; compile to publish it";
            }

            const char* GetUnavailableHint(const FPropertyEditContext&) const override
            {
                return "No curves yet; author one on a clip, or type a name to create it";
            }
        };

        class FAnimStateSource : public INamePickerSource
        {
        public:

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                const FAnimStateMachineEditContext* Machine = Context.Get<FAnimStateMachineEditContext>();
                if (Machine == nullptr || Machine->Graph == nullptr)
                {
                    return;
                }

                for (CEdGraphNode* Node : Machine->Graph->Nodes)
                {
                    CAnimGraphNode_State* State = Cast<CAnimGraphNode_State>(Node);
                    if (State != nullptr && !State->StateName.IsNone())
                    {
                        Out.push_back(State->StateName);
                    }
                }
            }

            const char* GetItemIcon() const override { return LE_ICON_CIRCLE_OUTLINE; }

            const char* GetStaleHint() const override { return "No state of this name on this machine"; }

            const char* GetUnavailableHint(const FPropertyEditContext&) const override
            {
                return "No named states on this machine; a state must be named before it can be aliased";
            }
        };

        class FInputActionSource : public INamePickerSource
        {
        public:

            void GatherChoices(const FPropertyEditContext&, TVector<FName>&) const override {}

            // The rows carry each action's type and bindings, which no plain name list can show.
            bool WantsCustomBody() const override { return true; }

            FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const override
            {
                FString Picked = Args.Current.IsNone() ? FString() : FString(Args.Current.c_str());
                if (InputActionPicker::DrawCombo(Args.StrId, Picked, Args.State->Filter))
                {
                    return { true, Picked.empty() ? FName() : FName(Picked.c_str()) };
                }
                return {};
            }
        };
    }

    void NamePicker::RegisterBuiltInSources()
    {
        Register("Bone",            MakeShared<FBonePickerSource>());
        Register("Socket",          MakeShared<FSocketPickerSource>());
        Register("Parameter",       MakeShared<FAnimParameterSource>(false));
        Register("ObjectParameter", MakeShared<FAnimParameterSource>(true));
        Register("Curve",           MakeShared<FAnimCurveSource>());
        Register("InputAction",     MakeShared<FInputActionSource>());
        Register("AnimState",       MakeShared<FAnimStateSource>());
    }
}
