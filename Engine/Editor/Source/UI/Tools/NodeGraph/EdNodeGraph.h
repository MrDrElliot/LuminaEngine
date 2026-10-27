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
#include "UI/Tools/Transactions/NodeGraphSnapshotCommand.h"
#include "GraphEditor.h"
#include "EdNodeGraph.generated.h"

namespace Lumina
{
    class CEdGraphNode;
    class CEdNode_Comment;
    class CPackage;
    class FGraphNodeBuilder;
    class FTransactionManager;
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

    // Wires bundled together where they cross the tie, each wire named by its output and input pin ids.
    REFLECT()
    struct SGraphZipTie
    {
        GENERATED_BODY()

        PROPERTY()
        uint32 ID = 0;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        PROPERTY()
        TVector<uint32> Links;
    };

    // A view saved to a number key, as its canvas center and zoom.
    REFLECT()
    struct SGraphBookmark
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Slot = 0;

        PROPERTY()
        float X = 0.0f;

        PROPERTY()
        float Y = 0.0f;

        PROPERTY()
        float Zoom = 1.0f;
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
        CEdNodeGraph(const CEdNodeGraph&) = delete;
        CEdNodeGraph& operator=(const CEdNodeGraph&) = delete;
        CEdNodeGraph(CEdNodeGraph&&) = delete;
        CEdNodeGraph& operator=(CEdNodeGraph&&) = delete;

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

        // An edit worth an undo step that leaves the compiled result alone, such as recoloring a comment.
        void NotifyCosmeticChange() { bLayoutChangedSinceImage = true; }

        // Spawns a comment around the selected nodes, or at the mouse with nothing selected.
        CEdNode_Comment* WrapSelectionInComment();

        // Sums this graph with every sub-graph under it, so an edit inside a state's blend tree counts.
        NODISCARD uint64 GetTreeContentVersion() const;

        // Both versions are saved with the graph, so an asset that was saved compiled opens compiled.
        NODISCARD bool NeedsCompile() const { return GetTreeContentVersion() != CompiledContentVersion; }
        void MarkCompiled() { CompiledContentVersion = GetTreeContentVersion(); }

        // Links with both endpoints in the set are rebuilt, the rest dropped. Returns each source node's copy.
        THashMap<CEdGraphNode*, CEdGraphNode*> CloneNodes(const TVector<CEdGraphNode*>& SourceNodes, ImVec2 Delta);

        // Deletable selected nodes plus the canvas-space center of their boxes.
        void CollectSelectedNodesForClone(TVector<CEdGraphNode*>& OutNodes, ImVec2& OutPivot);

        // Deep-copies Source's nodes, links and layout into this empty graph, with no editor context.
        void CloneContentFrom(const CEdNodeGraph* Source);

        // Hook for graph data keyed by node id, handed every source node mapped to its clone.
        virtual void PostCloneContent(const CEdNodeGraph* Source, const THashMap<CEdGraphNode*, CEdGraphNode*>& Clones) {}

        // Gives any sub-graph reached twice its own copy, repairing assets saved when clones aliased.
        uint32 UnaliasSubGraphs(THashSet<CEdNodeGraph*>& Visited);

        // No-ops below two selected nodes, three for the Distribute modes.
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

        // Lays the graph out in columns by distance from its roots, using the sizes the nodes last drew at.
        void TidyGraph();

        // Meaningless in a graph with no declared root, since nothing there can be measured as dead.
        bool bFadeDeadNodes = true;

        // Places Node under a screen position, using the view of the last draw.
        void PlaceNodeAtScreen(CEdGraphNode* Node, ImVec2 ScreenPos);

        // Selects Node and pans to it.
        void FocusNode(CEdGraphNode* Node);

        // Selects and frames these nodes on the next draw, or frames every node when the list is empty.
        void QueueFrameNodes(const TVector<int64>& NodeIDs);

        // The canvas rectangle on screen at the last draw and its screen pixels per canvas unit, false before the first draw.
        bool GetVisibleCanvasRect(ImVec2& OutMin, ImVec2& OutMax, float& OutZoom) const;

        // The pin carrying this GUID, or null. Linear, and only walked for the one pin under the cursor.
        CEdNodeGraphPin* FindPinByGUID(uint32 PinID) const;

        // Hover tooltips, drawn in screen space after the canvas.
        virtual void DrawPinTooltip(CEdNodeGraphPin* Pin);
        virtual void DrawLinkTooltip(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin) {}

