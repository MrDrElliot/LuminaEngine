#pragma once

#include "EdGraphNode.h"
#include "EdGraphSchema.h"
#include "GraphActionMenu.h"
#include "Containers/HashTable.h"
#include "Containers/Pair.h"
#include "Containers/Vector.h"
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
    // How AlignSelectedNodes arranges the selection. CenterX/CenterY are named for the coordinate they
    // equalize, not the axis the nodes end up spread along: CenterX gives every node the same center X
    // (a column), CenterY the same center Y (a row).
    enum class ENodeAlignment : uint8
    {
        Left,
        Right,
        Top,
        Bottom,
        CenterX,
        CenterY,
        DistributeX,   // even gaps between left/right edges, existing order preserved
        DistributeY,
    };

    REFLECT()
    class EDITOR_API CEdNodeGraph : public CObject
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

        // Bumped whenever the graph's CONTENT changes -- nodes or links added or removed. Node moves and
        // canvas pan/zoom deliberately do not count: they dirty the package but cannot invalidate
        // compiled output. Tools compare it against the value at their last compile to know whether one
        // is still needed.
        NODISCARD uint64 GetContentVersion() const { return ContentVersion; }
        void NotifyContentChanged() { ++ContentVersion; }

        // Clones SourceNodes into this graph, offset by Delta from their current positions, and selects
        // the clones. Links whose BOTH endpoints are in the set are rebuilt between the clones; links
        // reaching outside it are dropped, so a paste never rewires the graph it lands in. Must run
        // inside DrawGraph (node positions need the editor context).
        void CloneNodes(const TVector<CEdGraphNode*>& SourceNodes, ImVec2 Delta);

        // Moves the selected nodes into alignment. No-op below two selected nodes (and below three for
        // the Distribute modes). Must run inside DrawGraph: node geometry and SetNodePosition both need
        // the editor context current. The editor's own save hook marks the package dirty.
        void AlignSelectedNodes(ENodeAlignment Alignment);

        // Emits the "Alignment" menu items (assumes an open popup/menu). Shared by the node context
        // menu and anything else that wants to offer them.
        void DrawAlignmentMenuItems();

        // Tidy / unused-node entries. Emitted at the tail of the node context menu; split out so a graph
        // that builds its own menu can place them itself.
        void DrawGraphToolsMenuItems();

        // True for a node the graph's output flows out of. Everything reachable backwards from these is
        // what the graph actually compiles; anything else is dead weight the compiler never sees. The
        // base returns false, which disables the contribution features rather than guessing wrong.
        virtual bool IsGraphRootNode(CEdGraphNode* Node) const { return false; }

        // Nodes that reach a root through their outputs, i.e. the ones that actually affect the result.
        // Empty when the graph declares no roots. Cheap enough to call per frame on editor-sized graphs.
        void CollectContributingNodes(THashSet<CEdGraphNode*>& OutContributing) const;

        // Nodes NOT in the contributing set, in graph order. These compile to nothing.
        void CollectDeadNodes(TVector<CEdGraphNode*>& OutDead) const;

        void SelectDeadNodes();
        void DeleteDeadNodes();

        // Layered auto-layout: columns by distance from the root, rows ordered to reduce wire crossings.
        // Queued rather than applied directly, for the same reason as alignment -- it needs node SIZES,
        // which only exist once the editor has drawn them.
        void QueueTidyGraph() { bHasPendingTidy = true; }

        // Dead nodes are drawn faded. Off by default in graphs with no declared root (nothing to measure
        // "dead" against); the material graph turns it on.
        bool bFadeDeadNodes = true;

        // Positions Node at a screen point on the next draw. Screen->canvas conversion needs the
        // node-editor context, which is only current inside DrawGraph, so callers outside it (drop
        // handlers, external spawners) queue the placement instead of computing it themselves.
        void QueueNodePlacement(CEdGraphNode* Node, ImVec2 ScreenPos);

        // Called at the tail of DrawGraph, after the node-editor pass has ended and the host window is
        // current again, so an override can register a drag-drop target over the canvas and spawn nodes
        // from what lands on it. The node editor owns the canvas region and leaves no ImGui item to hang
        // a target on, which is why this is a hook rather than something a caller can bolt on outside.
        // Default no-op.
        virtual void DrawCanvasDropTarget() {}

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

        // Alignment picked from a context menu. Menus are drawn inside a Suspend block, so it is applied
        // on the next draw instead -- same deferral as PendingPlacements, and one code path for moves.
        uint64         ContentVersion = 0;

        bool           bHasPendingAlignment = false;
        ENodeAlignment PendingAlignment     = ENodeAlignment::Left;

        // Same deferral as alignment: TidyGraph reads NodeEditor::GetNodeSize, which is only valid for a
        // node the editor has already laid out at least once.
        bool           bHasPendingTidy      = false;

        // Applies the layered layout. Must run inside DrawGraph.
        void TidyGraph();

        // Node the "Node Context Menu" popup was opened on; the popup outlives the frame that spawned it.
        uint64         ContextMenuNodeID = 0;
        
    public:

        // Node classes this graph offers in its palette (and accepts on paste).
        //
        // Built on first use from reflection -- every CEdGraphNode subclass that names this graph's class
        // in GetSupportedGraphClass -- so nothing has to be listed here. See FGraphNodeRegistry.
        const THashSet<CClass*>& GetSupportedNodes();

        // Escape hatch for a node that discovery cannot reach: one whose supported graph is decided at
        // runtime, or a one-off a tool wants in a single graph instance. Unioned with the discovered set,
        // so it works called before or after the first GetSupportedNodes().
        //
        // Prefer GetSupportedGraphClass. This does not survive a cache rebuild for other graphs and, being
        // per-instance, is invisible to anyone reading the node's declaration.
        void RegisterGraphNode(CClass* InClass);

        uint64 AddNode(CEdGraphNode* InNode);

        // A free node ID, preferring PreferredID when it is usable. Node IDs must be unique within a
        // graph and never zero; see the definition for why a duplicate is not merely cosmetic.
        int64 GenerateUniqueNodeID(int64 PreferredID) const;

        /** All nodes currently in this graph. */
        PROPERTY()
        TVector<TObjectPtr<CEdGraphNode>>               Nodes;

        /** Serialized connection data encoding pin-to-pin links (pairs of 32-bit pin ids). */
        PROPERTY()
        TVector<uint32>                                 Connections;

        /** Serialized node editor layout state (positions, zoom, etc.). */
        PROPERTY()
        FString GraphSaveData;
        
        // Reach this through GetSupportedNodes(), which fills it from reflection on first use. Touching
        // it directly reads an empty set on a graph nobody has asked yet.
        THashSet<CClass*>                               SupportedNodes;
        bool                                            bSupportedNodesBuilt = false;
        uint32                                          SupportedNodesGeneration = 0;

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

        bool                                            bFirstDraw = true;
        bool                                            bDebug = false;
        
        ax::NodeEditor::EditorContext* GetEditorContext() const { return Context; }

    private:

        ax::NodeEditor::EditorContext* Context = nullptr;

        // Set each frame by an asset editor's debug overlay; default is "off".
        FGraphDebugContext DebugContext;
    };
    
    
}
