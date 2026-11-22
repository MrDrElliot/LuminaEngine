#pragma once

#include "World/ECS/Registry.h"


#include "Containers/HashTable.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Core/Math/Transform.h"

namespace Lumina
{
    class CWorld;
    class CObject;

    // Returns the affected entity or ECS::NullEntity. DropTarget is only what was hit, and bAttachToTarget is set only by an outliner row drop so a viewport drop never parents a crate to the floor.
    using FEditorAssetDropHandler = TFunction<ECS::FEntity(CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)>;

    class EDITOR_API FEditorAssetDropRegistry
    {
    public:

        static FEditorAssetDropRegistry& Get();

        // bPlacesEntity marks a handler that spawns a new entity with no target, which a viewport drag previews live.
        void Register(FName AssetClass, FEditorAssetDropHandler Handler, bool bPlacesEntity = false);

        const FEditorAssetDropHandler* FindHandler(FName AssetClass) const;

        NODISCARD bool PlacesEntity(FName AssetClass) const;

        // Runs the asset's handler, or with none sets the one object field on DropTarget's components that takes it.
        ECS::FEntity Drop(CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget) const;

        // Fills the editable object field on DropTarget that takes Asset, preferring an empty one when several do.
        static ECS::FEntity AssignToComponentField(CWorld* World, CObject* Asset, ECS::FEntity DropTarget);

    private:

        struct FEntry
        {
            FEditorAssetDropHandler Handler;
            bool                    bPlacesEntity = false;
        };

        // The class's own entry, else the nearest super class's, so one registration covers its subclasses.
        const FEntry* FindEntry(FName AssetClass) const;

        THashMap<FName, FEntry> Handlers;
    };
}
