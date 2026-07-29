#pragma once

#include "EdGraphNode.h"
#include "EdGraphSchema.h"
#include "GraphActionMenu.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "imgui-node-editor/imgui_node_editor.h"
#include "EdNodeGraph.generated.h"

namespace Lumina
{
    class CEdGraphNode;
    class CPackage;
}

namespace Lumina
{
    REFLECT()
    class CEdNodeGraph : public CObject
    {
        GENERATED_BODY()
        
    public:

        struct FNodeFactory
        {
            FString Name;
            FString Tooltip;
            TFunction<CEdGraphNode*()> CreationCallback;
        };

        
        CEdNodeGraph();
        ~CEdNodeGraph() override;

        virtual void Initialize();
        virtual void Shutdown();
        void Serialize(FArchive& Ar) override;
        void PostLoad() override;

        void DrawGraph();
        virtual void DrawGraphContextMenu();
        virtual void DrawNodeContextMenu(CEdGraphNode* Node);
        virtual void DrawPinContextMenu(CEdNodeGraphPin* Pin);

        virtual void ValidateGraph()  { }

        virtual CEdGraphNode* CreateNode(CClass* NodeClass);

        // Picks a pin on NewNode to auto-connect to SourcePin after a drag-from-pin creates a node.
        // Default returns the first opposite-direction pin the schema accepts; override for type-aware matching.
        virtual CEdNodeGraphPin* FindAutoConnectPin(CEdGraphNode* NewNode, CEdNodeGraphPin* SourcePin) const;

        // Connects SourcePin to TargetPin honoring the graph schema (breaks existing single-input links
        // first). No-op if either pin is null or the schema rejects the connection.
        void TryAutoConnect(CEdNodeGraphPin* SourcePin, CEdNodeGraphPin* TargetPin);

        virtual CEdGraphNode* OnNodeRemoved(CEdGraphNode* Node) { return nullptr; }

        // Quick-place hooks.
        virtual void HandleQuickPlace(int Digit, ImVec2 CanvasPos) {}
        virtual void HandleQuickPlace(char Key, ImVec2 CanvasPos) {}

        // Positions Node at a screen point on the next draw. Screen->canvas conversion needs the
        // node-editor context, which is only current inside DrawGraph, so callers outside it (drop
        // handlers, external spawners) queue the placement instead of computing it themselves.
        void QueueNodePlacement(CEdGraphNode* Node, ImVec2 ScreenPos);

        // When non-null, instantiated on double-clicking a wire: inserted at the click and the wire
        // reroutes through it. Default null; graphs wanting this UX (e.g. material) override.
        virtual CClass* GetRerouteNodeClass() const { return nullptr; }

        // Splits OutputPin -> InputPin by inserting a reroute node at CanvasPos.
        // No-op if GetRerouteNodeClass() returns null.
        CEdGraphNode* InsertRerouteOnLink(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin, ImVec2 CanvasPos);

        
        void SetNodeSelectedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { NodeSelectedCallback = Callback; }
        void SetPreNodeDeletedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { PreNodeDeletedCallback = Callback; }

        // Fired when a node is double-clicked on the canvas. The animation graph
        // editor uses this to descend into a node's sub-graph.
        void SetNodeDoubleClickedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { NodeDoubleClickedCallback = Callback; }

        // Fired every frame with the selected link's two pins, or (null, null) when none is selected.
        // The state machine canvas uses it to surface the selected transition in the properties panel.
        void SetLinkSelectedCallback(const TFunction<void(CEdNodeGraphPin*, CEdNodeGraphPin*)>& Callback) { LinkSelectedCallback = Callback; }

        // Transient per-frame debug overlay data, pushed by an asset editor while a graph is "running";
        // the draw loop animates link flow, prints pin values, highlights active nodes. Caller-owned, frame-only.
        struct FGraphDebugContext
        {
            bool                                            bEnabled = false;
            bool                                            bFlowLinks = false;
            const THashMap<CEdNodeGraphPin*, FString>*      PinValues = nullptr;
            const THashSet<const CEdGraphNode*>*            ActiveNodes = nullptr;
        };

        void SetDebugContext(const FGraphDebugContext& InContext) { DebugContext = InContext; }
        void ClearDebugContext() { DebugContext = FGraphDebugContext(); }
        const FGraphDebugContext& GetDebugContext() const { return DebugContext; }

