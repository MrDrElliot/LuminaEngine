#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimStateMachineGraph.generated.h"

namespace Lumina
{
    class CAnimGraphNode_State;
    class CAnimStateTransition;

    // State machine canvas: State nodes plus one Entry node, wires between States are transitions.
    // Transition data lives in CAnimStateTransition objects, reconciled against live links on edit.
    // Draws itself rather than using the generic node/wire look: state boxes, straight arrows, and a
    // clickable badge per transition.
    REFLECT()
    class CAnimStateMachineGraph : public CEdNodeGraph
    {
        GENERATED_BODY()
    public:

        void Initialize() override;
        void Shutdown() override;

        // Context-free setup (no node-editor context needed): ensures the Entry node and registers the
        // State node as creatable, so the compiler can ready a never-opened state machine. Idempotent.
        void EnsureSetup();

        const FEdGraphSchema& GetSchema() const override;

        // Rebuilds the serialized connection list and reconciles the transition
        // objects against the live State -> State wires.
        void ValidateGraph() override;

        // Finds the transition object bound to a State -> State link, or null
        // (e.g. the Entry wire, which has no transition data).
        CAnimStateTransition* FindTransition(int64 FromStateNodeID, int64 ToStateNodeID) const;

        // The State the Entry node wires to, or null when unwired.
        CAnimGraphNode_State* GetEntryState() const;

        // Transitions leaving State, in the order the runtime tests them (Priority, then wire order).
        void GetOutgoingTransitions(int64 FromStateNodeID, TVector<CAnimStateTransition*>& Out) const;

        // Label for whichever end of a transition a node id refers to: the state's name, or
        // "Any State" / "Entry" for the special nodes.
        FString GetEndpointLabel(int64 NodeID) const;

        const TVector<TObjectPtr<CAnimStateTransition>>& GetTransitions() const { return Transitions; }

        void PushGraphStyle() const override;
        void PopGraphStyle() const override;
        bool DrawCustomNode(CEdGraphNode* Node) override;
        void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImVec4& OutColor, float& OutThickness) const override;
        void DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links) override;

        /** Transition data behind each State -> State wire. Reconciled in ValidateGraph. */
        PROPERTY()
        TVector<TObjectPtr<CAnimStateTransition>> Transitions;

    private:

        // One-shot guards; not serialized. bSetupDone covers context-free setup; bInitialized covers
        // full Initialize() (which additionally creates the node-editor context).
        bool bSetupDone = false;
        bool bInitialized = false;
    };
}
