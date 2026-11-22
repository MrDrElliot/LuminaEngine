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

    // One palette entry of a node class that offers several, such as a formula node per formula.
    struct FGraphNodeVariant
    {
        FName   Name;
        FString DisplayName;
        FString Category;
        FString Tooltip;
    };

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

        // A class that lists variants is offered once per variant rather than once as itself.
        virtual void GetVariants(TVector<FGraphNodeVariant>& OutVariants) const {}

        // Picks the variant before the node builds its pins, and must be saved by the node to survive a reload.
        virtual void ApplyVariant(FName Variant) {}
        
        FString GetNodeFullName() { return FullName; }

        // FullName drives the emitted variable name, and only the material-function inliner should change it.
        void SetNodeFullName(const FString& In) { FullName = In; }

        virtual bool WantsTitlebar() const { return true; }
        virtual FStringView GetNodeDisplayName() const { return "Node"; }
        virtual FStringView GetNodeTooltip() const { return "No Tooltip"; }
        // Neutral, because a red default would make every unstyled node look like it failed to compile.
        virtual uint32 GetNodeTitleColor() const { return IM_COL32(72, 80, 96, 255); }
        virtual ImVec2 GetMinNodeBodySize() const { return ImVec2(80, 150); }
        virtual ImVec2 GetMinNodeTitleBarSize() const;

        virtual void DrawNodeBody() { }

        virtual bool IsDeletable() const { return true; }

        // A sub-graph descended into on double-click, which may be allocated lazily. Null for leaf nodes.
        virtual CEdNodeGraph* GetEnterableSubGraph() { return nullptr; }

        // A comment box annotates the canvas and compiles to nothing, so graph tools leave it out of their passes.
        virtual bool IsCommentNode() const { return false; }

        // True for wire-passthrough nodes, which graph walks resolve through and the compiler skips.
        virtual bool IsRerouteNode() const { return false; }

        // Split from IsRerouteNode because a named reroute passes through but still needs a titled body.
        virtual bool WantsRerouteDotRendering() const { return IsRerouteNode(); }

        // False for a static switch, whose branch pins are typed inputs the schema still has to police.
        virtual bool IsUntypedPassthrough() const { return IsRerouteNode(); }

        // Defaults to the first input, while a named reroute usage returns its declaration's input so wireless links resolve.
        virtual CEdNodeGraphPin* GetRerouteSourcePin() const;

        // A dependency with no wire, folded into reachability and topological order as a real input edge.
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

        virtual void DrawContextMenu() { }
        virtual void DrawNodeTitleBar();

        void SetError(const EdNodeGraph::FError& InError) { Error = InError; }
        const EdNodeGraph::FError& GetError() const { return Error.GetValue(); }
        bool HasError() const { return Error.IsSet(); }
        void ClearError() { Error = NullOpt; }

        void SetWarning(const EdNodeGraph::FError& InWarning) { Warning = InWarning; }
        const EdNodeGraph::FError& GetWarning() const { return Warning.GetValue(); }
        bool HasWarning() const { return Warning.IsSet(); }
        void ClearWarning() { Warning = NullOpt; }
        
        CEdNodeGraphPin* GetPin(uint32 ID, ENodePinDirection Direction);
        CEdNodeGraphPin* GetPinByIndex(uint32 Index, ENodePinDirection Direction);
        
        int64 GetNodeID() const { return NodeID; }

        void SetGridPos(float X, float Y) { GridX = X; GridY = Y; }
        float GetNodeX() const { return GridX; }
        float GetNodeY() const { return GridY; }

        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& GetInputPins() const { return NodePins[static_cast<uint32>(ENodePinDirection::Input)]; }
        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& GetOutputPins() const { return NodePins[static_cast<uint32>(ENodePinDirection::Output)]; }

        CEdNodeGraphPin* CreatePin(CClass* InClass, const FString& Name, ENodePinDirection Direction);

        // True when a pin on this node already uses this id, in either direction.
        bool IsPinIDTaken(uint32 ID) const;

        // Populated by CEdNodeGraph::AddNode so nodes can reach graph-wide context.
        CEdNodeGraph* GetOwningGraph() const { return OwningGraph; }

        // Call when an inline widget commits a value, since those bypass the property table and nothing else bumps GetContentVersion.
        void NotifyValueEdited();

        // A node that creates its sub-graph lazily calls this, so the new canvas rides along with the newest undo step.
        void NotifySubGraphCreated();

        /** Horizontal position of the node in the graph canvas. */
        PROPERTY(Hidden, DuplicateTransient)
        float GridX;

        /** Vertical position of the node in the graph canvas. */
        PROPERTY(Hidden, DuplicateTransient)
        float GridY;

        /** Unique identifier for this node within the graph. */
        PROPERTY(Hidden, DuplicateTransient)
        int64 NodeID = 0;

        // Whether the preview under the node's pins is open, for a graph whose nodes have one.
        PROPERTY(Hidden)
        bool bShowPreview = false;
        
        
    protected:

        TArray<TVector<TStrongObjectPtr<CEdNodeGraphPin>>, static_cast<uint32>(ENodePinDirection::Count)> NodePins;

        uint32 DebugExecutionOrder;


        FString                             FullName;

        // Pin IDs hash this rather than FullName so sanitizing the emitted identifier never drops serialized links.
        FString                             PinHashName;

        TOptional<EdNodeGraph::FError>      Error;
        TOptional<EdNodeGraph::FError>      Warning;
        bool                                bWasBuild = false;

        // Set by CEdNodeGraph::AddNode and non-owning.
        CEdNodeGraph*                       OwningGraph = nullptr;
    };
    
}
