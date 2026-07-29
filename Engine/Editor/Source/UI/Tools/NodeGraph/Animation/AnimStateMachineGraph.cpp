#include "AnimStateMachineGraph.h"
#include "AnimGraphSchema.h"
#include "AnimStateTransition.h"
#include "Nodes/AnimGraphNode_State.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"

#include <cfloat>
#include <cstdio>
#include <EASTL/algorithm.h>

namespace Lumina
{
    namespace
    {
        // State node IDs sit in the uint32 range (see CEdNodeGraph::AddNode), so
        // a directed (from, to) pair packs losslessly into a uint64 key.
        FORCEINLINE uint64 MakeTransitionKey(int64 FromNodeID, int64 ToNodeID)
        {
            return (uint64((uint32)FromNodeID) << 32) | (uint32)ToNodeID;
        }

        // Canvas presentation helpers. Namespaced rather than bare so the generic names don't
        // collide with another translation unit's helpers.
        namespace SM
        {
            constexpr float StateMinWidth  = 168.0f;
            constexpr float BadgeRounding  = 5.0f;
            constexpr float ArrowLength    = 13.0f;
            constexpr float ArrowWidth     = 9.0f;
            constexpr float ParallelOffset = 11.0f;

            const char* CompareSymbol(EAnimTransitionCompare Compare)
            {
                switch (Compare)
                {
                case EAnimTransitionCompare::Greater:      return ">";
                case EAnimTransitionCompare::GreaterEqual: return ">=";
                case EAnimTransitionCompare::Less:         return "<";
                case EAnimTransitionCompare::LessEqual:    return "<=";
                case EAnimTransitionCompare::Equal:        return "==";
                case EAnimTransitionCompare::NotEqual:     return "!=";
                }
                return "?";
            }

            ImVec2 Normalized(const ImVec2& V)
            {
                const float Length = Math::Sqrt(V.x * V.x + V.y * V.y);
                return Length > 1e-4f ? ImVec2(V.x / Length, V.y / Length) : ImVec2(1.0f, 0.0f);
            }

            // Where the ray from Center toward Target leaves the rect, pushed out by Inset.
            ImVec2 ProjectToBorder(const ImVec2& Center, const ImVec2& Target, const ImVec2& Min, const ImVec2& Max, float Inset)
            {
                const ImVec2 Dir = Target - Center;
                const float HalfW = Math::Max((Max.x - Min.x) * 0.5f + Inset, 1.0f);
                const float HalfH = Math::Max((Max.y - Min.y) * 0.5f + Inset, 1.0f);

                float T = FLT_MAX;
                if (Math::Abs(Dir.x) > 1e-4f)
                {
                    T = Math::Min(T, HalfW / Math::Abs(Dir.x));
                }
                if (Math::Abs(Dir.y) > 1e-4f)
                {
                    T = Math::Min(T, HalfH / Math::Abs(Dir.y));
                }
                return T == FLT_MAX ? Center : Center + Dir * T;
            }

            float DistanceToSegment(const ImVec2& P, const ImVec2& A, const ImVec2& B)
            {
                const ImVec2 AB = B - A;
                const float LengthSq = AB.x * AB.x + AB.y * AB.y;
                if (LengthSq < 1e-4f)
                {
                    const ImVec2 D = P - A;
                    return Math::Sqrt(D.x * D.x + D.y * D.y);
                }
                const float T = Math::Clamp(((P.x - A.x) * AB.x + (P.y - A.y) * AB.y) / LengthSq, 0.0f, 1.0f);
                const ImVec2 D = P - (A + AB * T);
                return Math::Sqrt(D.x * D.x + D.y * D.y);
            }

            void DrawArrowHead(ImDrawList* DL, const ImVec2& Tip, const ImVec2& Dir, ImU32 Color)
            {
                const ImVec2 Back = Tip - Dir * ArrowLength;
                const ImVec2 Side(-Dir.y * ArrowWidth * 0.5f, Dir.x * ArrowWidth * 0.5f);
                DL->AddTriangleFilled(Tip, Back + Side, Back - Side, Color);
            }
        }
    }

    FString CAnimStateTransition::GetConditionText() const
    {
        if (ConditionParameter.IsNone())
        {
            return FString("Always");
        }

        char Buffer[128] = {};
        snprintf(Buffer, sizeof(Buffer), "%s %s %g",
                 ConditionParameter.c_str(), SM::CompareSymbol(Compare), (double)CompareValue);
        return FString(Buffer);
    }

