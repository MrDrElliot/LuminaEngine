#pragma once

#include "Core/Object/Object.h"
#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "Renderer/MeshData.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Renderer/SkeletonResource.h"
#include "Skeleton.generated.h"

namespace Lumina
{
    class CSkeletalMesh;

    REFLECT()
    class RUNTIME_API CSkeleton : public CObject
    {
        friend class CMeshImporter;

        GENERATED_BODY()

    public:
        void Serialize(FArchive& Ar) override;

        bool IsAsset() const override { return true; }

        FSkeletonResource* GetSkeletonResource() const { return SkeletonResource.get(); }

        void SetSkeletonResource(TUniquePtr<FSkeletonResource>&& NewResource);

        void ComputeBindPoseSkinningMatrices(TVector<FMatrix4>& OutMatrices) const;

        const FMeshSocket* FindSocket(const FName& SocketName) const { return FindSocketByName(Sockets, SocketName); }

        /** Bone index the named socket rides on; INDEX_NONE when the socket or its bone doesn't exist. */
        int32 FindSocketBoneIndex(const FName& SocketName) const;

        PROPERTY(Editable, Category = "Preview")
        TObjectPtr<CSkeletalMesh> PreviewMesh;

        /**
         * Bones animated when a mesh is far away (skeleton LOD). Bones are stored parents-first, so
         * the first N form a valid partial hierarchy; the rest hold their bind-pose local transform
         * (attachments still follow their animated ancestors). 0 (default) disables the cut and
         * animates every bone; author a count only after checking the skeleton's bone order puts
         * detail bones (fingers, twists, face) last.
         */
        PROPERTY(Editable, Category = "LOD")
        int32 LowDetailBoneCount = 0;

        /** Named attach points riding skeleton bones. */
        PROPERTY(Editable, Category = "Sockets")
        TVector<FMeshSocket> Sockets;

    private:

        TUniquePtr<FSkeletonResource> SkeletonResource;
    };
}
