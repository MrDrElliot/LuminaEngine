#pragma once

#include "World/ECS/Registry.h"
#include "Containers/Name.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Math/Transform.h"
#include "Assets/AssetTypes/Prefabs/PrefabComponents.h"
#include "Prefab.generated.h"

namespace Lumina
{
    class CWorld;
    class CStruct;

    REFLECT()
    class RUNTIME_API CPrefab : public CObject
    {
        GENERATED_BODY()
    public:

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;

        bool IsAsset() const override { return true; }

        static uint32 GetDataGeneration();
        static void   BumpDataGeneration();

        /** Returns root entity of new instance (ECS::NullEntity on failure). OffsetTransform applied to root. */
        ECS::FEntity Instantiate(CWorld* TargetWorld, const FTransform& OffsetTransform = FTransform(), ECS::FEntity Parent = ECS::NullEntity);

        /** Diff-applies prefab data to one instance (matched by StableID): adds missing, removes
         *  extra. Root's world transform is preserved (placed location wins). */
        void RefreshInstance(CWorld* World, ECS::FEntity InstanceRoot);

        /** Refreshes every prefab instance in World; called from CWorld::InitializeWorld after registry swap. */
        static void RefreshAllInstancesInWorld(CWorld* World);

        /** Destroys instances whose source prefab is gone (SourcePrefab resolved to null / marked-destroy)
         *  from Registry, with clean hierarchy unlink. Run on the pending set before it goes live so a
         *  deleted prefab's placed copies never spawn into the world. */
        static void CullOrphanedInstances(ECS::FRegistry& Registry);

        /** Re-syncs every live instance of THIS prefab across all loaded worlds (GWorldManager contexts).
         *  Call after the prefab's data changes (e.g. on save) so open levels reflect the edit immediately. */
        void RefreshInstancesInLoadedWorlds();

        /** Destroys every live instance of THIS prefab across all loaded worlds */
        void DestroyAllInstancesInLoadedWorlds();

        /** Strips SPrefabInstanceComponent from the subtree, unpairing it from this prefab.
         *  Returns false if InstanceRoot is not an instance root sourced from this prefab. */
        static bool DetachInstance(CWorld* World, ECS::FEntity InstanceRoot);

        /** Replaces this prefab with a deep copy of RootEntity and descendants from SourceWorld. */
        void CaptureFromWorld(CWorld* SourceWorld, ECS::FEntity RootEntity);

        /** Pointer to the StableID-matched prefab entity's reflected component of the given type, or
         *  null if that entity/component is absent. The per-leaf override baseline + reset-to-prefab default. */
        void* ResolvePrefabComponentPtr(const FName& StableID, CStruct* Struct);

        /** Prefab entity carrying StableID, or ECS::NullEntity. Backed by a lookup rebuilt only when the
         *  prefab data generation moves, so the details panel can ask per property without rescanning
         *  every entity each time. */
        ECS::FEntity FindEntityByStableID(const FName& StableID);

        /** Instance root (bIsRoot node) at or above Entity, or ECS::NullEntity if Entity is not a prefab instance. */
        static ECS::FEntity FindInstanceRoot(ECS::FRegistry& Registry, ECS::FEntity Entity);

        /** Recomputes the override leaf set for (Entity, ComponentType) by diffing the live component against
         *  its prefab baseline, rewriting the instance root's ledger entries for that pair (lazy-created).
         *  When the component matches the prefab again the entries are cleared. */
        static void RecaptureComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        /** Records that ComponentType was added to an instance node (so refresh never prunes it), or, if the
         *  prefab actually ships it, just clears any prior "removed" mark so it inherits again. */
        static void NoteComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        /** Records that an inherited ComponentType was deleted from an instance node (so refresh never re-adds
         *  it); for an instance-added component, simply drops it from the added set. */
        static void NoteComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        // Per-instance tracking, which belongs to a placed copy and must never be captured into an asset.
        NODISCARD static bool IsInstanceTrackingComponent(uint32 ID);

