#pragma once

#include "EditorTransaction.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"

namespace Lumina
{
    class CEdNodeGraph;
    class CEdGraphNode;

    // A node graph's own properties followed by each node's, in node order.
    struct FNodeGraphImage
    {
        TVector<uint8> Bytes;

        // Strong, so a deleted node outlives its removal and an undo brings back the same object.
        TVector<TStrongObjectPtr<CEdGraphNode>> Nodes;

        // Everything the captured properties point at, such as sub-graphs and transitions, kept alive the same way.
        TVector<TStrongObjectPtr<CObject>> Referenced;

        bool IsValid() const { return !Bytes.empty(); }

        static FNodeGraphImage Capture(CEdNodeGraph* Graph);

        // Rewrites Graph in place, so node objects, their sub-graphs and anything pointing at them survive.
        void Restore(CEdNodeGraph* Graph) const;

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func);
    };

    class FNodeGraphSnapshotCommand final : public IUndoableCommand
    {
    public:

        FNodeGraphSnapshotCommand(CEdNodeGraph* InGraph, FNodeGraphImage InBefore, FNodeGraphImage InAfter);

        void Undo() override;
        void Redo() override;
        bool IsNoOp() const override { return Before.Bytes == After.Bytes; }

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;

    private:

        void Apply(const FNodeGraphImage& Image);

        // Weak, so a stale undo for a graph that was deleted no-ops instead of reviving it.
        TWeakObjectPtr<CEdNodeGraph> Graph;
        FNodeGraphImage              Before;
        FNodeGraphImage              After;
    };
}
