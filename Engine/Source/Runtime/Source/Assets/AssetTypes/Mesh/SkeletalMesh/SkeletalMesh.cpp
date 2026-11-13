#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Renderer/SkeletonResource.h"
#include "Animation/BoneLOD.h"

namespace Lumina
{
    void CSkeletalMesh::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Meshes");
        CMesh::Serialize(Ar);
    }

    void CSkeletalMesh::PostLoad()
    {
        CMesh::PostLoad();
        SkeletonJointIndicesAddress = Skeleton;
    }

    // A rig can bake an axis change into its bind pose, so the vertex box is carried through each skinning bone's bind matrix.
    SAABB CSkeletalMesh::GetCullBounds() const
    {
        const SAABB& Local = GetAABB();
        const FSkeletonResource* Resource = Skeleton.IsValid() ? Skeleton->GetSkeletonResource() : nullptr;
        if (!Local.IsValid() || Resource == nullptr || Resource->GetNumBones() == 0)
        {
            return Local;
        }

        TVector<FMatrix4> BindSkinning;
        Skeleton->ComputeBindPoseSkinningMatrices(BindSkinning);

        TVector<uint8> Visited(BindSkinning.Num(), 0u);
        SAABB Posed;
        bool bAny = false;
        const auto Grow = [&](uint32 Bone)
        {
            if (Bone >= (uint32)BindSkinning.Num() || Visited[Bone])
            {
                return;
            }
            Visited[Bone] = 1u;

            const SAABB Box = Local.ToWorld(BindSkinning[Bone]);
            Posed.Min = bAny ? Math::Min(Posed.Min, Box.Min) : Box.Min;
            Posed.Max = bAny ? Math::Max(Posed.Max, Box.Max) : Box.Max;
            bAny = true;
        };

        // Only the bones a meshlet palette names move vertices, so an unweighted bone's bind cannot inflate the box.
        for (uint32 Bone : GetMeshResource().MeshletData.MeshletBoneIndices)
        {
            Grow(Bone);
        }
        if (!bAny)
        {
            for (int32 Bone = 0; Bone < BindSkinning.Num(); ++Bone)
            {
                Grow(Bone);
            }
        }
        return bAny ? Posed : Local;
    }

    void CSkeletalMesh::PostPropertyChange(FProperty* ChangedProperty)
    {
        CMesh::PostPropertyChange(ChangedProperty);

        // Undo restores Skeleton alone, so the indices have to follow it back from here.
        RemapJointIndicesToSkeleton();
        BumpSocketGeneration();
    }

    const SMeshSocket* CSkeletalMesh::FindSocket(const FName& SocketName) const
    {
        if (const SMeshSocket* Socket = FindSocketByName(Sockets, SocketName))
        {
            return Socket;
        }
        return Skeleton.IsValid() ? Skeleton->FindSocket(SocketName) : nullptr;
    }

    void CSkeletalMesh::CollectActiveSockets(TVector<const SMeshSocket*>& OutSockets) const
    {
        OutSockets.Reset();
        for (const SMeshSocket& Socket : Sockets)
        {
            OutSockets.Add(&Socket);
        }

        if (Skeleton.IsValid())
        {
            for (const SMeshSocket& Socket : Skeleton->Sockets)
            {
                if (FindSocketByName(Sockets, Socket.SocketName) == nullptr)
                {
                    OutSockets.Add(&Socket);
                }
            }
        }
    }

    const BoneLOD::FBoneLODLayout* CSkeletalMesh::GetBoneLODLayout(const FSkeletonResource* InSkeleton) const
    {
        const FMeshResource* Resource = TryGetMeshResource();
        if (InSkeleton == nullptr || Resource == nullptr || Resource->MeshletData.MeshletBoneIndices.IsEmpty())
        {
            return nullptr;
        }

        const uint64 LayoutKey = InSkeleton->BoneLayoutHash != 0 ? InSkeleton->BoneLayoutHash : InSkeleton->ComputeBoneLayoutHash();
        const int32 PaletteEntries = Resource->MeshletData.MeshletBoneIndices.Num();
        const CSkeleton* SkeletonAsset = Skeleton.Get();
        // Both generations only grow, so their sum changes whenever either set of sockets does.
        const uint32 SocketGeneration = GetSocketGeneration() + (SkeletonAsset != nullptr ? SkeletonAsset->GetSocketGeneration() : 0u);

        const FBoneLODLayoutEntry* Active = ActiveBoneLODLayout.load(std::memory_order_acquire);
        if (Active != nullptr && Active->Matches(LayoutKey, Resource, PaletteEntries, SocketGeneration))
        {
            return Active->Layout.get();
        }

        FScopeLock Lock(BoneLODMutex);
        for (const TUniquePtr<FBoneLODLayoutEntry>& Entry : BoneLODLayouts)
        {
            if (Entry->Matches(LayoutKey, Resource, PaletteEntries, SocketGeneration))
            {
                ActiveBoneLODLayout.store(Entry.get(), std::memory_order_release);
                return Entry->Layout.get();
            }
        }

        // Socket bones only count when the socket asset describes this same skeleton.
        TVector<int32> SocketBones;
        if (SkeletonAsset != nullptr && SkeletonAsset->GetSkeletonResource() == InSkeleton)
        {
            TVector<const SMeshSocket*> ActiveSockets;
            CollectActiveSockets(ActiveSockets);
            for (const SMeshSocket* Socket : ActiveSockets)
            {
                const int32 Bone = InSkeleton->FindBoneIndex(Socket->BoneName);
                if (Bone != Constants::kIndexNone)
                {
                    SocketBones.Add(Bone);
                }
            }
        }

        TUniquePtr<FBoneLODLayoutEntry> Entry = MakeUnique<FBoneLODLayoutEntry>();
        Entry->LayoutKey        = LayoutKey;
        Entry->Resource         = Resource;
        Entry->PaletteEntries   = PaletteEntries;
        Entry->SocketGeneration = SocketGeneration;
        Entry->Layout           = BoneLOD::BuildLayout(*Resource, *InSkeleton, SocketBones);

        const FBoneLODLayoutEntry* Published = Entry.get();
        BoneLODLayouts.Add(Move(Entry));
        ActiveBoneLODLayout.store(Published, std::memory_order_release);
        return Published->Layout.get();
    }

    void CSkeletalMesh::RemapJointIndicesToSkeleton()
    {
        if (Skeleton == SkeletonJointIndicesAddress || Skeleton == nullptr)
        {
            return;
        }

        const FSkeletonResource* From = SkeletonJointIndicesAddress != nullptr ? SkeletonJointIndicesAddress->GetSkeletonResource() : nullptr;
        const FSkeletonResource* To   = Skeleton->GetSkeletonResource();
        if (From == nullptr || To == nullptr)
        {
            SkeletonJointIndicesAddress = Skeleton;
            return;
        }

        // Bones are parents-before-children, so an unmatched bone inherits its parent's already-resolved target.
        TVector<uint32> FromToTo(From->GetNumBones(), 0u);
        uint32 MatchedBones = 0;
        FName FirstUnmatched;
        for (int32 i = 0; i < From->GetNumBones(); ++i)
        {
            const FSkeletonResource::FBoneInfo& Bone = From->GetBone(i);
            const int32 Target = To->FindBoneIndex(Bone.Name);
            if (Target >= 0)
            {
                FromToTo[i] = (uint32)Target;
                ++MatchedBones;
                continue;
            }

            FromToTo[i] = Bone.ParentIndex >= 0 ? FromToTo[Bone.ParentIndex] : 0u;
            if (FirstUnmatched.IsNone())
            {
                FirstUnmatched = Bone.Name;
            }
        }

        // Left addressing the old skeleton, so assigning it back is still lossless.
        if (MatchedBones == 0)
        {
            LOG_WARN("[SkeletalMesh] '{}': no bone of '{}' matches '{}' by name, joint indices left unchanged; it will skin to the wrong bones.",
                     GetName(), SkeletonJointIndicesAddress->GetName(), Skeleton->GetName());
            return;
        }

        if (MatchedBones < (uint32)From->GetNumBones())
        {
            LOG_WARN("[SkeletalMesh] '{}': {} bone(s) of '{}' missing from '{}' (first: '{}'); their vertices now follow the nearest matching parent.",
                     GetName(), From->GetNumBones() - (int32)MatchedBones, SkeletonJointIndicesAddress->GetName(),
                     Skeleton->GetName(), FirstUnmatched);
        }

        TVector<FMatrix4> FromBindPose;
        TVector<FMatrix4> ToBindPose;
        SkeletonJointIndicesAddress->ComputeBindPoseSkinningMatrices(FromBindPose);
        Skeleton->ComputeBindPoseSkinningMatrices(ToBindPose);

        // Remapped on a copy, so instances keep drawing the live geometry until the copy's upload lands.
        const FMeshResource& Current = GetNewestMeshResource();
        TUniquePtr<FMeshResource> Remapped = MakeUnique<FMeshResource>();
        Remapped->Name             = Current.Name;
        Remapped->bSkinnedMesh     = Current.bSkinnedMesh;
        Remapped->GeometrySurfaces = Current.GeometrySurfaces;
        Remapped->MeshletData      = Current.MeshletData;
        Remapped->DistanceField    = Current.DistanceField;

        float WorstBindPoseDelta = 0.0f;
        uint32 WorstBone = 0;
        for (uint32& GlobalBone : Remapped->MeshletData.MeshletBoneIndices)
        {
            const uint32 OldBone = GlobalBone;
            GlobalBone = OldBone < static_cast<uint32>(FromToTo.Num()) ? FromToTo[OldBone] : 0u;
            if (OldBone >= static_cast<uint32>(FromBindPose.Num()))
            {
                continue;
            }

            for (int32 Column = 0; Column < 4; ++Column)
            {
                for (int32 Row = 0; Row < 4; ++Row)
                {
                    const float Delta = Math::Abs(FromBindPose[OldBone][Column][Row] - ToBindPose[GlobalBone][Column][Row]);
                    if (Delta > WorstBindPoseDelta)
                    {
                        WorstBindPoseDelta = Delta;
                        WorstBone = OldBone;
                    }
                }
            }
        }

        // Remapping fixes bone order only; a different bind pose or import scale still deforms the mesh.
        if (WorstBindPoseDelta > 1e-3f)
        {
            LOG_WARN("[SkeletalMesh] '{}': bind pose of '{}' differs from '{}' by up to {} at bone '{}'; the mesh will deform. Import both from the same file at the same scale.",
                     GetName(), Skeleton->GetName(), SkeletonJointIndicesAddress->GetName(), WorstBindPoseDelta,
                     From->GetBone((int32)WorstBone).Name);
        }

        SkeletonJointIndicesAddress = Skeleton;
        SetMeshResource(Move(Remapped));
    }
}
