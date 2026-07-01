#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Core/Templates/LuminaTemplate.h"   // Move / Forward
#include "Memory/SmartPtr.h"

namespace Lumina
{
    // One reversible edit, captured already-applied: the site mutates first, then records how to Undo/Redo it.
    class IUndoableCommand
    {
    public:

        virtual ~IUndoableCommand() = default;

        virtual void Undo() = 0;
        virtual void Redo() = 0;

        // Called once when the open transaction commits; diff-style commands capture their 'after' image here.
        virtual void Finalize() {}

        // A command whose before-image equals its after-image is dropped at commit (the drag-that-didn't-move).
        virtual bool IsNoOp() const { return false; }

        virtual FName GetName() const { return FName(); }
    };

    // Escape hatch for bespoke domains (terrain, node-graph); closures must capture stable ids, not raw handles.
    class FCustomCommand final : public IUndoableCommand
    {
    public:

        FCustomCommand(FName InName, TFunction<void()> InUndo, TFunction<void()> InRedo)
            : Name(InName), UndoFn(Move(InUndo)), RedoFn(Move(InRedo)) {}

        void Undo() override { if (UndoFn) { UndoFn(); } }
        void Redo() override { if (RedoFn) { RedoFn(); } }
        FName GetName() const override { return Name; }

    private:

        FName             Name;
        TFunction<void()> UndoFn;
        TFunction<void()> RedoFn;
    };

    // A named group of commands applied/reverted as one undo step (Undo runs them in reverse).
    struct FTransaction
    {
        FName                                 Name;
        TVector<TUniquePtr<IUndoableCommand>> Commands;

        bool IsEmpty() const { return Commands.empty(); }
        void Undo() { for (auto It = Commands.rbegin(); It != Commands.rend(); ++It) { (*It)->Undo(); } }
        void Redo() { for (TUniquePtr<IUndoableCommand>& C : Commands) { C->Redo(); } }
    };

    // Domain-blind undo/redo owner on FEditorTool; domains contribute IUndoableCommands.
    class FTransactionManager
    {
    public:

        static constexpr int32 MaxHistory = 64;

        // Open a builder that accumulates commands until CommitTransaction.
        void BeginTransaction(FName Name)
        {
            // Discard any stale open transaction (a property edit whose Finish was lost) so it can't strand recording.
            Open = FTransaction{};
            Open.Name = Name;
            bOpen = true;
        }

        void Record(TUniquePtr<IUndoableCommand> Command)
        {
            if (bOpen && Command != nullptr)
            {
                Open.Commands.push_back(Move(Command));
            }
        }

        // Name the open transaction; for callers that only know the label at commit time, e.g. EndTransaction(Name).
        void SetOpenTransactionName(FName Name)
        {
            if (bOpen)
            {
                Open.Name = Name;
            }
        }

        bool IsRecording() const { return bOpen; }

        void CommitTransaction();
        void AbortTransaction();

        void Undo();
        void Redo();

        bool  CanUndo() const { return !UndoStack.empty(); }
        bool  CanRedo() const { return !RedoStack.empty(); }
        FName PeekUndoName() const { return UndoStack.empty() ? FName() : UndoStack.back().Name; }
        FName PeekRedoName() const { return RedoStack.empty() ? FName() : RedoStack.back().Name; }

        void Clear();

        // Set by the owning tool to rebuild caches after any Undo/Redo (selection resync, outliner, etc.).
        TFunction<void()> OnPostApply;

    private:

        void PushCommitted(FTransaction&& Transaction);

        TVector<FTransaction> UndoStack;
        TVector<FTransaction> RedoStack;
        FTransaction          Open;
        bool                  bOpen = false;
    };
}
