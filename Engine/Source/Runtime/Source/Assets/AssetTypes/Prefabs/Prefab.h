#pragma once
#include "Containers/Name.h"
#include "Core/Object/Object.h"
#include "Core/Math/Transform.h"
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

        // A prefab is a browsable asset. Without this it inherits CObject's false, and anything that gates
        // registration on it (the importers' save pass) writes the package but never indexes it -- the
        // browser then shows "not yet indexed" and cannot open it. The world editor's Create Prefab path
        // hid this by calling AssetCreated unconditionally.
        bool IsAsset() const override { return true; }

        /** Bumped whenever prefab data or a live instance's component set is rewritten. Editor details
         *  panels cache raw pointers into entt component storage (and into this registry, for the
         *  reset-to-prefab baseline); a refresh removes/re-emplaces components, which relocates neighbours
         *  under entt's swap-and-pop, and a re-capture replaces the registry outright. Nothing broadcasts
         *  either, so consumers compare this the way they compare DotNet::GetScriptGeneration(). */
        static uint32 GetDataGeneration();
        static void   BumpDataGeneration();

        /** Returns root entity of new instance (entt::null on failure). OffsetTransform applied to root. */
        entt::entity Instantiate(CWorld* TargetWorld, const FTransform& OffsetTransform = FTransform(), entt::entity Parent = entt::null);

        /** Diff-applies prefab data to one instance (matched by StableID): adds missing, removes
         *  extra. Root's world transform is preserved (placed location wins). */
        void RefreshInstance(CWorld* World, entt::entity InstanceRoot);

        /** Refreshes every prefab instance in World; called from CWorld::InitializeWorld after registry swap. */
        static void RefreshAllInstancesInWorld(CWorld* World);

        /** Destroys instances whose source prefab is gone (SourcePrefab resolved to null / marked-destroy)
         *  from Registry, with clean hierarchy unlink. Run on the pending set before it goes live so a
         *  deleted prefab's placed copies never spawn into the world. */
        static void CullOrphanedInstances(entt::registry& Registry);

        /** Re-syncs every live instance of THIS prefab across all loaded worlds (GWorldManager contexts).
         *  Call after the prefab's data changes (e.g. on save) so open levels reflect the edit immediately. */
        void RefreshInstancesInLoadedWorlds();

        /** Destroys every live instance of THIS prefab across all loaded worlds */
        void DestroyAllInstancesInLoadedWorlds();

        /** Strips SPrefabInstanceComponent from the subtree, unpairing it from this prefab.
         *  Returns false if InstanceRoot is not an instance root sourced from this prefab. */
        static bool DetachInstance(CWorld* World, entt::entity InstanceRoot);

        /** Replaces this prefab with a deep copy of RootEntity and descendants from SourceWorld. */
        void CaptureFromWorld(CWorld* SourceWorld, entt::entity RootEntity);

        /** Pointer to the StableID-matched prefab entity's reflected component of the given type, or
         *  null if that entity/component is absent. The per-leaf override baseline + reset-to-prefab default. */
        void* ResolvePrefabComponentPtr(const FName& StableID, CStruct* Struct);

        /** Prefab entity carrying StableID, or entt::null. Backed by a lookup rebuilt only when the
         *  prefab data generation moves, so the details panel can ask per property without rescanning
         *  every entity each time. */
        entt::entity FindEntityByStableID(const FName& StableID);

        /** Instance root (bIsRoot node) at or above Entity, or entt::null if Entity is not a prefab instance. */
        static entt::entity FindInstanceRoot(entt::registry& Registry, entt::entity Entity);

        /** Recomputes the override leaf set for (Entity, ComponentType) by diffing the live component against
         *  its prefab baseline, rewriting the instance root's ledger entries for that pair (lazy-created).
         *  When the component matches the prefab again the entries are cleared. */
        static void RecaptureComponentOverrides(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType);

        /** Records that ComponentType was added to an instance node (so refresh never prunes it), or, if the
         *  prefab actually ships it, just clears any prior "removed" mark so it inherits again. */
        static void NoteComponentAdded(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType);

        /** Records that an inherited ComponentType was deleted from an instance node (so refresh never re-adds
         *  it); for an instance-added component, simply drops it from the added set. */
        static void NoteComponentRemoved(entt::registry& Registry, entt::entity Entity, CStruct* ComponentType);

        /** Deep-copies entities Source->Dest (OutMap = src->dest ids); remaps relationships + entity-handle
         *  props (escaping refs cleared). SourceEntities limits the set; ExtraSkipStorage skips more types by id. */
        static void CopyRegistry(entt::registry& Source, entt::registry& Dest, THashMap<entt::entity, entt::entity>& OutMap,
            const TVector<entt::entity>* SourceEntities = nullptr,
            bool(*ExtraSkipStorage)(entt::id_type) = nullptr);

        entt::registry Registry;

    private:

        void RebuildStableIDLookup();

        THashMap<FName, entt::entity> StableIDLookup;
        uint32                        StableIDLookupGeneration = 0;
        bool                          bStableIDLookupBuilt     = false;
    };
}