        // Schema that governs what connections are allowed in this graph.
        virtual const FEdGraphSchema& GetSchema() const { return GetDefaultEdGraphSchema(); }

        // When true, the draw loop calls DrawPin() on each unconnected input pin so pins can render
        // inline editors (default values, enum combos) on the node face.
        virtual bool ShouldDrawInlinePinEditors() const { return false; }

        // Node-editor style pushed around the whole editor pass, popped by the matching hook. The
        // state machine canvas straightens links and hides their default decoration here.
        virtual void PushGraphStyle() const {}
        virtual void PopGraphStyle() const {}

        // Draws Node's visual in place of the default header/pin builder. Return true when handled;
        // link collection stays with the draw loop either way.
        virtual bool DrawCustomNode(CEdGraphNode* Node) { return false; }

        // Color and thickness of an emitted link. A graph that renders its own wires returns a
        // transparent color and keeps the editor's link purely for hit-testing and deletion.
        virtual void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImVec4& OutColor, float& OutThickness) const
        {
            OutColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            OutThickness = 1.0f;
        }

        // Draws the graph's wires itself. Called before any node is submitted, so coordinates are
        // canvas space and the drawing lands under the nodes. Links are indexed by (link id - 1).
        // Suspend the editor around any ImGui window (tooltip, popup) opened from here.
        virtual void DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links) {}

        // Package under which newly constructed nodes are allocated. Defaults to this graph's package,
        // so nodes live alongside the asset that owns the graph.
        virtual CPackage* GetNodeOuter();


    private:

        static bool GraphSaveSettings(const char* data, size_t size, ax::NodeEditor::SaveReasonFlags reason, void* userPointer);
        static size_t GraphLoadSettings(char* data, void* userPointer);

        // Compact reroute renderer (single dot, no header).
        void DrawRerouteNode(CEdGraphNode* Node);

        // Draws a pin's live debug value (when the debug context supplies one) as a
        // small colored token inline in the pin row. No-op when debug is off.
        void DrawPinDebugValue(CEdNodeGraphPin* Pin);

        // Placements requested outside the draw loop, applied (and cleared) on the next DrawGraph.
        struct FPendingPlacement
        {
            TObjectPtr<CEdGraphNode> Node;
            ImVec2                   ScreenPos;
        };
        TVector<FPendingPlacement> PendingPlacements;
        
    public:

        void RegisterGraphNode(CClass* InClass);
        
        uint64 AddNode(CEdGraphNode* InNode);

        /** All nodes currently in this graph. */
        PROPERTY()
        TVector<TObjectPtr<CEdGraphNode>>               Nodes;

        /** Serialized connection data encoding pin-to-pin links (pairs of 32-bit pin ids). */
        PROPERTY()
        TVector<uint32>                                 Connections;

        /** Serialized node editor layout state (positions, zoom, etc.). */
        PROPERTY()
        FString GraphSaveData;
        
        THashSet<CClass*>                               SupportedNodes;

        TFunction<void(CEdGraphNode*)>                  NodeSelectedCallback;
        TFunction<void(CEdGraphNode*)>                  PreNodeDeletedCallback;
        TFunction<void(CEdGraphNode*)>                  NodeDoubleClickedCallback;
        TFunction<void(CEdNodeGraphPin*, CEdNodeGraphPin*)> LinkSelectedCallback;

        int64                                           NextID = 0;

        FGraphActionMenu                                ActionMenu;

        // Pin the user dragged off when releasing on empty space. Consumed by the action menu
        // popup to auto-connect the new node back to this pin. Cleared when the popup closes.
        CEdNodeGraphPin*                                PendingSourcePin = nullptr;
        bool                                            bOpenCreateFromPin = false;

        TVector<CEdGraphNode*>                          CopiedNodes;
        
        bool                                            bFirstDraw = true;
        bool                                            bDebug = false;
        
        ax::NodeEditor::EditorContext* GetEditorContext() const { return Context; }

    private:

        ax::NodeEditor::EditorContext* Context = nullptr;

        // Set each frame by an asset editor's debug overlay; default is "off".
        FGraphDebugContext DebugContext;
    };
    
    
}
