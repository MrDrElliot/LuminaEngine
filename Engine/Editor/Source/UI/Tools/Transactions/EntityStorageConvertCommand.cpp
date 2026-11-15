#include "EntityStorageConvertCommand.h"

#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "World/Entity/Components/EditorComponent.h"
#include "World/Subsystems/WorldSettings.h"

namespace Lumina
{
    FEntityStorageConvertCommand::FEntityStorageConvertCommand(CWorld* InWorld)
        : World(InWorld)
        , bWasExternal(ExternalEntities::IsEnabled(InWorld->GetEntityRegistry()))
        , bWasPackageDirty(InWorld->GetPackage()->IsDirty())
        , ChangesBefore(InWorld->GetExternalEntityChanges())
    {
    }

    void FEntityStorageConvertCommand::Redo()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = W->GetEntityRegistry();
        TVector<ECS::FEntity> Entities;
        Registry.ForEachEntityExcept<FEditorComponent>([&](ECS::FEntity Entity)
        {
            Entities.Add(Entity);
        });

        W->GetDefaultWorldSettings().bExternalEntities = !bWasExternal;
        FExternalEntityChanges& Changes = W->GetExternalEntityChanges();
        if (bWasExternal)
        {
            // Only loaded entities are folded in, so a file that failed to load stays on disk.
            Changes.Dirty.Reset();
            for (ECS::FEntity Entity : Entities)
            {
                if (const FName Guid = ExternalEntities::GuidOf(Registry, Entity); !Guid.IsNone())
                {
                    Changes.Deleted.Add(Guid);
                }
            }
        }
        else
        {
            ExternalEntities::MarkChanged(W, Entities);
        }
        W->GetPackage()->MarkDirty();
    }

    void FEntityStorageConvertCommand::Undo()
    {
        CWorld* W = World.Get();
        if (W == nullptr)
        {
            return;
        }

        CPackage* Package = W->GetPackage();

        // A clean package means the convert was saved, so the restored mode still has to be written.
        const bool bDirty = bWasPackageDirty || !Package->IsDirty();
        W->GetDefaultWorldSettings().bExternalEntities = bWasExternal;
        W->GetExternalEntityChanges() = ChangesBefore;
        if (bDirty)
        {
            Package->MarkDirty();
        }
        else
        {
            Package->ClearDirty();
        }
    }
}
