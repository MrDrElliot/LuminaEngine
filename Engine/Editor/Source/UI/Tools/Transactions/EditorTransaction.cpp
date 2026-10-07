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
        Kept.reserve(Open.Commands.size());
        for (TUniquePtr<IUndoableCommand>& Command : Open.Commands)
        {
            Command->Finalize();
            if (!Command->IsNoOp())
            {
                Kept.push_back(Move(Command));
            }
        }
        Open.Commands = Move(Kept);

        if (!Open.Commands.empty())
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
            Open.Commands.push_back(Move(Command));
            return;
        }

        Command->Finalize();
        if (Command->IsNoOp())
        {
            return;
        }

        if (UndoStack.empty())
        {
            FTransaction Transaction;
            Transaction.Name = Command->GetName();
            Transaction.Commands.push_back(Move(Command));
            PushCommitted(Move(Transaction));
            return;
        }

        UndoStack.back().Commands.push_back(Move(Command));
        RedoStack.clear();
    }

    void FTransactionManager::AbortTransaction()
    {
        bOpen = false;
        Open = FTransaction{};
    }

    void FTransactionManager::PushCommitted(FTransaction&& Transaction)
    {
        if ((int32)UndoStack.size() >= MaxHistory)
        {
            UndoStack.erase(UndoStack.begin());
        }
        UndoStack.push_back(Move(Transaction));
        RedoStack.clear();
    }

    void FTransactionManager::Undo()
    {
        if (UndoStack.empty())
        {
            return;
        }

        FTransaction Transaction = Move(UndoStack.back());
        UndoStack.pop_back();

        bApplying = true;
        Transaction.Undo();
        bApplying = false;

        RedoStack.push_back(Move(Transaction));

        if (OnPostApply)
        {
            OnPostApply();
        }
    }

    void FTransactionManager::Redo()
    {
        if (RedoStack.empty())
        {
            return;
        }

        FTransaction Transaction = Move(RedoStack.back());
        RedoStack.pop_back();

        bApplying = true;
        Transaction.Redo();
        bApplying = false;

        UndoStack.push_back(Move(Transaction));

        if (OnPostApply)
        {
            OnPostApply();
        }
    }

    void FTransactionManager::Clear()
    {
        UndoStack.clear();
        RedoStack.clear();
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
