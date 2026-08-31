#pragma once

#include "EdNodeGraphPin.h"
#include <imgui.h>
#include "Containers/StaticArray.h"
#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EdGraphNode.generated.h"

namespace Lumina
{
    enum class EMaterialInputType : uint8;
    class CEdNodeGraph;
    class CEdNodeGraphPin;
    
    enum class ENodePinDirection : uint8
    {
        Input       = 0,
        Output      = 1,

        Count       = 2,
    };
    
    namespace EdNodeGraph
    {
        struct FError
        {
            FString             Name;
            FString             Description;
            CEdGraphNode*       Node = nullptr;
        };
    }

    REFLECT()
    class EDITOR_API CEdGraphNode : public CObject
    {
        GENERATED_BODY()
        
    public:
        
        friend class CEdNodeGraph;

        void PostCreateCDO() override;
        
        virtual void BuildNode() { }

        /**
         * The graph class this node belongs to, or null for "no graph".
         *
         * This is how a node is FOUND: FGraphNodeRegistry walks every reflected CEdGraphNode subclass and
         * asks its CDO, so there is no registration list to add to. Declare it once on a family base --
         * CMaterialGraphNode answers CMaterialNodeGraph -- and every node in that family inherits the
         * answer, including ones a game or plugin module declares, which an engine-side list could never
         * have named. Override it lower down to narrow the answer (FunctionInput -> function graphs only).
         *
         * A node is offered in that graph class and in any graph deriving from it, so a material function
         * graph gets the whole material node library for free.
         *
         * Null is the base's answer rather than "every graph" on purpose: a node opts IN by answering, so
         * a CEdGraphNode subclass that is not part of any palette needs no marker.
         */
        virtual CClass* GetSupportedGraphClass() const { return nullptr; }

        /**
         * Whether this node should be offered in a graph of GraphClass. Defaults to the class test above.
         *
         * Must depend only on GraphClass, never on a particular graph instance or its asset: results are
         * cached per graph class. A rule that needs the asset (a node valid only on terrain materials)
         * belongs in the node's compile step as an error, not here.
         */
        virtual bool IsSupportedInGraph(CClass* GraphClass) const;

        virtual FFixedString GetNodeCategory() const { return "General"; }
        
        FString GetNodeFullName() { return FullName; }

        // FullName drives the emitted variable name. The material-function inliner temporarily prefixes
        // a function's interior node names so nested calls don't collide, then restores them. Not general-use.
        void SetNodeFullName(const FString& In) { FullName = In; }

        virtual bool WantsTitlebar() const { return true; }
        virtual FStringView GetNodeDisplayName() const { return "Node"; }
        virtual FStringView GetNodeTooltip() const { return "No Tooltip"; }
        virtual uint32 GetNodeTitleColor() const { return IM_COL32(200, 35, 35, 255); }
        virtual ImVec2 GetMinNodeBodySize() const { return ImVec2(80, 150); }
        virtual ImVec2 GetMinNodeTitleBarSize() const;

        virtual void DrawNodeBody() { }

        virtual bool IsDeletable() const { return true; }

        // A sub-graph this node contains, descended into on double-click (e.g. a state machine canvas).
        // Null for ordinary leaf nodes. May lazily allocate the graph.
        virtual CEdNodeGraph* GetEnterableSubGraph() { return nullptr; }

        // True for wire-passthrough nodes (e.g. CEdNode_Reroute): graph walks skip them and resolve
        // through to the real source/target, and the compiler emits nothing for them.
        virtual bool IsRerouteNode() const { return false; }

        // Whether a passthrough node collapses to a single dot. Split from IsRerouteNode because a
        // named reroute passes through like one but still needs a titled body to show its name.
        virtual bool WantsRerouteDotRendering() const { return IsRerouteNode(); }

        // False for a static switch, whose branch pins are typed inputs the schema still has to police.
        virtual bool IsUntypedPassthrough() const { return IsRerouteNode(); }

        // The pin feeding a passthrough node. Defaults to the first input; a named reroute usage
        // returns its declaration's input instead, which is what lets a wireless link resolve through
        // every walk that already understands reroutes.
        virtual CEdNodeGraphPin* GetRerouteSourcePin() const;

