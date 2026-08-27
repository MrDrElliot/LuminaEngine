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

    // Spawns/applies a dropped asset. Returns the affected entity (created or DropTarget); ECS::NullEntity
    // on failure.
    //
    // DropTarget is the entity the drop LANDED ON -- the outliner row, or whatever the viewport ray hit.
    // It is what a handler acts on (set this mesh's material, play this clip on that character), and it
    // says nothing about hierarchy.
    //
    // bAttachToTarget is the separate question of whether the gesture MEANT "make it a child of that".
    // Only an outliner row drop does; placing into the viewport does not, or dropping a crate onto the
    // floor would parent the crate to the floor. Handlers that spawn consult it before reparenting;
    // handlers that only modify DropTarget ignore it.
    using FEditorAssetDropHandler = TFunction<ECS::FEntity(CWorld* World, CObject* Asset, const FTransform& SpawnTransform, ECS::FEntity DropTarget, bool bAttachToTarget)>;

    class FEditorAssetDropRegistry
    {
    public:

        static FEditorAssetDropRegistry& Get();

        void Register(FName AssetClass, FEditorAssetDropHandler Handler);

        const FEditorAssetDropHandler* FindHandler(FName AssetClass) const;

    private:

        THashMap<FName, FEditorAssetDropHandler> Handlers;
    };
}
