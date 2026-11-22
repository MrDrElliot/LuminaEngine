#pragma once

#include "Containers/HashTable.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "World/Forest/ForestScatter.h"
#include "ForestSystem.generated.h"

namespace Lumina
{
    struct SForestComponent;

    namespace ForestSystemInternal
    {
        // A tile of plants, in the near band that holds every species, the mid band that leaves out those culled before it, or the far band of impostor points.
        struct FTileKey
        {
            int32 Band = 0;
            int32 X    = 0;
            int32 Z    = 0;

            uint64 Pack() const { return ((uint64)(uint8)Band << 56) | ((uint64)(uint32)(X & 0x0FFFFFFF) << 28) | (uint64)(uint32)(Z & 0x0FFFFFFF); }
            bool operator==(const FTileKey&) const = default;
        };

        // Filled by a background task and read once its flag is set.
        struct FBuild
        {
            TAtomic<bool>             bDone{false};
            // Set when the tile is dropped, so a build still queued skips its work and frees the slot for one near the view.
            TAtomic<bool>             bCanceled{false};
            TVector<Forest::FPlant>   Plants;
        };

        struct FTile
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
            TVector<SFoliageType>                              Types;
            THashMap<uint64, FTile>                            Tiles;
            TVector<TSharedPtr<FBuild>>                        Running;
            TVector<FTerrainTrack>                             Terrains;
            uint64                                             PaintHash = ~0ull;
            // Where tiles switch to impostor points, never past the component's ImpostorDistance.
            float                                              PointBandStart = 0.0f;
            // Paint is hashed again only when the component was written after this tick.
            uint32                                             CheckedTick = 0;
        };

        // A tile the view asks for this frame, with how far it is, which orders its build and apply.
        struct FWanted
        {
            FTileKey Key;
            float    Distance = 0.0f;
        };
    }

    // Streams each forest in tiles around the view, scattered on worker threads, with real foliage near and impostor points far.
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
        void ReleaseTiles(CWorld& World, ForestSystemInternal::FForestState& State);
        void ApplyBuild(CWorld& World, const ForestSystemInternal::FTileKey& Key, ForestSystemInternal::FTile& Tile, ForestSystemInternal::FForestState& State);
        void GatherWanted(CWorld& World, const SForestComponent& Forest, const FVector3& Center, const ForestSystemInternal::FForestState& State, TVector<ForestSystemInternal::FWanted>& Out);
        void StartBuilds(const SForestComponent& Forest, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State,
                         THashSet<uint64>& OutWantedSet, TVector<ForestSystemInternal::FTileKey>& OutUnready);
        void ApplyFinishedBuilds(CWorld& World, const TVector<ForestSystemInternal::FWanted>& Wanted, ForestSystemInternal::FForestState& State);
        void RetireStaleTiles(CWorld& World, const THashSet<uint64>& WantedSet, const TVector<ForestSystemInternal::FTileKey>& Unready, ForestSystemInternal::FForestState& State);
        // A tile only replaces another once the renderer has drawn it, since its entity takes frames to bind.
        void TrackDrawnTiles(CWorld& World, ForestSystemInternal::FForestState& State);
        void PublishStats(CWorld& World, const ForestSystemInternal::FForestState& State);

        THashMap<ECS::FEntity, ForestSystemInternal::FForestState> Forests;
    };
}
