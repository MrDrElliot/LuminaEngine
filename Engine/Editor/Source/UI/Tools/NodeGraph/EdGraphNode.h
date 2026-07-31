#pragma once

#include "EdNodeGraphPin.h"
#include <imgui.h>
#include "Containers/Array.h"
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
    class CEdGraphNode : public CObject
    {
        GENERATED_BODY()
        
    public:
        
        friend class CEdNodeGraph;

        void PostCreateCDO() override;
        
        virtual void BuildNode() { }
        
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

        // True for wire-passthrough nodes (e.g. CEdNode_Reroute): drawn as a single dot; graph walks
        // skip them and resolve through to the real source/target.
        virtual bool IsRerouteNode() const { return false; }

        void SetDebugExecutionOrder(uint32 Order) { DebugExecutionOrder = Order; }
        uint32 GetDebugExecutionOrder() const { return DebugExecutionOrder; }

        virtual void PushNodeStyle();
        virtual void PopNodeStyle();

        virtual void DrawContextMenu() { }
        virtual void DrawNodeTitleBar();

        void SetError(const EdNodeGraph::FError& InError) { Error = InError; }
        const EdNodeGraph::FError& GetError() const { return Error.value(); }
        bool HasError() const { return Error.has_value(); }
        void ClearError() { Error = eastl::nullopt; }
        
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
