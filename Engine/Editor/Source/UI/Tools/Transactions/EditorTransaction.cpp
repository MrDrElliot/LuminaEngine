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

        Transaction.Undo();

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

        Transaction.Redo();

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
