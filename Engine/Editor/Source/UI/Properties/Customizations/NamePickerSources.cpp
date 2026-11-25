#include "EditorPCH.h"
#include "InputActionCustomization.h"
#include "UI/Properties/NamePicker.h"
#include "UI/Properties/PropertyEditContexts.h"

#include "Core/Object/Cast.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateMachineGraph.h"
#include "UI/Tools/NodeGraph/Animation/Nodes/AnimGraphNode_State.h"
#include "Renderer/SkeletonResource.h"
#include "Containers/Algorithm.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "World/Entity/Components/Component.h"
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
            BoneNodes.SetNum(NumBones, InvalidTreeNode);

            for (int32 i = 0; i < NumBones; ++i)
            {
                const FSkeletonResource::FBoneInfo& Bone = Skeleton.GetBone(i);
                if (bFiltering && !ImGuiX::PassSearchFilter(Filter, *Bone.Name))
                {
                    continue;
                }

                // Bones are parents-before-children, so a parent's node exists before its children's.
                const FTreeNodeID Parent = (!bFiltering && Bone.ParentIndex != Constants::kIndexNone)
                    ? BoneNodes[Bone.ParentIndex]
                    : InvalidTreeNode;

                const FTreeNodeID Node = Tree.CreateNode(Parent, *Bone.Name);
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
            Preview += Current.IsNone() ? "None" : *Current;
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
                    Result = { true, FName(*Tree.Get<FTreeNodeDisplay>(Item).DisplayName) };
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
            const bool bOpen = ImGui::BeginCombo(Args.StrId, *Preview, ImGuiComboFlags_HeightLargest);

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

                Out.Reserve(Skeleton->GetNumBones());
                for (int32 i = 0; i < Skeleton->GetNumBones(); ++i)
                {
                    Out.Add(Skeleton->GetBone(i).Name);
                }
            }

            bool WantsCustomBody() const override { return true; }

            FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const override
            {
                const FSkeletonResource* Skeleton = GetSkeleton(Args.Context);
                const bool bStale = Skeleton == nullptr
                    ? !Args.Current.IsNone()
                    : (!Args.Current.IsNone() && Skeleton->FindBoneIndex(Args.Current) == Constants::kIndexNone);

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
                const bool bHasAny = Sockets != nullptr && (!Sockets->Sockets.IsEmpty() || Sockets->Skeleton != nullptr);
                const bool bStale = bHasAny && !Args.Current.IsNone() && !Contains(*Sockets, Args.Current);

                FNamePickerResult Result;

                if (BeginPickerCombo(Args, bStale))
                {
                    DrawFilterRow(Args);
                    ImGui::Separator();

                    DrawNoneRow(Args.Current, Result);

                    if (Sockets != nullptr && !Sockets->Sockets.IsEmpty())
                    {
                        ImGui::SeparatorText("Sockets");
                        for (const FName& Socket : Sockets->Sockets)
                        {
                            if (Socket.IsNone() || !ImGuiX::PassSearchFilter(Args.State->Filter, *Socket))
                            {
                                continue;
                            }
                            if (ImGui::Selectable(*Socket, Socket == Args.Current))
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
                return Sockets.Skeleton != nullptr && Sockets.Skeleton->FindBoneIndex(Name) != Constants::kIndexNone;
            }
        };

        // Deep enough for a vector inside a struct inside an input, and a bound on cycles through asset references.
        constexpr int32 kMaxParameterDepth = 5;

        constexpr ImVec2 kParameterTreeSize(420.0f, 440.0f);

        // One field a parameter path can reach, readable as the wanted kind, a way down to more fields, or both.
        struct FParameterField
        {
            const FProperty* Property = nullptr;
            FString          Path;
            bool             bReadable = false;
            CStruct*         Inner = nullptr;
            bool             bHops = false;
        };

        // A path follows at most one asset reference, matching what the graph resolves at runtime.
        template<typename TVisit>
        void ForEachParameterField(CStruct* Struct, const FString& Prefix, bool bObjectValued, bool bHopped, TVisit&& Visit)
        {
            Struct->ForEachProperty<FProperty>([&](FProperty* Property)
            {
                if (Property->HasSetterOrGetter())
                {
                    return;
                }

                FParameterField Field;
                Field.Property = Property;
                Field.Path = Prefix.IsEmpty() ? FString(*Property->GetPropertyName()) : Prefix + "." + *Property->GetPropertyName();
                const EAnimParamValueType Type = AnimParamValueTypeFromProperty(Property);
                Field.bReadable = Type != EAnimParamValueType::Unresolved && (Type == EAnimParamValueType::Object) == bObjectValued;
                if (Property->GetType() == EPropertyTypeFlags::Struct)
                {
                    Field.Inner = static_cast<const FStructProperty*>(Property)->GetStruct();
                }
                else if (Property->GetType() == EPropertyTypeFlags::Object && !bHopped)
                {
                    Field.Inner = static_cast<const FObjectProperty*>(Property)->GetPropertyClass();
                    Field.bHops = true;
                }
                if (Field.bReadable || Field.Inner != nullptr)
                {
                    Visit(Field);
                }
            });
        }

        void GatherParameterPaths(CStruct* Struct, const FString& Prefix, bool bObjectValued, bool bHopped, int32 Depth, TVector<FName>& Out)
        {
            ForEachParameterField(Struct, Prefix, bObjectValued, bHopped, [&](const FParameterField& Field)
            {
                if (Field.bReadable)
                {
                    Out.Add(FName(*Field.Path));
                }
                if (Field.Inner != nullptr && Depth < kMaxParameterDepth)
                {
                    GatherParameterPaths(Field.Inner, Field.Path, bObjectValued, bHopped || Field.bHops, Depth + 1, Out);
                }
            });
        }

        // The struct an input's component type names right now, so a type added since the last compile shows at once.
        CStruct* FindInputStruct(const SAnimGraphInput& Input)
        {
            return Input.Component.IsNone() ? nullptr : FindComponentStruct(Input.Component.ToString());
        }

        void GatherGraphParameterPaths(const CAnimationGraph* Graph, bool bObjectValued, TVector<FName>& Out)
        {
            if (Graph == nullptr)
            {
                return;
            }
            for (const SAnimGraphInput& Input : Graph->Inputs)
            {
                if (CStruct* Struct = FindInputStruct(Input))
                {
                    GatherParameterPaths(Struct, Input.Name.ToString(), bObjectValued, false, 0, Out);
                }
            }
            if (CStruct* Struct = Graph->GetParameterStruct())
            {
                GatherParameterPaths(Struct, FString(), bObjectValued, false, 0, Out);
            }
        }

        const char* DescribeFieldType(const FProperty* Property)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double: return "float";
            case EPropertyTypeFlags::Bool:   return "bool";
            case EPropertyTypeFlags::Enum:   return "enum";
            case EPropertyTypeFlags::Struct: return *static_cast<const FStructProperty*>(Property)->GetStruct()->GetName();
            case EPropertyTypeFlags::Object:
                {
                    const CClass* Class = static_cast<const FObjectProperty*>(Property)->GetPropertyClass();
                    return Class != nullptr ? *Class->GetName() : "asset";
                }
            default: return "int";
            }
        }

        // Called right after the row's item, whose line the hint joins at the right edge.
        void DrawTypeHint(const char* Text)
        {
            const float Width = ImGui::CalcTextSize(Text).x;
            const float RightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(Math::Max(RightEdge - Width, ImGui::GetCursorPosX() + 12.0f));
            ImGui::TextDisabled("%s", Text);
        }

        void PickField(const FParameterField& Field, FNamePickerResult& Result)
        {
            Result = { true, FName(*Field.Path) };
            ImGui::CloseCurrentPopup();
        }

        void DrawParameterFieldTree(CStruct* Struct, const FString& Prefix, bool bObjectValued, bool bHopped, int32 Depth,
                                    const FNamePickerArgs& Args, FNamePickerResult& Result)
        {
            ForEachParameterField(Struct, Prefix, bObjectValued, bHopped, [&](const FParameterField& Field)
            {
                const char* Name = *Field.Property->GetPropertyName();
                const bool bSelected = Args.Current == FName(*Field.Path);
                ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_SpanAvailWidth | (bSelected ? ImGuiTreeNodeFlags_Selected : 0);
                const char* Icon = Field.bReadable ? (bObjectValued ? LE_ICON_CUBE_OUTLINE : LE_ICON_VARIABLE) : LE_ICON_FOLDER_OUTLINE;

                ImGui::PushID(Name);
                if (Field.Inner != nullptr && Depth < kMaxParameterDepth)
                {
                    const bool bOpen = ImGui::TreeNodeEx("##Field", Flags | ImGuiTreeNodeFlags_OpenOnArrow, "%s  %s", Icon, Name);
                    if (Field.bReadable && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                    {
                        PickField(Field, Result);
                    }
                    DrawTypeHint(DescribeFieldType(Field.Property));
                    if (bOpen)
                    {
                        DrawParameterFieldTree(Field.Inner, Field.Path, bObjectValued, bHopped || Field.bHops, Depth + 1, Args, Result);
                        ImGui::TreePop();
                    }
                }
                else if (Field.bReadable)
                {
                    ImGui::TreeNodeEx("##Field", Flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s  %s", Icon, Name);
                    if (ImGui::IsItemClicked())
                    {
                        PickField(Field, Result);
                    }
                    DrawTypeHint(DescribeFieldType(Field.Property));
                }
                ImGui::PopID();
            });
        }

        // Inputs first, since they are where a graph's data usually comes from, then the instance's own parameter block.
        class FAnimParameterSource : public INamePickerSource
        {
        public:

            explicit FAnimParameterSource(bool bInObjectValued) : bObjectValued(bInObjectValued) {}

            void GatherChoices(const FPropertyEditContext& Context, TVector<FName>& Out) const override
            {
                const FAnimGraphEditContext* Anim = Context.Get<FAnimGraphEditContext>();
                GatherGraphParameterPaths((Anim != nullptr) ? Anim->Graph : nullptr, bObjectValued, Out);
            }

            bool WantsCustomBody() const override { return true; }

            FNamePickerResult DrawCustomBody(const FNamePickerArgs& Args) const override
            {
                const FAnimGraphEditContext* Anim = Args.Context.Get<FAnimGraphEditContext>();
                const CAnimationGraph* Graph = Anim != nullptr ? Anim->Graph : nullptr;

                TVector<FName> Choices;
                GatherGraphParameterPaths(Graph, bObjectValued, Choices);
                const bool bStale = Graph != nullptr && !Args.Current.IsNone() && !Algo::Contains(Choices, Args.Current);

                FNamePickerResult Result;
                if (BeginPickerCombo(Args, bStale))
                {
                    // At least the tree's own width, and the popup's when the field is wider, so type hints never sit under the scrollbar.
                    const float Width = Math::Max(ImGui::GetContentRegionAvail().x, kParameterTreeSize.x);
                    if (Args.State->Filter.Draw("##Filter", Width))
                    {
                        Args.State->Tree.MarkTreeDirty();
                    }
                    ImGui::Separator();

                    if (ImGui::BeginChild("##Parameters", ImVec2(Width, kParameterTreeSize.y)))
                    {
                        DrawNoneRow(Args.Current, Result);
                        if (Graph == nullptr)
                        {
                            ImGui::TextDisabled("This editor does not provide a graph to pick parameters from.");
                        }
                        else if (Args.State->Filter.IsActive())
                        {
                            DrawFiltered(Args, Choices, Result);
                        }
                        else
                        {
                            DrawSources(Args, *Graph, Result);
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndCombo();
                }

                if (bStale && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                {
                    ImGuiX::TextTooltip_Internal("Nothing this graph reads has this path; renamed or removed?");
                }
                return Result;
            }

        private:

            void DrawFiltered(const FNamePickerArgs& Args, const TVector<FName>& Choices, FNamePickerResult& Result) const
            {
                bool bAny = false;
                for (const FName& Choice : Choices)
                {
                    if (!ImGuiX::PassSearchFilter(Args.State->Filter, *Choice))
                    {
                        continue;
                    }
                    bAny = true;
                    if (ImGui::Selectable(*Choice, Choice == Args.Current))
                    {
                        Result = { true, Choice };
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (!bAny)
                {
                    ImGui::TextDisabled("Nothing matches.");
                }
            }

            void DrawSources(const FNamePickerArgs& Args, const CAnimationGraph& Graph, FNamePickerResult& Result) const
            {
                bool bAny = false;
                for (const SAnimGraphInput& Input : Graph.Inputs)
                {
                    CStruct* Struct = FindInputStruct(Input);
                    if (Struct == nullptr || Input.Name.IsNone())
                    {
                        continue;
                    }
                    bAny = true;
                    ImGui::PushID(*Input.Name);
                    const bool bOpen = ImGui::TreeNodeEx("##Input", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                         LE_ICON_PUZZLE_OUTLINE "  %s", *Input.Name);
                    DrawTypeHint(*Struct->GetName());
                    if (bOpen)
                    {
                        DrawParameterFieldTree(Struct, Input.Name.ToString(), bObjectValued, false, 0, Args, Result);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                if (CStruct* Struct = Graph.GetParameterStruct())
                {
                    bAny = true;
                    if (ImGui::TreeNodeEx("##Instance", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                          LE_ICON_TUNE_VARIANT "  Graph Parameters"))
                    {
                        DrawParameterFieldTree(Struct, FString(), bObjectValued, false, 0, Args, Result);
                        ImGui::TreePop();
                    }
                }

                if (!bAny)
                {
                    ImGui::TextWrapped("Nothing to read yet. Add an input in the graph editor's Parameters panel to read a component off the entity.");
                }
            }

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

                for (const auto& NodeRef : Machine->Graph->Nodes)
                {
                    CEdGraphNode* Node = NodeRef.Get();
                    CAnimGraphNode_State* State = Cast<CAnimGraphNode_State>(Node);
                    if (State != nullptr && !State->StateName.IsNone())
                    {
                        Out.Add(State->StateName);
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
                FString Picked = Args.Current.IsNone() ? FString() : FString(*Args.Current);
                if (InputActionPicker::DrawCombo(Args.StrId, Picked, Args.State->Filter))
                {
                    return { true, Picked.IsEmpty() ? FName() : FName(*Picked) };
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