        // Runs at the tail of DrawGraph with the host window current, the only place a canvas-wide drop target works.
        virtual void DrawCanvasDropTarget() {}

        // When non-null, double-clicking a wire inserts one of these and reroutes the wire through it.
        virtual CClass* GetRerouteNodeClass() const { return nullptr; }

        // Splits the link with a reroute node centered on CanvasPos, unless GetRerouteNodeClass returns null.
        CEdGraphNode* InsertRerouteOnLink(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin, ImVec2 CanvasPos);

        // Bundles these wires through CanvasPos, taking them out of any tie that already held them.
        void AddZipTie(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links, ImVec2 CanvasPos);

        // Drops tied wires that no longer exist, and ties left holding none.
        void PruneZipTies();

        
        // Every wire as its input and output pin.
        void CollectLinks(TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& OutLinks) const;

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

        // Draws Node in place of the default builder. Return true when handled; links are collected either way.
        virtual bool DrawCustomNode(CEdGraphNode* Node) { return false; }

        // A transparent wire is left for the graph to draw itself in DrawGraphOverlay.
        virtual void GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const;

        // The color an output's wires carry, which for a reroute is whatever feeds it.
        ImU32 GetWireColor(const CEdNodeGraphPin* OutputPin) const;

        // A short label riding the middle of a wire, such as an implicit conversion.
        virtual bool GetLinkBadge(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, FString& OutText, ImU32& OutColor) { return false; }

        // Dots run along a wire whose value changes from frame to frame.
        virtual bool WantsLinkFlow(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin) { return false; }

        // A 0 to 1 share of the graph's cost and its label, drawn along the node while a cost view is on.
        virtual bool GetNodeHeat(const CEdGraphNode* Node, float& OutHeat, FString& OutLabel) { return false; }

        // What the node compiled to, offered from its context menu.
        virtual bool GetNodeGeneratedCode(const CEdGraphNode* Node, FString& OutCode) { return false; }

        // Graph-specific entries in a node's context menu, under the node's own.
        virtual void DrawNodeMenuItems(CEdGraphNode* Node) {}

        // Drawn inside a standard node, under its pins.
        virtual void DrawNodePreview(CEdGraphNode* Node, FGraphNodeBuilder& Builder) {}

        // Marked with an eye at its corner, for a graph that shows one node's value somewhere else.
        virtual bool IsNodePreviewed(const CEdGraphNode* Node) { return false; }

        // Called before any node is submitted, so coordinates are canvas space and wires land under the nodes.
        virtual void DrawGraphOverlay(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links) {}

        // A wire is identified by its two endpoints, so ids survive deleting some other wire.
        static uint64 MakeLinkID(const CEdNodeGraphPin* InputPin, const CEdNodeGraphPin* OutputPin);

        // Package for newly constructed nodes, defaulting to this graph's own package.
        virtual CPackage* GetNodeOuter();

        //~ Edits. The canvas and scripted callers both come through here, so they share the rules and the undo.

        // Records this graph's edits on that tool's history, or none when null.
        void SetTransactionManager(FTransactionManager* InManager);
        NODISCARD FTransactionManager* GetTransactionManager() const { return TransactionManager; }

        // Records whatever changed since the last undo step, labeled Label, and no-ops when nothing did.
        void CommitEdit(FName Label);

        // Records what changed since the last step as part of that step, for a side effect such as a lazily created canvas.
        void CommitEditIntoLastStep();

        // Used by FNodeGraphEditScope, which is the way to bracket an edit.
        void EnterEditScope() { ++EditScopeDepth; }
        void LeaveEditScope(FName Label);

        // Holds back the end-of-frame commit while an edit spanning frames, such as a property drag, is open.
        void BeginLongEdit();
        void EndLongEdit(FName Label);

        // Adopts the current state as where the next undo step starts, without recording anything.
        void RebaseUndoImage();

        // The next commit's label, for a canvas action whose change is picked up at the end of the frame.
        void SetPendingEditLabel(FName Label) { PendingEditLabel = Label; }

        // Positions the node on the canvas even when called outside DrawGraph.
        CEdGraphNode* SpawnNode(CClass* NodeClass, float X, float Y);

        // Refuses the whole set when any node is one the graph keeps for itself.
        bool RemoveNodes(const TVector<CEdGraphNode*>& InNodes, FString* OutError = nullptr);

        // Applies the schema and the single-link input rule, replacing an input's existing link.
        bool ConnectPins(CEdNodeGraphPin* Output, CEdNodeGraphPin* Input, FString* OutError = nullptr);