        /** Deep-copies entities Source->Dest (OutMap = src->dest ids); remaps relationships + entity-handle
         *  props (escaping refs cleared). SourceEntities limits the set; ExtraSkipStorage skips more types by id. */
        static void CopyRegistry(ECS::FRegistry& Source, ECS::FRegistry& Dest, THashMap<ECS::FEntity, ECS::FEntity>& OutMap,
            const TVector<ECS::FEntity>* SourceEntities = nullptr,
            bool(*ExtraSkipStorage)(uint32) = nullptr);

        // A variant owns no authored registry: Registry is RESOLVED from ParentPrefab plus the delta
        // below, so a parent edit reaches every descendant without anything being copied forward.

        /** Prefab this one is a variant of. Null for a root prefab, whose Registry is authored directly. */
        PROPERTY(ReadOnly, Category = "Variant")
        TObjectPtr<CPrefab> ParentPrefab;

        NODISCARD bool IsVariant() const { return ParentPrefab != nullptr; }

        // Still true when the parent asset went missing, so a save cannot write an empty Registry over the delta.
        NODISCARD bool HoldsVariantPayload() const { return IsVariant() || bVariantPayloadOnDisk; }

        /** True if Candidate is this prefab or anywhere up its parent chain. Guards cycles. */
        NODISCARD bool IsDescendantOf(const CPrefab* Candidate) const;

        /** Rebuilds Registry from the parent chain, resolving ancestors first. No-op for a root prefab.
         *  A cycle or a missing parent leaves Registry empty and logs. */
        void ResolveVariant();

        /** Diffs the current Registry against the resolved parent and stores the result as this variant's
         *  delta. Called on save; a root prefab keeps its Registry verbatim instead. */
        void CaptureVariantDelta();

        /** Re-resolves every loaded variant descending from this prefab, deepest last, then refreshes
         *  their live instances. Call after this prefab's data changes. */
        void PropagateToVariants();

        /** Loaded prefabs whose ParentPrefab is this one. */
        NODISCARD TVector<CPrefab*> FindDirectVariants() const;

        // Bumped every time ResolveVariant rebuilds Registry. An open editor watches this to know its
        // preview world is showing data the parent has since replaced.
        NODISCARD uint32 GetVariantResolveCount() const { return VariantResolveCount; }

        // A variant serializes only its delta, so an unresolved Registry is empty rather than authoritative.
        NODISCARD bool IsUnresolvedVariant() const { return bVariantResolveFailed; }

        ECS::FRegistry Registry;

        /** Entities the variant diverges on. Only these are serialized; everything else comes from the
         *  parent. Each carries SPrefabComponent so its StableID pairs it with a parent node. */
        ECS::FRegistry VariantDelta;

        // The ledger. Public because the reflector's generated code takes offsetof on every PROPERTY;
        // nothing outside the variant paths should write these.
        PROPERTY()
        TVector<SPrefabPropertyOverride> VariantOverriddenProperties;

        PROPERTY()
        TVector<SPrefabComponentRef> VariantAddedComponents;

        PROPERTY()
        TVector<SPrefabComponentRef> VariantRemovedComponents;

        PROPERTY()
        TVector<SPrefabVariantNode> VariantStructuralNodes;

        PROPERTY()
        TVector<FName> VariantRemovedEntities;

    private:

        void ResolveVariantGuarded(TVector<const CPrefab*>& VisitStack);
        void ApplyVariantDelta();
        void ClearVariantDelta();

        void RebuildStableIDLookup();

        THashMap<FName, ECS::FEntity> StableIDLookup;
        uint32                        StableIDLookupGeneration = 0;
        bool                          bStableIDLookupBuilt     = false;
        uint32                        VariantResolveCount      = 0;

        // Transient, since it describes this session's parent chain rather than anything on disk.
        bool                          bVariantResolveFailed    = false;

        // What the file said its payload was, which outlives a ParentPrefab that failed to resolve.
        bool                          bVariantPayloadOnDisk    = false;
    };
}
