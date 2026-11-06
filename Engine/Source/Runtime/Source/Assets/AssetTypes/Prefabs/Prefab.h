#pragma once

#include "World/ECS/Registry.h"
#include "Containers/Function.h"
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

        // The entities live in an ECS registry, which reflection cannot describe, so the prefab names them.
        void VisitAdditionalObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;
        void PostLoad() override;

        bool IsAsset() const override { return true; }

        static uint32 GetDataGeneration();
        static void   BumpDataGeneration();

        // Never a revision a prefab reaches, so a copy stamped with it always reads as behind.
        static constexpr uint32 kStaleRevision = ~0u;

        // Moves on every change to Registry, so a placed copy can tell it is behind the asset.
        NODISCARD uint32 GetRevision() const { return Revision; }
        void MarkRegistryChanged();

        // Refreshes only the placed copies behind their prefab, which an undo past a prefab update leaves.
        static void RefreshStaleInstances(CWorld* World);

        // Returns root entity of new instance (ECS::NullEntity on failure). OffsetTransform applied to root.
        ECS::FEntity Instantiate(CWorld* TargetWorld, const FTransform& OffsetTransform = FTransform(), ECS::FEntity Parent = ECS::NullEntity);

        // Adds and removes entities matched by StableID to fit the prefab, keeping the root's placed world transform.
        void RefreshInstance(CWorld* World, ECS::FEntity InstanceRoot);

        // Refreshes every prefab instance in World; called from CWorld::InitializeWorld after registry swap.
        static void RefreshAllInstancesInWorld(CWorld* World);

        // Run on the pending registry before it goes live, so a deleted prefab's placed copies never spawn.
        static void CullOrphanedInstances(ECS::FRegistry& Registry);

        // Call after this prefab's data changes, so every open level shows the edit at once.
        void RefreshInstancesInLoadedWorlds();

        // Destroys every live instance of THIS prefab across all loaded worlds
        void DestroyAllInstancesInLoadedWorlds();

        // Strips SPrefabInstanceComponent from the subtree, and fails when InstanceRoot is not an instance root of a prefab.
        static bool DetachInstance(CWorld* World, ECS::FEntity InstanceRoot);

        // Replaces this prefab with a deep copy of RootEntity and descendants from SourceWorld.
        // Adopting tags every captured source entity as a member of this prefab, so a pushed-back instance stays one instance.
        void CaptureFromWorld(CWorld* SourceWorld, ECS::FEntity RootEntity, bool bAdoptSourceAsInstance = false);

        // The prefab's own component the overrides diff against and reset to, or null when it is absent.
        void* ResolvePrefabComponentPtr(const FName& StableID, CStruct* Struct);

        // ECS::NullEntity when absent, from a lookup rebuilt only when the data generation moves.
        ECS::FEntity FindEntityByStableID(const FName& StableID);

        // Instance root (bIsRoot node) at or above Entity, or ECS::NullEntity if Entity is not a prefab instance.
        static ECS::FEntity FindInstanceRoot(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Rewrites the instance root's override entries for this component from a diff against the prefab.
        static void RecaptureComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        // The override paths one node holds for ComponentType, which an undo record keeps beside the value.
        static TVector<FName> GetComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        // Puts those paths back and takes every other leaf from the prefab's current data, since the restored value may predate it.
        static void RestoreComponentOverrides(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType, const TVector<FName>& Paths);

        // An undo brought back part of this placement from an older image, so the next stale pass refreshes it.
        static void MarkInstanceStale(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // Keeps a refresh from pruning the added component, or clears its removed mark when the prefab ships it.
        static void NoteComponentAdded(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        // Keeps a refresh from re-adding the removed component, or drops it from the added set when the instance added it.
        static void NoteComponentRemoved(ECS::FRegistry& Registry, ECS::FEntity Entity, CStruct* ComponentType);

        // Instance roots whose ledger NoteEntitiesRemoved would write for these doomed entities.
        static void CollectRemovedNodeRoots(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Doomed, TVector<ECS::FEntity>& OutRoots);

        // Records inherited instance nodes the user is deleting, so a refresh never respawns them.
        static void NoteEntitiesRemoved(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Doomed);

        // Called with true before and false after RefreshInstancesInLoadedWorlds rewrites a world, so an editor can record it.
        static void SetLoadedWorldRefreshHook(TFunction<void(CWorld*, bool)> Hook);

        // Per-instance tracking, which belongs to a placed copy and must never be captured into an asset.
        NODISCARD static bool IsInstanceTrackingComponent(uint32 ID);

        // Deep copy that remaps relationships and entity handles, clearing any that escape the copied set.
        static void CopyRegistry(ECS::FRegistry& Source, ECS::FRegistry& Dest, THashMap<ECS::FEntity, ECS::FEntity>& OutMap,
            const TVector<ECS::FEntity>* SourceEntities = nullptr,
            bool(*ExtraSkipStorage)(uint32) = nullptr);

        // A variant owns no authored registry: Registry is RESOLVED from ParentPrefab plus the delta
        // below, so a parent edit reaches every descendant without anything being copied forward.

        // Prefab this one is a variant of. Null for a root prefab, whose Registry is authored directly.
        PROPERTY(ReadOnly, Category = "Variant")
        TStrongObjectPtr<CPrefab> ParentPrefab;

        NODISCARD bool IsVariant() const { return ParentPrefab != nullptr; }

        // Still true when the parent asset went missing, so a save cannot write an empty Registry over the delta.
        NODISCARD bool HoldsVariantPayload() const { return IsVariant() || bVariantPayloadOnDisk; }

        // True if Candidate is this prefab or anywhere up its parent chain. Guards cycles.
        NODISCARD bool IsDescendantOf(const CPrefab* Candidate) const;

        // Rebuilds Registry from the parent chain, leaving it empty and logging on a cycle or missing parent.
        void ResolveVariant();

        // Called on save to store the diff against the resolved parent, while a root prefab keeps Registry verbatim.
        void CaptureVariantDelta();

        // Re-resolves loaded descendant variants deepest last and refreshes their instances, after this prefab's data changes.
        void PropagateToVariants();

        // Loaded prefabs whose ParentPrefab is this one.
        NODISCARD TVector<CPrefab*> FindDirectVariants() const;

        // Bumped every time ResolveVariant rebuilds Registry. An open editor watches this to know its
        // preview world is showing data the parent has since replaced.
        NODISCARD uint32 GetVariantResolveCount() const { return VariantResolveCount; }

        // A variant serializes only its delta, so an unresolved Registry is empty rather than authoritative.
        NODISCARD bool IsUnresolvedVariant() const { return bVariantResolveFailed; }

        ECS::FRegistry Registry;

        // Child transforms from before the editor's last commit; a refresh moves only instance children still at these values.
        THashMap<FName, FTransform> PreCommitTransforms;

        // Script classes each node had before that commit; a refresh drops only scripts the prefab itself dropped.
        THashMap<FName, TVector<const CClass*>> PreCommitScripts;

        void CapturePreCommitState();
        void ClearPreCommitState();

        // The only entities a variant serializes, each paired with a parent node by its SPrefabComponent StableID.
        ECS::FRegistry VariantDelta;

        // The ledger. Public because the reflector's generated code takes offsetof on every PROPERTY;
        // nothing outside the variant paths should write these.
        PROPERTY(Hidden)
        TVector<SPrefabPropertyOverride> VariantOverriddenProperties;

        PROPERTY(Hidden)
        TVector<SPrefabComponentRef> VariantAddedComponents;

        PROPERTY(Hidden)
        TVector<SPrefabComponentRef> VariantRemovedComponents;

        PROPERTY(Hidden)
        TVector<SPrefabVariantNode> VariantStructuralNodes;

        PROPERTY(Hidden)
        TVector<FName> VariantRemovedEntities;

    private:

        void ResolveVariantGuarded(TVector<const CPrefab*>& VisitStack);
        void ApplyVariantDelta();
        void ClearVariantDelta();

        void RebuildStableIDLookup();

        static void RefreshInstancesInWorld(CWorld* World, bool bOnlyStale);

        THashMap<FName, ECS::FEntity> StableIDLookup;
        uint32                        StableIDLookupGeneration = 0;
        bool                          bStableIDLookupBuilt     = false;
        uint32                        VariantResolveCount      = 0;
        uint32                        Revision                 = 0;

        // Transient, since it describes this session's parent chain rather than anything on disk.
        bool                          bVariantResolveFailed    = false;

        // What the file said its payload was, which outlives a ParentPrefab that failed to resolve.
        bool                          bVariantPayloadOnDisk    = false;
    };
}
