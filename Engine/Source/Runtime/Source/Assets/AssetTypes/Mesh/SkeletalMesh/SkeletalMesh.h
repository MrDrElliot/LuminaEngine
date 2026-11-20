#pragma once

#include "Animation/BoneLOD.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "Core/Threading/Thread.h"
#include <atomic>
#include "SkeletalMesh.generated.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;

    REFLECT()
    class RUNTIME_API CSkeletalMesh : public CMesh
    {
        GENERATED_BODY()

        friend class CMeshImporter;

    public:

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;
        bool IsAsset() const override { return true; }
        bool IsSkinned() const override { return true; }
        SAABB GetCullBounds() const override;

        // Rewrites the joint indices into Skeleton's bone order, matching by name, after Skeleton is reassigned.
        void RemapJointIndicesToSkeleton();

        // The order this mesh's poses evaluate in against Skeleton, which bone LOD cuts; null without skin data. Safe from any thread.
        const BoneLOD::FBoneLODLayout* GetBoneLODLayout(const FSkeletonResource* Skeleton) const;

        // This mesh's socket by that name, else the skeleton's, so a mesh socket overrides a skeleton one of the same name.
        const SMeshSocket* FindSocket(const FName& SocketName) const;

        // Every socket this mesh resolves, its own first, then the skeleton's that it does not override.
        void CollectActiveSockets(TVector<const SMeshSocket*>& OutSockets) const;

        // Bumped whenever this mesh's own sockets change; the skeleton keeps its own generation.
        uint32 GetSocketGeneration() const { return OwnSocketGeneration.load(std::memory_order_relaxed); }
        void BumpSocketGeneration() { OwnSocketGeneration.fetch_add(1, std::memory_order_relaxed); }

        PROPERTY(Editable, Category = "Skeleton")
        TStrongObjectPtr<CSkeleton> Skeleton;

        // Attach points for this mesh only, each replacing a skeleton socket of the same name on this mesh.
        PROPERTY(Editable, Category = "Sockets")
        TVector<SMeshSocket> Sockets;

        // What a runtime merge built this from, held so the parts stay loaded while anything wears it.
        TVector<TStrongObjectPtr<CSkeletalMesh>> MergeSources;

    private:

        struct FBoneLODLayoutEntry
        {
            uint64             LayoutKey        = 0;
            const void*        Resource         = nullptr;
            size_t             PaletteEntries   = 0;
            uint32             SocketGeneration = 0;
            TUniquePtr<BoneLOD::FBoneLODLayout> Layout;

            bool Matches(uint64 InLayoutKey, const void* InResource, size_t InPaletteEntries, uint32 InSocketGeneration) const
            {
                return LayoutKey == InLayoutKey && Resource == InResource && PaletteEntries == InPaletteEntries
                    && SocketGeneration == InSocketGeneration;
            }
        };

        // The skeleton whose bone order the joint indices currently follow, which a reassigned Skeleton remaps them from.
        TStrongObjectPtr<CSkeleton> JointIndicesSkeleton;

        // Never freed before the mesh, because animation threads hold pointers into them without a lock.
        mutable TVector<TUniquePtr<FBoneLODLayoutEntry>> BoneLODLayouts;
        mutable std::atomic<const FBoneLODLayoutEntry*>  ActiveBoneLODLayout{ nullptr };
        mutable FMutex                                   BoneLODMutex;

        std::atomic<uint32> OwnSocketGeneration{ 1 };
    };
}
