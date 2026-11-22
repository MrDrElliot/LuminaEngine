#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "PrefabComponents.generated.h"

namespace Lumina
{
    class CPrefab;

    /** Placed on every prefab-registry entity; StableID survives save/load to pair entities with instances. */
    REFLECT(Component, HideInComponentList)
    struct RUNTIME_API SPrefabComponent
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly)
        FName StableID;
    };

    /** Marks a world entity as a prefab instance; refreshed from the source prefab on world load. */
    REFLECT(Component, Category = "Prefab")
    struct RUNTIME_API SPrefabInstanceComponent
    {
        GENERATED_BODY()

        PROPERTY(ReadOnly, Category = "Prefab")
        TStrongObjectPtr<CPrefab> SourcePrefab;

        PROPERTY(ReadOnly, Category = "Prefab")
        FName StableID;

        PROPERTY(ReadOnly, Category = "Prefab")
        bool bIsRoot = false;

        // The source's revision this copy last took, so an undo that restores older data is caught and refreshed.
        PROPERTY(Hidden)
        uint32 AppliedRevision = 0;
    };

    /** One overridden leaf on an instance: a single property addressed by a delimiter-joined path
     *  ("Top" or "Nested.Field"). Top-level properties are a single segment. */
    REFLECT()
    struct RUNTIME_API SPrefabPropertyOverride
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName EntityStableID;

        PROPERTY(Hidden)
        FName ComponentType;

        PROPERTY(Hidden)
        FName PropertyPath;
    };

    /** Identifies a component on a specific instance node (instance-added or inherited-then-removed). */
    REFLECT()
    struct RUNTIME_API SPrefabComponentRef
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName EntityStableID;

        PROPERTY(Hidden)
        FName ComponentType;
    };

    // A node a variant adds or reparents, with parentage by StableID since an inherited parent has no id in the variant's registry.
    REFLECT()
    struct RUNTIME_API SPrefabVariantNode
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FName StableID;

        /** None means the variant's root. */
        PROPERTY(Hidden)
        FName ParentStableID;

        /** The parent prefab has no node with this StableID; the variant introduces it. */
        PROPERTY(Hidden)
        bool bAdded = false;
    };

    /** The override ledger. Lives on the instance ROOT only and tracks the whole subtree's divergence
     *  from the source prefab, keyed by node StableID. PROPERTY(Hidden) => serialized with the world but
     *  never rendered in the details panel (FProperty::IsVisible() is false without Editable/ReadOnly);
     *  HideInComponentList keeps it out of the Add-Component picker. Created lazily on first divergence. */
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SPrefabOverrideComponent
    {
        GENERATED_BODY()

        /** Leaves the instance has diverged on; prefab updates skip these. */
        PROPERTY(Hidden)
        TVector<SPrefabPropertyOverride> PropertyOverrides;

        /** Components the instance added; a refresh must never prune them. */
        PROPERTY(Hidden)
        TVector<SPrefabComponentRef> AddedComponents;

        /** Inherited components the instance deleted; a refresh must never re-add them. */
        PROPERTY(Hidden)
        TVector<SPrefabComponentRef> RemovedComponents;

        // Inherited nodes the instance deleted, which a refresh must never respawn.
        PROPERTY(Hidden)
        TVector<FName> RemovedEntities;
    };
}
