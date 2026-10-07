#pragma once

#include "Containers/Vector.h"
#include "Core/Reflection/Type/ObjectReferenceVisitor.h"
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

        /** Objects this command will act on when it is undone or redone, so they can be repointed. */
        virtual void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) {}
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

        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
        {
            for (TUniquePtr<IUndoableCommand>& Command : Commands)
            {
                if (Command)
                {
                    Command->VisitObjectReferences(Func);
                }
            }
        }
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
            // A restore that triggers an edit hook would otherwise record a step and wipe the redo history mid-undo.
            if (bApplying)
            {
                return;
            }

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

        // True while an undo or redo is being applied, when nothing new may be recorded.
        bool IsApplying() const { return bApplying; }

        void CommitTransaction();
        void AbortTransaction();

        // Folds a side effect into the newest step, so undoing that step takes the side effect with it.
        void AppendToLast(TUniquePtr<IUndoableCommand> Command);

        void Undo();
        void Redo();

        bool  CanUndo() const { return !UndoStack.empty(); }
        bool  CanRedo() const { return !RedoStack.empty(); }
        FName PeekUndoName() const { return UndoStack.empty() ? FName() : UndoStack.back().Name; }
        FName PeekRedoName() const { return RedoStack.empty() ? FName() : RedoStack.back().Name; }

        void Clear();

        // A stacked command outlives the edit that made it, so a reinstanced object has to reach it too or
        // the next undo writes into an object that is no longer the one on screen.
        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func);

        // Set by the owning tool to rebuild caches after any Undo/Redo (selection resync, outliner, etc.).
        TFunction<void()> OnPostApply;

        // Fired whenever a step lands on the stack, so a tool watching for untracked edits can move its baseline.
        TFunction<void()> OnCommitted;

    private:

        void PushCommitted(FTransaction&& Transaction);

        TVector<FTransaction> UndoStack;
        TVector<FTransaction> RedoStack;
        FTransaction          Open;
        bool                  bOpen = false;
        bool                  bApplying = false;
    };
}