        // A node this one depends on with no wire between them. Reachability and topological order
        // fold it in as a real input edge, so a named reroute's upstream still compiles, and still
        // compiles first.
        virtual CEdGraphNode* GetImplicitInputNode() const { return nullptr; }

        // Title-bar text. Separate from GetNodeDisplayName, which pin ids hash and a rename must not move.
        virtual FString GetNodeTitleText() const;

        // F2 rename: fills OutText and returns true for a node with an editable name. Default is no.
        virtual bool GetRenameText(FString& OutText) const { return false; }

        // Commits an F2 rename. Only reached when GetRenameText returned true.
        virtual void SetRenameText(const FString& InText) {}

        // A clone copies object references as references, so a node owning a sub-graph re-copies it here.
        virtual void PostCloneFrom(const CEdGraphNode* Source) {}

        // The sub-graph this node owns, or null. Unlike GetEnterableSubGraph this never creates one.
        virtual CEdNodeGraph* GetOwnedSubGraph() const { return nullptr; }

        // Swaps the owned sub-graph for a private deep copy, repairing one shared with another node.
        virtual void ReplaceSubGraphWithCopy() {}

        void SetDebugExecutionOrder(uint32 Order) { DebugExecutionOrder = Order; }
        uint32 GetDebugExecutionOrder() const { return DebugExecutionOrder; }

        virtual void PushNodeStyle();
        virtual void PopNodeStyle();

        virtual void DrawContextMenu() { }
        virtual void DrawNodeTitleBar();

        void SetError(const EdNodeGraph::FError& InError) { Error = InError; }
        const EdNodeGraph::FError& GetError() const { return Error.value(); }
        bool HasError() const { return Error.has_value(); }
        void ClearError() { Error = NullOpt; }
        
        CEdNodeGraphPin* GetPin(uint32 ID, ENodePinDirection Direction);
        CEdNodeGraphPin* GetPinByIndex(uint32 Index, ENodePinDirection Direction);
        
        int64 GetNodeID() const { return NodeID; }

        void SetGridPos(float X, float Y) { GridX = X; GridY = Y; }
        float GetNodeX() const { return GridX; }
        float GetNodeY() const { return GridY; }

        const TVector<TObjectPtr<CEdNodeGraphPin>>& GetInputPins() const { return NodePins[static_cast<uint32>(ENodePinDirection::Input)]; }
        const TVector<TObjectPtr<CEdNodeGraphPin>>& GetOutputPins() const { return NodePins[static_cast<uint32>(ENodePinDirection::Output)]; }

        CEdNodeGraphPin* CreatePin(CClass* InClass, const FString& Name, ENodePinDirection Direction);

        // True when a pin on this node already uses this id, in either direction.
        bool IsPinIDTaken(uint32 ID) const;

        // Owning graph; populated by CEdNodeGraph::AddNode. Lets nodes reach up for graph-wide
        // context (e.g. material domain on CMaterialOutputNode).
        CEdNodeGraph* GetOwningGraph() const { return OwningGraph; }

        // Call when an inline editor on this node commits a value. Those widgets (a constant's
        // DragFloat, a pin's ColorEdit, a texture slot) write straight into the node and never pass
        // through the property table, so without this nothing marks the graph as changed and a tool
        // watching GetContentVersion never learns it needs to recompile.
        void NotifyValueEdited();

        /** Horizontal position of the node in the graph canvas. */
        PROPERTY(DuplicateTransient)
        float GridX;

        /** Vertical position of the node in the graph canvas. */
        PROPERTY(DuplicateTransient)
        float GridY;

        /** Unique identifier for this node within the graph. */
        PROPERTY(DuplicateTransient)
        int64 NodeID = 0;
        
        
    protected:

        TArray<TVector<TObjectPtr<CEdNodeGraphPin>>, static_cast<uint32>(ENodePinDirection::Count)> NodePins;

        uint32 DebugExecutionOrder;


        FString                             FullName;

        // Raw "DisplayName_NodeID". Pin IDs hash this rather than FullName, so sanitizing the emitted
        // identifier never shifts pin IDs and drops the serialized links of existing graphs.
        FString                             PinHashName;

        TOptional<EdNodeGraph::FError>      Error;
        bool                                bWasBuild = false;

        // Set by CEdNodeGraph::AddNode. Non-owning -- the graph owns the
        // node, not the other way around.
        CEdNodeGraph*                       OwningGraph = nullptr;
    };
    
}