        void BreakPinLinks(CEdNodeGraphPin* Pin);
        void BreakLink(CEdNodeGraphPin* PinA, CEdNodeGraphPin* PinB);

        void MoveNode(CEdGraphNode* Node, float X, float Y);

        // For a caller that wrote node properties directly, so the canvas adopts them and the edit becomes one undo step.
        void CommitExternalNodeEdit(FName Label);

        // Repairs what an undo image cannot carry, after it rewrote Nodes and Connections.
        void FixupAfterRestore(const TVector<CEdGraphNode*>& PreviousNodes);


        FGraphEditor& GetEditor() { return Editor; }

        // Opens a node on the canvas at its grid position, for a graph drawing its own node body.
        void BeginCanvasNode(CEdGraphNode* Node, const FGraphNodeStyle& Style);

        // Canvas-space box of the node as last drawn.
        ImRect GetNodeBounds(const CEdGraphNode* Node) const;

    private:

        // F2 on a single selected renamable node.
        void HandleRenameShortcut();

        // Guarded against the aliased sub-graphs UnaliasSubGraphs repairs, which would otherwise recurse forever.
        uint64 AccumulateTreeContentVersion(THashSet<const CEdNodeGraph*>& Visited) const;

        // Compact reroute renderer (single dot, no header).
        void DrawRerouteNode(CEdGraphNode* Node);

        // Drawn behind the graph, and its title bar drags what it contains.
        void DrawCommentNode(CEdNode_Comment* Comment);

        void DrawStandardNode(CEdGraphNode* Node);

        // Restates each comment's title in screen space once the canvas is zoomed out too far to read it.
        void DrawCommentHints();

        // Spells out a failing node's error right under it, so it is read in the graph rather than found in a log.
        void DrawNodeErrorBubble(const CEdGraphNode* Node);

        // C wraps the selection in a comment.
        void HandleCommentShortcut();

        // Acts on what the canvas reported from the frame just drawn.
        void HandleCanvasRequests(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links);
        void HandleLinkDrag();
        void HandleStroke(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links);
        void DeleteSelection(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links);
        void PasteClipboard(ImVec2 CanvasPos);
        void DrawCanvasPopups(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links);
        void SubmitZipTies();

        // Q straightens, Ctrl+F finds, and Ctrl or Shift with a digit sets or recalls a bookmark.
        void HandleGraphHotkeys();
        void StraightenSelection();
        void SetBookmark(int32 Slot);
        void RecallBookmark(int32 Slot);
        void DrawFindPopup();
        void DrawGeneratedCodePopup();
        void DrawNodeHeat(CEdGraphNode* Node, float Heat, const FString& Label);

        // A lone node dropped on a wire takes it in through FindAutoConnectPin on both ends, and only into a free input.
        void HandleSplice(const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links);
        bool FindSplicePins(const FGraphSplice& Splice, const TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>>& Links,
                            CEdNodeGraphPin*& OutInput, CEdNodeGraphPin*& OutOutput) const;

        EGraphPinHighlight GetPinDragHighlight(CEdNodeGraphPin* Pin) const;
        FGraphPinIcon MakePinIcon(CEdNodeGraphPin* Pin, ImU32 Color, bool bDisabled) const;


        // Adopts a position the canvas wrote back while the node was dragged.
        void SyncNodePosition(CEdGraphNode* Node, ImVec2 Position);

        // Opens the rename box for Node on the next frame, when it has a renamable name.
        void BeginRenameNode(CEdGraphNode* Node);

        // Drawn after the canvas, since a popup opened inside a node would land in the zoomed canvas.
        void DrawChoicePopup();

        // Draws a pin's live debug value inline in the pin row when the debug context supplies one.
        void DrawPinDebugValue(CEdNodeGraphPin* Pin);

        // Framing waits for the next draw, which lays out any node created since.
        bool           bHasPendingFrame     = false;
        TVector<int64> PendingFrameNodes;

        // Rewires every pin from the serialized Connections, the one place links are persisted.
        void RelinkFromConnections();

        // Commits a change the canvas made this frame once no drag or widget edit is still in flight.
        void CommitFrameEdits();

        FTransactionManager*    TransactionManager = nullptr;
        FNodeGraphImage         UndoImage;
        uint64                  UndoImageContentVersion = 0;
        int32                   EditScopeDepth = 0;
        int32                   LongEditDepth = 0;
        FName                   PendingEditLabel;
        bool                    bLayoutChangedSinceImage = false;

