#include "NodeGraphSnapshotCommand.h"

#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"

namespace Lumina
{
    FNodeGraphImage FNodeGraphImage::Capture(CEdNodeGraph* Graph)
    {
        LUMINA_PROFILE_SCOPE();

        FNodeGraphImage Image;
        if (Graph == nullptr)
        {
            return Image;
        }

        FMemoryWriter Writer(Image.Bytes);
        FObjectProxyArchiver Ar(Writer, false);
        Graph->GetClass()->SerializeTaggedProperties(Ar, Graph);

        auto Hold = [&Image](CObject* Object) -> CObject*
        {
            if (Object != nullptr)
            {
                Image.Referenced.push_back(Object);
            }
            return Object;
        };
        FObjectReferenceVisitor::VisitStruct(Graph->GetClass(), Graph, Hold);

        Image.Nodes.reserve(Graph->Nodes.size());
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
        {
            Image.Nodes.push_back(Node);
            if (Node.IsValid())
            {
                Node->GetClass()->SerializeTaggedProperties(Ar, Node.Get());
                FObjectReferenceVisitor::VisitStruct(Node->GetClass(), Node.Get(), Hold);
            }
        }

        return Image;
    }

    void FNodeGraphImage::Restore(CEdNodeGraph* Graph) const
    {
        LUMINA_PROFILE_SCOPE();

        if (Graph == nullptr || Bytes.empty())
        {
            return;
        }

        TVector<CEdGraphNode*> Previous;
        Previous.reserve(Graph->Nodes.size());
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
        {
            Previous.push_back(Node.Get());
        }

        // The compile state and the canvas layout string describe now, not the moment of the capture.
        const uint64  ContentVersion         = Graph->ContentVersion;
        const uint64  CompiledContentVersion = Graph->CompiledContentVersion;
        const FString GraphSaveData          = Graph->GraphSaveData;

        FMemoryReader Reader(Bytes);
        FObjectProxyArchiver Ar(Reader, true);
        Graph->GetClass()->SerializeTaggedProperties(Ar, Graph);

        // The graph's node list was just read back, and is the order the node properties were written in.
        for (const TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid())
            {
                Node->GetClass()->SerializeTaggedProperties(Ar, Node.Get());
            }
        }

        Graph->ContentVersion         = ContentVersion;
        Graph->CompiledContentVersion = CompiledContentVersion;
        Graph->GraphSaveData          = GraphSaveData;

        Graph->FixupAfterRestore(Previous);

        if (CPackage* Package = Graph->GetPackage())
        {
            Package->MarkDirty();
        }
    }

    void FNodeGraphImage::VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        for (TStrongObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            Node = static_cast<CEdGraphNode*>(Func(Node.Get()));
        }
        for (TStrongObjectPtr<CObject>& Object : Referenced)
        {
            Object = Func(Object.Get());
        }
    }

    FNodeGraphSnapshotCommand::FNodeGraphSnapshotCommand(CEdNodeGraph* InGraph, FNodeGraphImage InBefore, FNodeGraphImage InAfter)
        : Graph(InGraph)
        , Before(Move(InBefore))
        , After(Move(InAfter))
    {
    }

    void FNodeGraphSnapshotCommand::Apply(const FNodeGraphImage& Image)
    {
        if (CEdNodeGraph* Target = Graph.Get())
        {
            Image.Restore(Target);
            Target->RebaseUndoImage();
        }
    }

    void FNodeGraphSnapshotCommand::Undo() { Apply(Before); }
    void FNodeGraphSnapshotCommand::Redo() { Apply(After); }

    void FNodeGraphSnapshotCommand::VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        Before.VisitObjectReferences(Func);
        After.VisitObjectReferences(Func);
    }
}
