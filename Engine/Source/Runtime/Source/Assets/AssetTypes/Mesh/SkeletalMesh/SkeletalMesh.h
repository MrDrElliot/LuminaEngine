#pragma once

#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "SkeletalMesh.generated.h"

namespace Lumina
{
    class CSkeleton;

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

        // Rewrites the joint indices into Skeleton's bone order, matching by name, after Skeleton is reassigned.
        void RemapJointIndicesToSkeleton();

        PROPERTY(Editable, Category = "Skeleton")
        TStrongObjectPtr<CSkeleton> Skeleton;

        // What a runtime merge built this from, held so the parts stay loaded while anything wears it.
        TVector<TStrongObjectPtr<CSkeletalMesh>> MergeSources;

    private:

        TStrongObjectPtr<CSkeleton> SkeletonJointIndicesAddress;
    };
}