        // What each context menu was opened on, since a popup outlives the frame that spawned it.
        int64          ContextMenuNodeID = 0;
        uint32         ContextMenuPinID  = 0;
        uint64         ContextMenuLinkID = 0;
        uint32         ContextMenuTieID  = 0;

        // Set while PostLoad rebuilds, so AddNode does not reconcile against links that are not back yet.
        bool           bIsPostLoading = false;

        // Set when C goes down over a selection and cleared by a click, so C plus a click stays a quick-place.
        bool           bCommentKeyArmed = false;

        // F2 rename box, opened a frame after the key since the popup lives in the suspended region.
        bool           bOpenRenamePopup = false;
        int64          RenameNodeID     = 0;
        char           RenameBuffer[128] = {};

        // A node's dropdown, held here because the node only requests it while the canvas is drawing.
        bool                     bOpenChoicePopup = false;
        int64                    ChoiceNodeID     = 0;
        int32                    ChoiceCurrent    = 0;
        ImVec2                   ChoiceAnchor;
        float                    ChoiceWidth      = 0.0f;
        TVector<FString>         Choices;
        TFunction<void(int32)>   OnChoiceChosen;
        
    public:

        // Palette classes, built on first use from reflection so nothing is listed here. See FGraphNodeRegistry.
        const THashSet<CClass*>& GetSupportedNodes();

        // Classes with a pin that takes a wire from SourcePin, found by building one throwaway node of each.
        void CollectClassesAccepting(CEdNodeGraphPin* SourcePin, THashSet<CClass*>& OutClasses);

        // Per-instance escape hatch for a node discovery cannot reach. Prefer GetSupportedGraphClass.
        void RegisterGraphNode(CClass* InClass);

        uint64 AddNode(CEdGraphNode* InNode);

        // A free node ID, preferring PreferredID. IDs must be unique within a graph and never zero.
        int64 GenerateUniqueNodeID(int64 PreferredID) const;

        // All nodes currently in this graph.
        PROPERTY(Hidden)
        TVector<TStrongObjectPtr<CEdGraphNode>>         Nodes;

        // Pin-to-pin links, stored as pairs of 32-bit pin ids.
        PROPERTY(Hidden)
        TVector<uint32>                                 Connections;

        PROPERTY(Hidden)
        TVector<SGraphZipTie>                           ZipTies;

        PROPERTY(Hidden)
        TVector<SGraphBookmark>                         Bookmarks;

        // Bumped by NotifyContentChanged. See GetContentVersion.
        PROPERTY(Hidden)
        uint64 ContentVersion = 0;

        // ContentVersion at the last compile. A legacy graph loads 0 for both and so reads as compiled.
        PROPERTY(Hidden)
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

        // The pin a wire is being pulled from this frame, which every other pin answers in its icon.
        CEdNodeGraphPin*                                DragSourcePin = nullptr;
        bool                                            bDragMovesLinks = false;

        bool                                            bOpenFind = false;
        char                                            FindQuery[128] = {};
        int32                                           FindSelection = 0;
        int64                                           CodeViewNodeID = 0;
        bool                                            bOpenCodeView = false;
        bool                                            bOpenCreateFromPin = false;

        bool                                            bFirstDraw = true;
        bool                                            bNeedsInitialFraming = true;
        bool                                            bDebug = false;
        
        // Lists Choices under the item just drawn in Node, and OnChosen gets the picked index if Node still exists.
        void OpenChoicePopup(CEdGraphNode* Node, TVector<FString> Choices, int32 Current, TFunction<void(int32)> OnChosen);

    private:

        FGraphEditor Editor;

        // Set each frame by an asset editor's debug overlay; default is "off".
        FGraphDebugContext DebugContext;
    };
    
    

    // Brackets a graph edit as one undo step that commits when the outermost scope closes.
    class FNodeGraphEditScope
    {
    public:

        FNodeGraphEditScope(CEdNodeGraph* InGraph, FName InLabel) : Graph(InGraph), Label(InLabel) { if (Graph) { Graph->EnterEditScope(); } }
        ~FNodeGraphEditScope() { if (Graph) { Graph->LeaveEditScope(Label); } }

        FNodeGraphEditScope(const FNodeGraphEditScope&) = delete;
        FNodeGraphEditScope& operator=(const FNodeGraphEditScope&) = delete;
        FNodeGraphEditScope(FNodeGraphEditScope&&) = delete;
        FNodeGraphEditScope& operator=(FNodeGraphEditScope&&) = delete;

    private:

        CEdNodeGraph* Graph;
        FName         Label;
    };
}
