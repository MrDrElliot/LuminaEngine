#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"
#include "World/Entity/Systems/ExclusiveSystem.h"
#include "TerrainCollisionSystem.generated.h"

namespace Lumina
{
    class FTerrainStore;
    struct STerrainColliderComponent;
    namespace Physics { class FCollisionShape; class IPhysicsScene; }

    namespace TerrainCollisionInternal
    {
        // Filled by a background task and read once its flag is set.
        struct FPatchBuild
        {
            TAtomic<bool> bDone{false};
            // Set when the patch is dropped, so a build still queued skips its work.
            TAtomic<bool> bCanceled{false};
            TSharedPtr<const Physics::FCollisionShape> Shape;
        };

        struct FPatch
        {
            uint32                  GroupID    = 0;
            TSharedPtr<FPatchBuild> Build;
            uint32                  LastWanted = 0;
            // Heights changed under it after its body or running build read them.
            bool                    bStale     = false;
        };

        // Where a terrain lies, which a change to drops every patch since none of them fit any more.
        struct FTerrainLayout
        {
            TSharedPtr<FTerrainStore> Store;
            FVector3 Corner     = FVector3(0.0f);
            float    Spacing    = 0.0f;
            float    MaxHeight  = 0.0f;
            int32    Resolution = 0;

            bool operator==(const FTerrainLayout&) const = default;
        };

        struct FTerrainCollision
        {
            FTerrainLayout           Layout;
            THashMap<uint64, FPatch> Patches;
            uint32                   SeenVersion = 0;
        };

        // A patch some source asks for this frame, nearest the views first.
        struct FWantedPatch
        {
            uint64 Key      = 0;
            float  Distance = 0.0f;
        };
    }

    // Streams each collider terrain as static height field patches around the cameras, and under every character and moving body, cooked on workers.
    REFLECT()
    class RUNTIME_API CTerrainCollisionSystem : public CExclusiveSystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;
        bool HasWork(const SSystemContext& Context) override;
        void OnExclusiveUpdate(const SExclusiveContext& Context) override;
        void OnTeardown(const SExclusiveContext& Context) override;

        // A terrain past this size keeps only the patches near what could touch it, so queries elsewhere find no collision.
        static bool StreamsCollision(int32 Resolution);

        // Patches with a body, across every terrain.
        int32 GetResidentPatchCount() const;
        bool  HasPendingBuilds() const;

    private:

        void GatherSources(const SExclusiveContext& Context, bool bScanBodies);
        void UpdateTerrain(ECS::FEntity Entity, const TerrainCollisionInternal::FTerrainLayout& Layout, const STerrainColliderComponent& Collider,
                           Physics::IPhysicsScene& Scene, TerrainCollisionInternal::FTerrainCollision& State);
        void GatherWanted(const TerrainCollisionInternal::FTerrainLayout& Layout, float StreamingRadius, TVector<TerrainCollisionInternal::FWantedPatch>& Out) const;
        static void ReleaseTerrain(Physics::IPhysicsScene* Scene, TerrainCollisionInternal::FTerrainCollision& State);
        void ReleaseAll(Physics::IPhysicsScene* Scene);

        THashMap<ECS::FEntity, TerrainCollisionInternal::FTerrainCollision> Terrains;
        TVector<FVector3> Views;
        TVector<FVector3> Bodies;
        TVector<TerrainCollisionInternal::FWantedPatch> WantedScratch;
        Physics::IPhysicsScene* BoundScene = nullptr;
        uint32 Frame = 0;
    };
}
