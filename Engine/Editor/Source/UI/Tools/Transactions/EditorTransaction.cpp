#include "EditorTransaction.h"

namespace Lumina
{
    void FTransactionManager::CommitTransaction()
    {
        if (!bOpen)
        {
            return;
        }
        bOpen = false;

        // Snapshot each command's after-image, then drop the ones that changed nothing.
        TVector<TUniquePtr<IUndoableCommand>> Kept;
        Kept.Reserve(Open.Commands.Num());
        for (TUniquePtr<IUndoableCommand>& Command : Open.Commands)
        {
            Command->Finalize();
            if (!Command->IsNoOp())
            {
                Kept.Add(Move(Command));
            }
        }
        Open.Commands = Move(Kept);

        if (!Open.Commands.IsEmpty())
        {
            PushCommitted(Move(Open));
        }
        Open = FTransaction{};
    }

    void FTransactionManager::AppendToLast(TUniquePtr<IUndoableCommand> Command)
    {
        if (Command == nullptr || bApplying)
        {
            return;
        }

        if (bOpen)
        {
            Open.Commands.Add(Move(Command));
            return;
        }

        Command->Finalize();
        if (Command->IsNoOp())
        {
            return;
        }

        if (UndoStack.IsEmpty())
        {
            FTransaction Transaction;
            Transaction.Name = Command->GetName();
            Transaction.Commands.Add(Move(Command));
            PushCommitted(Move(Transaction));
            return;
        }

        UndoStack.Last().Commands.Add(Move(Command));
        RedoStack.Reset();

        if (OnCommitted)
        {
            OnCommitted();
        }
    }

    void FTransactionManager::AbortTransaction()
    {
        bOpen = false;
        Open = FTransaction{};
    }

    void FTransactionManager::PushCommitted(FTransaction&& Transaction)
    {
        if (UndoStack.Num() >= MaxHistory)
        {
            UndoStack.RemoveAt(0);
        }
        UndoStack.Add(Move(Transaction));
        RedoStack.Reset();

        if (OnCommitted)
        {
            OnCommitted();
        }
    }

    void FTransactionManager::Undo()
    {
        if (UndoStack.IsEmpty())
        {
            return;
        }

        FTransaction Transaction = Move(UndoStack.Last());
        UndoStack.Pop();

        bApplying = true;
        Transaction.Undo();
        bApplying = false;

        RedoStack.Add(Move(Transaction));

        if (OnPostApply)
        {
            OnPostApply();
        }
    }

    void FTransactionManager::Redo()
    {
        if (RedoStack.IsEmpty())
        {
            return;
        }

        FTransaction Transaction = Move(RedoStack.Last());
        RedoStack.Pop();

        bApplying = true;
        Transaction.Redo();
        bApplying = false;

        UndoStack.Add(Move(Transaction));

        if (OnPostApply)
        {
            OnPostApply();
        }
    }

    void FTransactionManager::Clear()
    {
        UndoStack.Reset();
        RedoStack.Reset();
        Open = FTransaction{};
        bOpen = false;
    }
}

namespace Lumina
{
    void FTransactionManager::VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func)
    {
        for (FTransaction& Transaction : UndoStack)
        {
            Transaction.VisitObjectReferences(Func);
        }
        for (FTransaction& Transaction : RedoStack)
        {
            Transaction.VisitObjectReferences(Func);
        }
        Open.VisitObjectReferences(Func);
    }
}
