#pragma once

#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"
#include "World/ExternalEntities/ExternalEntities.h"

namespace Lumina
{
    class CWorld;

    // Switches a world between keeping its entities inline and one file per entity, which the next save carries out.
    class FEntityStorageConvertCommand final : public IUndoableCommand
    {
    public:

        // Captures the state to undo to, so construct it before Redo makes the switch.
        explicit FEntityStorageConvertCommand(CWorld* InWorld);

        void Undo() override;
        void Redo() override;
        FName GetName() const override { return "Convert Entity Storage"; }

    private:

        TWeakObjectPtr<CWorld>  World;
        bool                    bWasExternal = false;
        bool                    bWasPackageDirty = false;
        FExternalEntityChanges  ChangesBefore;
    };
}
