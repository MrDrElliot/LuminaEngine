#pragma once

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "EditorTransaction.h"
#include "World/Entity/Registry/EntityRegistry.h"

namespace Lumina
{
    class CWorld;

    // Undo record for an operation that DESTROYS entities, the mirror of FEntityCreationCommand.
    class FEntityDestroyCommand final : public IUndoableCommand
    {
    public:

        // Serializes every candidate up front, since none of them can be read back once they are gone.
        FEntityDestroyCommand(CWorld* InWorld, const TVector<entt::entity>& InCandidates);

        void Undo() override;
        void Redo() override;

        // Prunes the candidates down to the ones the operation actually destroyed.
        void Finalize() override;

        bool IsNoOp() const override { return Entries.empty(); }

        // The candidates plus their descendants, which is the widest set a delete can reach.
        static void CollectCandidates(FEntityRegistry& Registry, const TVector<entt::entity>& Roots,
                                      TVector<entt::entity>& Out);

    private:

        struct FEntry
        {
            entt::entity Entity = entt::null;
            int64        Offset = 0;
            int64        Size = 0;
        };

        // Weak, since a strong ref would keep a torn-down world alive and stop a stale undo from no-opping.
        TWeakObjectPtr<CWorld>  World;

        TVector<FEntry>         Entries;
        TVector<uint8>          Data;
    };
}
