#pragma once

#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "AnimStateMachineGraph.generated.h"

namespace Lumina
{
    class CAnimGraphNode_State;
    class CAnimStateTransition;

    // State machine canvas whose wires are transitions, with data in CAnimStateTransition objects reconciled against live links.
    REFLECT()
    class CAnimStateMachineGraph : public CEdNodeGraph
    {
        GENERATED_BODY()
    public:

        void Initialize() override;
        void Shutdown() override;

        // Idempotent setup that needs no node-editor context, so the compiler can ready a never-opened machine.
        void EnsureSetup();

        const FEdGraphSchema& GetSchema() const override;

        // Reconciles the transition objects against the live State to State wires.
        void ValidateGraph() override;

        // Transitions are keyed by node id, so a cloned canvas has to re-key them onto its own nodes.
        void PostCloneContent(const CEdNodeGraph* Source, const THashMap<CEdGraphNode*, CEdGraphNode*>& Clones) override;

        // Null for links without transition data, such as the Entry wire.
        CAnimStateTransition* FindTransition(int64 FromStateNodeID, int64 ToStateNodeID) const;

        // The Any State edge into this state, which the runtime can only report by its destination.
        CAnimStateTransition* FindAnyStateTransitionTo(int64 ToStateNodeID) const;

        // Live blend readout, pushed by the editor tool each frame. Weight is the destination's share.
        void SetDebugTransition(const CAnimStateTransition* Transition, float Weight);

        // The State the Entry node wires to, or null when unwired.
        CAnimGraphNode_State* GetEntryState() const;

        // Transitions leaving State, in the order the runtime tests them (Priority, then wire order).
        void GetOutgoingTransitions(int64 FromStateNodeID, TVector<CAnimStateTransition*>& Out) const;

        // The state's name, or "Any State" or "Entry" for the special nodes.
        FString GetEndpointLabel(int64 NodeID) const;

        const TVector<TStrongObjectPtr<CAnimStateTransition>>& GetTransitions() const { return Transitions; }

        bool DrawCustomNode(CEdGraphNode* Node) override;
        void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const override;
        void DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links) override;
        void DrawLinkTooltip(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin) override;

        /** Transition data behind each State -> State wire. Reconciled in ValidateGraph. */
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CAnimStateTransition>> Transitions;

    private:

        // One-shot guards that are not serialized, with bInitialized also covering the node-editor context.
        bool bSetupDone = false;
        bool bInitialized = false;

        // Transition the VM is blending through right now, and how far along. Transient.
        const CAnimStateTransition* DebugTransition = nullptr;
        float                       DebugTransitionWeight = 0.0f;
    };
}