    void CAnimStateMachineGraph::EnsureSetup()
    {
        // Context-free: only touches nodes / the creatable-node registry, so it
        // is safe to call on a state machine the compiler readies but never opens.
        if (bSetupDone)
        {
            return;
        }
        bSetupDone = true;

        // Every state machine has exactly one (undeletable) Entry node.
        bool bHasEntry = false;
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid() && Node->IsA<CAnimGraphNode_StateEntry>())
            {
                bHasEntry = true;
                break;
            }
        }

        if (!bHasEntry)
        {
            CreateNode(CAnimGraphNode_StateEntry::StaticClass());
        }

        RegisterGraphNode(CAnimGraphNode_State::StaticClass());
        RegisterGraphNode(CAnimGraphNode_StateAny::StaticClass());

        ValidateGraph();
    }

    void CAnimStateMachineGraph::Initialize()
    {
        if (bInitialized)
        {
            return;
        }
        bInitialized = true;

        Super::Initialize();
        EnsureSetup();
    }

    void CAnimStateMachineGraph::Shutdown()
    {
        Super::Shutdown();
    }

    const FEdGraphSchema& CAnimStateMachineGraph::GetSchema() const
    {
        return GetAnimGraphSchema();
    }

    CAnimStateTransition* CAnimStateMachineGraph::FindTransition(int64 FromStateNodeID, int64 ToStateNodeID) const
    {
        for (const TObjectPtr<CAnimStateTransition>& Transition : Transitions)
        {
            if (Transition.IsValid() &&
                Transition->FromStateNodeID == FromStateNodeID &&
                Transition->ToStateNodeID == ToStateNodeID)
            {
                return Transition.Get();
            }
        }
        return nullptr;
    }

    CAnimGraphNode_State* CAnimStateMachineGraph::GetEntryState() const
    {
        for (CEdGraphNode* Node : Nodes)
        {
            CAnimGraphNode_StateEntry* Entry = Cast<CAnimGraphNode_StateEntry>(Node);
            if (Entry == nullptr || Entry->OutPin == nullptr || !Entry->OutPin->HasConnection())
            {
                continue;
            }
            return Cast<CAnimGraphNode_State>(Entry->OutPin->GetConnection(0)->GetOwningNode());
        }
        return nullptr;
    }

    void CAnimStateMachineGraph::GetOutgoingTransitions(int64 FromStateNodeID, TVector<CAnimStateTransition*>& Out) const
    {
        Out.clear();
        for (const TObjectPtr<CAnimStateTransition>& Transition : Transitions)
        {
            if (Transition.IsValid() && Transition->FromStateNodeID == FromStateNodeID)
            {
                Out.push_back(Transition.Get());
            }
        }

        eastl::stable_sort(Out.begin(), Out.end(), [](const CAnimStateTransition* A, const CAnimStateTransition* B)
        {
            return A->Priority < B->Priority;
        });
    }

    FString CAnimStateMachineGraph::GetEndpointLabel(int64 NodeID) const
    {
        for (CEdGraphNode* Node : Nodes)
        {
            if (Node->GetNodeID() != NodeID)
            {
                continue;
            }
            if (CAnimGraphNode_State* State = Cast<CAnimGraphNode_State>(Node))
            {
                return State->GetStateLabel();
            }
            if (Node->IsA<CAnimGraphNode_StateAny>())
            {
                return FString("Any State");
            }
            if (Node->IsA<CAnimGraphNode_StateEntry>())
            {
                return FString("Entry");
            }
        }
        return FString("(missing)");
    }

    void CAnimStateMachineGraph::ValidateGraph()
    {
        // Flatten live pin connections into the serialized list (PostLoad reads
        // it back to rewire pins on open) -- same contract as the other graphs.
        Connections.clear();
        Connections.reserve(16);

        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }

        // Reconcile transition objects against live wires into a State. Entry wires carry no
        // transition data; Any State wires do (they compile to a from-anywhere edge).
        THashSet<uint64> LiveKeys;

        for (CEdGraphNode* Node : Nodes)
        {
            CAnimGraphNode_State* ToState = Cast<CAnimGraphNode_State>(Node);
            if (ToState == nullptr)
            {
                continue;
            }

            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    CEdGraphNode* FromNode = Connection->GetOwningNode();
                    const bool bTransitionSource = FromNode != nullptr &&
                        (FromNode->IsA<CAnimGraphNode_State>() || FromNode->IsA<CAnimGraphNode_StateAny>());
                    if (!bTransitionSource)
                    {
                        continue;
                    }
                    LiveKeys.insert(MakeTransitionKey(FromNode->GetNodeID(), ToState->GetNodeID()));
                }
            }
        }

        // Drop transitions whose wire no longer exists.
        for (int32 i = (int32)Transitions.size() - 1; i >= 0; --i)
        {
            CAnimStateTransition* Transition = Transitions[i].Get();
            const bool bStale = Transition == nullptr ||
                LiveKeys.find(MakeTransitionKey(Transition->FromStateNodeID, Transition->ToStateNodeID)) == LiveKeys.end();
            if (bStale)
            {
                Transitions.erase(Transitions.begin() + i);
            }
        }

        // Create transition objects for newly-wired links.
        for (uint64 Key : LiveKeys)
        {
            const int64 FromNodeID = (int64)(uint32)(Key >> 32);
            const int64 ToNodeID   = (int64)(uint32)(Key & 0xFFFFFFFFull);

            if (FindTransition(FromNodeID, ToNodeID) == nullptr)
            {
                CAnimStateTransition* NewTransition = NewObject<CAnimStateTransition>(GetPackage());
                NewTransition->FromStateNodeID = FromNodeID;
                NewTransition->ToStateNodeID   = ToNodeID;
                Transitions.push_back(NewTransition);
            }
        }
    }

    void CAnimStateMachineGraph::PushGraphStyle() const
    {
        using namespace ax;

        // Zero strength collapses a link's bezier handles onto its endpoints, and the pins below
        // pivot at the node center, so the editor's own link is a straight center-to-center segment.
        // It stays invisible (see GetLinkStyle) and only serves as the hit and delete target for the
        // arrow DrawGraphOverlay draws in its place, hence killing its hover/selection decoration too.
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_LinkStrength, 0.0f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeRounding, 9.0f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodePadding, ImVec4(13.0f, 10.0f, 13.0f, 10.0f));
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_NodeBorderWidth, 1.5f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_HoveredNodeBorderWidth, 2.5f);
        NodeEditor::PushStyleVar(NodeEditor::StyleVar_SelectedNodeBorderWidth, 3.0f);

        const ImVec4 Invisible(0.0f, 0.0f, 0.0f, 0.0f);
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_SelLinkBorder, Invisible);
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_HovLinkBorder, Invisible);
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_HighlightLinkBorder, Invisible);
    }

    void CAnimStateMachineGraph::PopGraphStyle() const
    {
        using namespace ax;

        NodeEditor::PopStyleColor(3);
        NodeEditor::PopStyleVar(6);
    }

    void CAnimStateMachineGraph::GetLinkStyle(CEdNodeGraphPin*, CEdNodeGraphPin*, ImVec4& OutColor, float& OutThickness) const
    {
        // Every wire on this canvas is drawn by DrawGraphOverlay. The editor's own link stays
        // submitted but invisible so hover, selection and Delete keep working on it.
        OutColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        OutThickness = 6.0f;
    }

    bool CAnimStateMachineGraph::DrawCustomNode(CEdGraphNode* Node)
    {
        using namespace ax;

        CAnimGraphNode_State*      State = Cast<CAnimGraphNode_State>(Node);
        CAnimGraphNode_StateEntry* Entry = Cast<CAnimGraphNode_StateEntry>(Node);
        CAnimGraphNode_StateAny*   Any   = Cast<CAnimGraphNode_StateAny>(Node);

        if (State == nullptr && Entry == nullptr && Any == nullptr)
        {
            return false;
        }

        const FGraphDebugContext& Debug = GetDebugContext();
        const bool bActive = Debug.bEnabled && Debug.ActiveNodes != nullptr
            && Debug.ActiveNodes->find(Node) != Debug.ActiveNodes->end();

        ImVec4 Accent = ImGui::ColorConvertU32ToFloat4(Node->GetNodeTitleColor());
        if (bActive)
        {
            Accent = EditorColors::Warning();
        }
        else if (Node->HasError())
        {
            Accent = EditorColors::Danger();
        }

        const ImVec4 Background = bActive
            ? EditorColors::WithAlpha(Accent, 0.28f)
            : EditorColors::WithAlpha(EditorColors::PanelBg(), 0.96f);

        // Previous frame's rect: the pin strips and the center pivot are expressed in it. Zero on
        // the very first frame, corrected on the next one.
        const ImVec2 NodePos  = NodeEditor::GetNodePosition(Node->GetNodeID());
        const ImVec2 NodeSize = NodeEditor::GetNodeSize(Node->GetNodeID());
        const ImVec2 NodeMin  = NodePos;
        const ImVec2 NodeMax  = NodePos + ImVec2(Math::Max(NodeSize.x, 24.0f), Math::Max(NodeSize.y, 24.0f));
        const ImVec2 NodeCenter = (NodeMin + NodeMax) * 0.5f;

        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBg, Background);
        NodeEditor::PushStyleColor(NodeEditor::StyleColor_NodeBorder, EditorColors::WithAlpha(Accent, bActive ? 1.0f : 0.85f));

        NodeEditor::BeginNode(Node->GetNodeID());
        {
            ImGui::BeginVertical("state");

            if (State != nullptr)
            {
                const bool bIsEntryState = GetEntryState() == State;

                ImGui::BeginHorizontal("title");
                ImGui::TextColored(EditorColors::TextPrimary(), "%s", State->GetStateLabel().c_str());
                ImGui::Spring(1.0f, 12.0f);
                if (bIsEntryState)
                {
                    ImGui::TextColored(EditorColors::Success(), "%s", LE_ICON_PLAY);
                }
                if (bActive)
                {
                    ImGui::TextColored(EditorColors::Warning(), "%s", LE_ICON_LIGHTNING_BOLT);
                }
                ImGui::EndHorizontal();

                // Reserve the minimum width here rather than on the text row, so a long state name
                // is free to grow the box instead of being squeezed by the spring.
                ImGui::Dummy(ImVec2(SM::StateMinWidth, 1.0f));

                TVector<CAnimStateTransition*> Outgoing;
                GetOutgoingTransitions(State->GetNodeID(), Outgoing);

                ImGui::BeginHorizontal("sub");
                if (Outgoing.empty())
                {
                    ImGui::TextColored(EditorColors::TextMuted(), "no exits");
                }
                else
                {
                    ImGui::TextColored(EditorColors::TextDim(), "%d exit%s", (int32)Outgoing.size(), Outgoing.size() == 1 ? "" : "s");
                }
                ImGui::Spring(1.0f, 12.0f);
                if (State->BlendTree.IsValid())
                {
                    ImGui::TextColored(EditorColors::TextMuted(), "%s", LE_ICON_SITEMAP);
                }
                ImGui::EndHorizontal();
            }
            else
            {
                ImGui::BeginHorizontal("title");
                ImGui::TextColored(Accent, "%s", Entry != nullptr ? LE_ICON_PLAY " Entry" : LE_ICON_ARROW_EXPAND_HORIZONTAL " Any State");
                ImGui::EndHorizontal();
            }

            ImGui::EndVertical();

            // Edge strips rather than pin widgets: the middle of the box still drags the node, while
            // grabbing the left or right band starts (or accepts) a transition. Both pivot at the node
            // center so links run center to center.
            const float EdgeW = Math::Clamp((NodeMax.x - NodeMin.x) * 0.27f, 12.0f, 46.0f);

            CAnimGraphPin* InPin = State != nullptr ? State->InPin : nullptr;
            if (InPin != nullptr)
            {
                NodeEditor::BeginPin(InPin->GetPinGUID(), NodeEditor::PinKind::Input);
                NodeEditor::PinRect(NodeMin, ImVec2(NodeMin.x + EdgeW, NodeMax.y));
                NodeEditor::PinPivotRect(NodeCenter, NodeCenter);
                NodeEditor::EndPin();
            }

            CAnimGraphPin* OutPin = State != nullptr ? State->OutPin : (Entry != nullptr ? Entry->OutPin : Any->OutPin);
            if (OutPin != nullptr)
            {
                NodeEditor::BeginPin(OutPin->GetPinGUID(), NodeEditor::PinKind::Output);
                NodeEditor::PinRect(ImVec2(NodeMax.x - EdgeW, NodeMin.y), NodeMax);
                NodeEditor::PinPivotRect(NodeCenter, NodeCenter);
                NodeEditor::EndPin();
            }
        }
        NodeEditor::EndNode();

        NodeEditor::PopStyleColor(2);
        return true;
    }

    void CAnimStateMachineGraph::DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links)
    {
        using namespace ax;

        if (Links.empty())
        {
            return;
        }

        // Canvas space throughout: this runs before any node is submitted, so the wires land under
        // the state boxes and the canvas transform scales everything with zoom for free.
        ImDrawList* DL = ImGui::GetWindowDrawList();
        ImFont* Font = ImGui::GetFont();
        const float FontSize = ImGui::GetFontSize();

        const ImVec2 MousePos = ImGui::GetMousePos();
        const bool bCanPick = !NodeEditor::GetHoveredNode() && !NodeEditor::GetHoveredPin();

        int32 HoveredLink = INDEX_NONE;
        uint32 ClickLinkID = 0;

        for (int32 Index = 0; Index < (int32)Links.size(); ++Index)
        {
            CEdNodeGraphPin* InputPin  = Links[Index].first;
            CEdNodeGraphPin* OutputPin = Links[Index].second;
            if (InputPin == nullptr || OutputPin == nullptr)
            {
                continue;
            }

            CEdGraphNode* ToNode   = InputPin->GetOwningNode();
            CEdGraphNode* FromNode = OutputPin->GetOwningNode();
            if (ToNode == nullptr || FromNode == nullptr)
            {
                continue;
            }

            const uint32 LinkID = (uint32)Index + 1u;
            const bool bEntryWire = FromNode->IsA<CAnimGraphNode_StateEntry>();
            CAnimStateTransition* Transition = bEntryWire
                ? nullptr
                : FindTransition(FromNode->GetNodeID(), ToNode->GetNodeID());

            const ImVec2 FromMin = NodeEditor::GetNodePosition(FromNode->GetNodeID());
            const ImVec2 FromMax = FromMin + NodeEditor::GetNodeSize(FromNode->GetNodeID());
            const ImVec2 ToMin   = NodeEditor::GetNodePosition(ToNode->GetNodeID());
            const ImVec2 ToMax   = ToMin + NodeEditor::GetNodeSize(ToNode->GetNodeID());

            const ImVec2 FromCenter = (FromMin + FromMax) * 0.5f;
            const ImVec2 ToCenter   = (ToMin + ToMax) * 0.5f;
            if (Math::Abs(ToCenter.x - FromCenter.x) < 1.0f && Math::Abs(ToCenter.y - FromCenter.y) < 1.0f)
            {
                continue;
            }

            // A reciprocal transition gets both lines pushed off the center axis so they read as two
            // separate edges instead of one line with arrows at each end.
            const bool bHasReverse = !bEntryWire &&
                FindTransition(ToNode->GetNodeID(), FromNode->GetNodeID()) != nullptr;

            const ImVec2 Dir  = SM::Normalized(ToCenter - FromCenter);
            const ImVec2 Perp = ImVec2(-Dir.y, Dir.x) * (bHasReverse ? SM::ParallelOffset : 0.0f);

            const ImVec2 Start = SM::ProjectToBorder(FromCenter, ToCenter, FromMin, FromMax, 2.0f) + Perp;
            const ImVec2 End   = SM::ProjectToBorder(ToCenter, FromCenter, ToMin, ToMax, 2.0f) + Perp;
            const ImVec2 Mid   = (Start + End) * 0.5f;

            const bool bSelected = NodeEditor::IsLinkSelected(LinkID);

            // Badge geometry first: it doubles as the primary click target for the transition.
            FString BadgeText;
            if (bEntryWire)
            {
                BadgeText = "entry";
            }
            else if (Transition != nullptr)
            {
                BadgeText = Transition->GetConditionText();
            }
            else
            {
                BadgeText = "transition";
            }

            const ImVec2 TextSize = Font->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, BadgeText.c_str());
            const ImVec2 BadgePad(7.0f, 3.5f);
            const ImVec2 BadgeMin = Mid - ImVec2(TextSize.x * 0.5f + BadgePad.x, TextSize.y * 0.5f + BadgePad.y);
            const ImVec2 BadgeMax = Mid + ImVec2(TextSize.x * 0.5f + BadgePad.x, TextSize.y * 0.5f + BadgePad.y);

            const bool bOverBadge = bCanPick && MousePos.x >= BadgeMin.x && MousePos.x <= BadgeMax.x
                && MousePos.y >= BadgeMin.y && MousePos.y <= BadgeMax.y;
            const bool bOverLine  = bCanPick && SM::DistanceToSegment(MousePos, Start, End) <= 6.0f;
            const bool bHovered   = bOverBadge || bOverLine;

            if (bHovered)
            {
                HoveredLink = Index;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    ClickLinkID = LinkID;
                }
            }

            ImVec4 LineColor = bEntryWire ? EditorColors::Success() : EditorColors::TextDim();
            if (bSelected)
            {
                LineColor = EditorColors::Accent();
            }
            else if (bHovered)
            {
                LineColor = EditorColors::Lighten(LineColor, 0.25f);
            }
            else if (Transition != nullptr && Transition->ConditionParameter.IsNone())
            {
                // An unconditional edge fires the instant its source becomes active; call it out.
                LineColor = EditorColors::Warning();
            }

            const float Thickness = bSelected ? 3.2f : (bHovered ? 2.6f : 1.9f);
            const ImU32 LineU32 = EditorColors::U32(LineColor);

            // Stop the shaft short of the arrowhead so the tip stays crisp.
            const ImVec2 ShaftEnd = End - Dir * (SM::ArrowLength * 0.85f);
            DL->AddLine(Start, ShaftEnd, LineU32, Thickness);
            SM::DrawArrowHead(DL, End, Dir, LineU32);

            if (bSelected)
            {
                DL->AddCircleFilled(Start, 3.4f, LineU32);
            }

            const ImVec4 BadgeBg = bSelected
                ? EditorColors::WithAlpha(EditorColors::Accent(), 0.30f)
                : EditorColors::WithAlpha(EditorColors::WindowBg(), 0.94f);

            DL->AddRectFilled(BadgeMin, BadgeMax, EditorColors::U32(BadgeBg), SM::BadgeRounding);
            DL->AddRect(BadgeMin, BadgeMax, LineU32, SM::BadgeRounding, 0, bSelected || bHovered ? 1.8f : 1.1f);
            DL->AddText(Font, FontSize, ImVec2(Mid.x - TextSize.x * 0.5f, Mid.y - TextSize.y * 0.5f),
                        EditorColors::U32(bSelected ? EditorColors::TextPrimary() : EditorColors::TextDim()), BadgeText.c_str());

            // Blend duration and the interrupt flag ride just under the badge: the two things you
            // want to compare across edges without clicking each one.
            if (Transition != nullptr)
            {
                char Meta[64] = {};
                snprintf(Meta, sizeof(Meta), Transition->bCanInterrupt ? "%.2fs  " LE_ICON_LIGHTNING_BOLT : "%.2fs",
                         Transition->BlendDuration);
                const ImVec2 MetaSize = Font->CalcTextSizeA(FontSize * 0.85f, FLT_MAX, 0.0f, Meta);
                DL->AddText(Font, FontSize * 0.85f,
                            ImVec2(Mid.x - MetaSize.x * 0.5f, BadgeMax.y + 2.0f),
                            EditorColors::U32(EditorColors::WithAlpha(EditorColors::TextMuted(), 0.9f)), Meta);
            }
        }

        if (ClickLinkID != 0)
        {
            NodeEditor::ClearSelection();
            NodeEditor::SelectLink(ClickLinkID);
        }

        if (HoveredLink != INDEX_NONE)
        {
            CEdNodeGraphPin* InputPin  = Links[HoveredLink].first;
            CEdNodeGraphPin* OutputPin = Links[HoveredLink].second;
            CEdGraphNode* ToNode   = InputPin->GetOwningNode();
            CEdGraphNode* FromNode = OutputPin->GetOwningNode();

            // A tooltip is its own ImGui window, so the canvas has to be left first.
            NodeEditor::Suspend();
            ImGui::BeginTooltip();
            ImGui::TextColored(EditorColors::TextPrimary(), "%s  " LE_ICON_ARROW_RIGHT_BOLD "  %s",
                               GetEndpointLabel(FromNode->GetNodeID()).c_str(),
                               GetEndpointLabel(ToNode->GetNodeID()).c_str());

            if (CAnimStateTransition* Transition = FindTransition(FromNode->GetNodeID(), ToNode->GetNodeID()))
            {
                ImGui::Separator();
                ImGui::Text("Condition   %s", Transition->GetConditionText().c_str());
                ImGui::Text("Blend       %.2fs", Transition->BlendDuration);
                ImGui::Text("Priority    %d", Transition->Priority);
                ImGui::Text("Interrupt   %s", Transition->bCanInterrupt ? "can pre-empt an in-flight blend" : "runs to completion");
                if (FromNode->IsA<CAnimGraphNode_StateAny>())
                {
                    ImGui::TextColored(EditorColors::Warning(), "Checked from every state.");
                }
                ImGui::Separator();
                ImGui::TextColored(EditorColors::TextMuted(), "Click to edit. Select and press Delete to remove.");
            }
            else
            {
                ImGui::Separator();
                ImGui::TextColored(EditorColors::TextMuted(), "The state the machine starts in.");
            }
            ImGui::EndTooltip();
            NodeEditor::Resume();
        }
    }
}
