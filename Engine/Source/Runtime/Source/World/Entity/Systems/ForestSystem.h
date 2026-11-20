#pragma once

#include "Containers/HashTable.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "World/Forest/ForestAggregate.h"
#include "ForestSystem.generated.h"

namespace Lumina
{
    struct SForestComponent;

    namespace ForestSystemInternal
    {
        // Filled by a background task and read once its flag is set.
        struct FBuild
        {
            TAtomic<bool>             bDone{false};
            // Set when the node is dropped, so a build still queued skips its work and frees the slot for one near the view.
            TAtomic<bool>             bCanceled{false};
            bool                      bInstances = false;
            Forest::FAggregateResult  Aggregate;
            TVector<Forest::FPlant>   Plants;
        };

        struct FNode
        {
            ECS::FEntity        Entity = ECS::NullEntity;
            TSharedPtr<FBuild>  Build;
            bool                bReady = false;
            // Updates since the entity was made, and how many of the last ones the renderer could already draw it.
            uint16              ReadyFrames = 0;
            uint16              DrawnFrames = 0;
        };

        // One terrain under the forest, copied again only once its sculpting settles.
        struct FTerrainTrack
        {
            ECS::FEntity                               Entity = ECS::NullEntity;
            float                                      Spacing = 0.0f;
            TSharedPtr<const Forest::FTerrainSnapshot> Snapshot;
            uint32                                     Version = ~0u;
            uint32                                     StableFrames = 0;
            uint32                                     SeenVersion = ~0u;
        };

        struct FForestState
        {
            uint64                                             StateHash  = ~0ull;
            TSharedPtr<const Forest::FRules>                   Rules;
            TVector<TSharedPtr<const Forest::FSpeciesGeometry>> Geometry;
            TVector<SFoliageType>                              Types;
            THashMap<uint64, FNode>                            Nodes;
            TVector<TSharedPtr<FBuild>>                        Running;
            TVector<FTerrainTrack>                             Terrains;
            uint64                                             PaintHash = ~0ull;
            uint32                                             PaintStableFrames = 0;
            // Paint is hashed again only when the component was written after this tick.
            uint32                                             CheckedTick = 0;
            // Painted ground whose aggregates still show the old paint, rebuilt once the strokes stop.
            TVector<Forest::FArea>                             StaleAggregateAreas;
        };

        // A node the view asks for this frame, with how far it is, which orders its build and apply.
        struct FWanted
        {
            Forest::FNodeKey Key;
            float            Distance = 0.0f;
        };
    }

    // Streams each forest in tiles around the view, real foliage near it and voxel aggregates of whole tiles beyond, built on worker threads.
    REFLECT()
    class RUNTIME_API CForestSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        bool HasWork(const SSystemContext& Context) override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

    private:

        void UpdateForest(CWorld& World, ECS::FEntity Entity, const SForestComponent& Forest, bool bWritten, ForestSystemInternal::FForestState& State);
        bool RefreshState(CWorld& World, const SForestComponent& Forest, const FVector3& Center, bool bWritten, ForestSystemInternal::FForestState& State);
        void ReleaseNodes(CWorld& World, ForestSystemInternal::FForestState& State);
        void ApplyBuild(CWorld& World, const Forest::FNodeKey& Key, ForestSystemInternal::FNode& Node, ForestSystemInternal::FForestState& State);
        // Walks the quadtree from the top, refining a node while its aggregate's voxels would look too large.
        void GatherWanted(CWorld& World, const SForestComponent& Forest, const FVector3& Center, const Forest::FRules& Rules, TVector<ForestSystemInternal::FWanted>& Out);
        void StartBuilds(const SForestComponent& Forest, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State,
                         THashSet<uint64>& OutWantedSet, TVector<Forest::FNodeKey>& OutUnready);
        void ApplyFinishedBuilds(CWorld& World, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State);
        void RetireStaleNodes(CWorld& World, const THashSet<uint64>& WantedSet, const TVector<Forest::FNodeKey>& Unready, ForestSystemInternal::FForestState& State);
        // A node only replaces another once the renderer has drawn it, since its entity takes frames to upload, resolve and bind.
        void TrackDrawnNodes(CWorld& World, ForestSystemInternal::FForestState& State);
        void PublishStats(CWorld& World, const ForestSystemInternal::FForestState& State);

        THashMap<ECS::FEntity, ForestSystemInternal::FForestState> Forests;
    };
}
