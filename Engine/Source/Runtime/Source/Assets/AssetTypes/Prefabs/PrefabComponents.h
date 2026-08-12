#pragma once

#include "Containers/Array.h"
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
        TObjectPtr<CPrefab> SourcePrefab;

        PROPERTY(ReadOnly, Category = "Prefab")
        FName StableID;

        PROPERTY(ReadOnly, Category = "Prefab")
        bool bIsRoot = false;
    };

    /** One overridden leaf on an instance: a single property addressed by a delimiter-joined path
     *  ("Top" or "Nested.Field"). Top-level properties are a single segment. */
    REFLECT()
    struct RUNTIME_API SPrefabPropertyOverride
    {
        GENERATED_BODY()

        PROPERTY()
        FName EntityStableID;

        PROPERTY()
        FName ComponentType;

        PROPERTY()
        FName PropertyPath;
    };

    /** Identifies a component on a specific instance node (instance-added or inherited-then-removed). */
    REFLECT()
    struct RUNTIME_API SPrefabComponentRef
    {
        GENERATED_BODY()

        PROPERTY()
        FName EntityStableID;

        PROPERTY()
        FName ComponentType;
    };

    // A node a variant adds outright, or inherits but reparents. Parentage is by StableID because an
    // added entity usually hangs under an inherited one, which has no id in the variant's own registry.
    REFLECT()
    struct RUNTIME_API SPrefabVariantNode
    {
        GENERATED_BODY()

        PROPERTY()
        FName StableID;

        /** None means the variant's root. */
        PROPERTY()
        FName ParentStableID;

        /** The parent prefab has no node with this StableID; the variant introduces it. */
        PROPERTY()
        bool bAdded = false;
    };

    /** The override ledger. Lives on the instance ROOT only and tracks the whole subtree's divergence
     *  from the source prefab, keyed by node StableID. Bare PROPERTY() => serialized with the world but
     *  never rendered in the details panel (FProperty::IsVisible() is false without Editable/ReadOnly);
     *  HideInComponentList keeps it out of the Add-Component picker. Created lazily on first divergence. */
    REFLECT(Component, HideInComponentList, HideInDetails)
    struct RUNTIME_API SPrefabOverrideComponent
    {
        GENERATED_BODY()

        /** Leaves the instance has diverged on; prefab updates skip these. */
        PROPERTY()
        TVector<SPrefabPropertyOverride> PropertyOverrides;

        /** Components the instance added; a refresh must never prune them. */
        PROPERTY()
        TVector<SPrefabComponentRef> AddedComponents;

        /** Inherited components the instance deleted; a refresh must never re-add them. */
        PROPERTY()
        TVector<SPrefabComponentRef> RemovedComponents;
    };
}
