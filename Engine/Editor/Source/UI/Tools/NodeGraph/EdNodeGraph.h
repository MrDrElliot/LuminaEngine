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
    // CenterX and CenterY name the coordinate that gets equalized, so CenterX lands the nodes in a column.
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

        // The node carrying this id, or null.
        CEdGraphNode* FindNode(int64 InNodeID) const;

        // Override for type-aware matching; the default takes the first opposite-direction pin the schema accepts.
        virtual CEdNodeGraphPin* FindAutoConnectPin(CEdGraphNode* NewNode, CEdNodeGraphPin* SourcePin) const;

        // Breaks an existing single-input link first, and no-ops if either pin is null or the schema refuses.
        void TryAutoConnect(CEdNodeGraphPin* SourcePin, CEdNodeGraphPin* TargetPin);

        virtual CEdGraphNode* OnNodeRemoved(CEdGraphNode* Node) { return nullptr; }

        // Quick-place hooks.
        virtual void HandleQuickPlace(int Digit, ImVec2 CanvasPos) {}
        virtual void HandleQuickPlace(char Key, ImVec2 CanvasPos) {}

        // Bumped when nodes or links are added or removed, never for a node move or a canvas pan.
        NODISCARD uint64 GetContentVersion() const { return ContentVersion; }

        // A load rebuilds every node through AddNode, which is not a content change.
        void NotifyContentChanged() { if (!bIsPostLoading) { ++ContentVersion; } }

        // Sums this graph with every sub-graph under it, so an edit inside a state's blend tree counts.
        NODISCARD uint64 GetTreeContentVersion() const;

        // Both versions are saved with the graph, so an asset that was saved compiled opens compiled.
        NODISCARD bool NeedsCompile() const { return GetTreeContentVersion() != CompiledContentVersion; }
        void MarkCompiled() { CompiledContentVersion = GetTreeContentVersion(); }

        // Must run inside DrawGraph. Links with both endpoints in the set are rebuilt, the rest dropped.
        void CloneNodes(const TVector<CEdGraphNode*>& SourceNodes, ImVec2 Delta);

        // Deletable selected nodes plus the canvas-space center of their boxes. Must run inside DrawGraph.
        void CollectSelectedNodesForClone(TVector<CEdGraphNode*>& OutNodes, ImVec2& OutPivot);

        // Deep-copies Source's nodes, links and layout into this empty graph, with no editor context.
        void CloneContentFrom(const CEdNodeGraph* Source);

        // Hook for graph data keyed by node id, handed every source node mapped to its clone.
        virtual void PostCloneContent(const CEdNodeGraph* Source, const THashMap<CEdGraphNode*, CEdGraphNode*>& Clones) {}

        // Gives any sub-graph reached twice its own copy, repairing assets saved when clones aliased.
        uint32 UnaliasSubGraphs(THashSet<CEdNodeGraph*>& Visited);

        // Must run inside DrawGraph, and no-ops below two selected nodes (three for the Distribute modes).
        void AlignSelectedNodes(ENodeAlignment Alignment);

        // Emits the "Alignment" menu items into an already-open popup.
        void DrawAlignmentMenuItems();

        // Tidy and unused-node entries, split out so a graph building its own menu can place them.
        void DrawGraphToolsMenuItems();

        // Everything reachable backwards from a root is what the graph compiles; the base returns false.
        virtual bool IsGraphRootNode(CEdGraphNode* Node) const { return false; }

        // Nodes reaching a root through their outputs, empty when the graph declares no roots.
        void CollectContributingNodes(THashSet<CEdGraphNode*>& OutContributing) const;

        // Nodes outside the contributing set, in graph order. These compile to nothing.
        void CollectDeadNodes(TVector<CEdGraphNode*>& OutDead) const;

        void SelectDeadNodes();
        void DeleteDeadNodes();

        // Queued rather than applied, since the layered layout needs sizes the editor has already drawn.
        void QueueTidyGraph() { bHasPendingTidy = true; }

        // Meaningless in a graph with no declared root, since nothing there can be measured as dead.
        bool bFadeDeadNodes = true;

        // Screen to canvas conversion needs the editor context, so callers outside DrawGraph queue instead.
        void QueueNodePlacement(CEdGraphNode* Node, ImVec2 ScreenPos);

        // Runs at the tail of DrawGraph with the host window current, the only place a canvas-wide drop target works.
        virtual void DrawCanvasDropTarget() {}

        // When non-null, double-clicking a wire inserts one of these and reroutes the wire through it.
        virtual CClass* GetRerouteNodeClass() const { return nullptr; }

        // Splits the link with a reroute node at CanvasPos, unless GetRerouteNodeClass returns null.
        CEdGraphNode* InsertRerouteOnLink(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin, ImVec2 CanvasPos);

        
        void SetNodeSelectedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { NodeSelectedCallback = Callback; }
        void SetPreNodeDeletedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { PreNodeDeletedCallback = Callback; }

        // The animation graph editor uses this to descend into a node's sub-graph.
        void SetNodeDoubleClickedCallback(const TFunction<void(CEdGraphNode*)>& Callback) { NodeDoubleClickedCallback = Callback; }

        // Fired every frame with the selected link's two pins, or (null, null) when none is selected.
        void SetLinkSelectedCallback(const TFunction<void(CEdNodeGraphPin*, CEdNodeGraphPin*)>& Callback) { LinkSelectedCallback = Callback; }

        // Caller-owned overlay data, valid for the one frame an asset editor pushes it.
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

        // When true, unconnected input pins get a DrawPin call so they can render an inline editor.
        virtual bool ShouldDrawInlinePinEditors() const { return false; }

        // Node-editor style pushed around the whole editor pass, popped by the matching hook.
        virtual void PushGraphStyle() const {}
        virtual void PopGraphStyle() const {}

        // Draws Node in place of the default builder. Return true when handled; links are collected either way.
        virtual bool DrawCustomNode(CEdGraphNode* Node) { return false; }

        // A graph rendering its own wires returns a transparent color and keeps the link for hit-testing.
        virtual void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImVec4& OutColor, float& OutThickness) const
        {
            OutColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            OutThickness = 1.0f;
        }

        // A graph drawing its own wires returns false, since flow markers ignore the link's alpha.
        virtual bool WantsDebugLinkFlow() const { return true; }

        // Called before any node is submitted, so coordinates are canvas space and wires land under the nodes.
        virtual void DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links) {}

        // A wire is identified by its two endpoints, so ids survive deleting some other wire.
        static uint64 MakeLinkID(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin);

        // Package for newly constructed nodes, defaulting to this graph's own package.
        virtual CPackage* GetNodeOuter();


    private:

        static bool GraphSaveSettings(const char* data, size_t size, ax::NodeEditor::SaveReasonFlags reason, void* userPointer);
        static size_t GraphLoadSettings(char* data, void* userPointer);

        // F2 on a single selected renamable node. Must run inside DrawGraph's suspended region.
        void HandleRenameShortcut();

        // Guarded against the aliased sub-graphs UnaliasSubGraphs repairs, which would otherwise recurse forever.
        uint64 AccumulateTreeContentVersion(THashSet<const CEdNodeGraph*>& Visited) const;

        // Compact reroute renderer (single dot, no header).
        void DrawRerouteNode(CEdGraphNode* Node);

        // Draws a pin's live debug value inline in the pin row when the debug context supplies one.
        void DrawPinDebugValue(CEdNodeGraphPin* Pin);

        // Placements requested outside the draw loop, applied (and cleared) on the next DrawGraph.
        struct FPendingPlacement
        {
            TObjectPtr<CEdGraphNode> Node;
            ImVec2                   ScreenPos;
        };
        TVector<FPendingPlacement> PendingPlacements;

        // Applied on the next draw, since menus are drawn inside a Suspend block.
        bool           bHasPendingAlignment = false;
        ENodeAlignment PendingAlignment     = ENodeAlignment::Left;

        // Same deferral as alignment, since NodeEditor::GetNodeSize needs a node the editor has laid out.
        bool           bHasPendingTidy      = false;

        // Applies the layered layout. Must run inside DrawGraph.
        void TidyGraph();

        // Node the "Node Context Menu" popup was opened on; the popup outlives the frame that spawned it.
        uint64         ContextMenuNodeID = 0;

        // Set while PostLoad rebuilds, so AddNode does not reconcile against links that are not back yet.
        bool           bIsPostLoading = false;

        // F2 rename box, opened a frame after the key since the popup lives in the suspended region.
        bool           bOpenRenamePopup = false;
        int64          RenameNodeID     = 0;
        char           RenameBuffer[128] = {};
        
    public:

        // Palette classes, built on first use from reflection so nothing is listed here. See FGraphNodeRegistry.
        const THashSet<CClass*>& GetSupportedNodes();

        // Per-instance escape hatch for a node discovery cannot reach. Prefer GetSupportedGraphClass.
        void RegisterGraphNode(CClass* InClass);

        uint64 AddNode(CEdGraphNode* InNode);

        // A free node ID, preferring PreferredID. IDs must be unique within a graph and never zero.
        int64 GenerateUniqueNodeID(int64 PreferredID) const;

        // All nodes currently in this graph.
        PROPERTY()
        TVector<TObjectPtr<CEdGraphNode>>               Nodes;

        // Pin-to-pin links, stored as pairs of 32-bit pin ids.
        PROPERTY()
        TVector<uint32>                                 Connections;

        // Node editor layout state, meaning node positions and group sizes.
        PROPERTY()
        FString GraphSaveData;

        // Bumped by NotifyContentChanged. See GetContentVersion.
        PROPERTY()
        uint64 ContentVersion = 0;

        // ContentVersion at the last compile. A legacy graph loads 0 for both and so reads as compiled.
        PROPERTY()
        uint64 CompiledContentVersion = 0;
        
        // Reach this through GetSupportedNodes(), which fills it from reflection on first use.
        THashSet<CClass*>                               SupportedNodes;
        bool                                            bSupportedNodesBuilt = false;
        uint32                                          SupportedNodesGeneration = 0;

        TFunction<void(CEdGraphNode*)>                  NodeSelectedCallback;
        TFunction<void(CEdGraphNode*)>                  PreNodeDeletedCallback;
        TFunction<void(CEdGraphNode*)>                  NodeDoubleClickedCallback;
        TFunction<void(CEdNodeGraphPin*, CEdNodeGraphPin*)> LinkSelectedCallback;

        int64                                           NextID = 0;

        FGraphActionMenu                                ActionMenu;

        // Pin dragged off onto empty space, consumed by the action menu to auto-connect the new node.
        CEdNodeGraphPin*                                PendingSourcePin = nullptr;
        bool                                            bOpenCreateFromPin = false;

        bool                                            bFirstDraw = true;
        bool                                            bNeedsInitialFraming = true;
        bool                                            bDebug = false;
        
        ax::NodeEditor::EditorContext* GetEditorContext() const { return Context; }

    private:

        ax::NodeEditor::EditorContext* Context = nullptr;

        // Set each frame by an asset editor's debug overlay; default is "off".
        FGraphDebugContext DebugContext;
    };
    
    
}
