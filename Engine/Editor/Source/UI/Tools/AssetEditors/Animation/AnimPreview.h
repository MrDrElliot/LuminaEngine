#pragma once

#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "GUID/GUID.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CClass;
    class CSkeletalMesh;
    class CSkeleton;
    struct FAssetData;
    struct FSkeletonResource;

    namespace AnimPreview
    {
        bool SharesBoneNames(const FSkeletonResource& A, const FSkeletonResource& B);

        bool AreSkeletonsCompatible(const CSkeleton& A, const CSkeleton& B);

        // Matches on the asset's skeleton import, so a candidate is judged without loading it.
        bool IsAssetCompatible(const FAssetData& Data, const CSkeleton& Skeleton, THashMap<FGuid, bool>& SkeletonVerdicts);
    }

    class FCompatibleAssetList
    {
    public:

        struct FEntry
        {
            FGuid   GUID;
            FString Name;
        };

        explicit FCompatibleAssetList(CClass* InAssetClass);

        // Assets of the class whose skeleton matches by bones, sorted by name.
        const TVector<FEntry>& Get(const CSkeleton* Skeleton);

    private:

        CClass*                  AssetClass = nullptr;
        const CSkeleton*         CachedSkeleton = nullptr;
        TVector<FEntry>          Entries;
        FAssetRegistryStaleLatch StaleLatch;
    };

    // A per-editor preview mesh that falls back to the skeleton's own, and is never saved with the asset.
    class FPreviewMeshSelection
    {
    public:

        FPreviewMeshSelection();

        CSkeletalMesh* Resolve(const CSkeleton* Skeleton) const;

        // Returns true when the resolved preview mesh changed.
        bool DrawMenu(CSkeleton* Skeleton);

    private:

        TStrongObjectPtr<CSkeletalMesh> Override;
        FCompatibleAssetList            Meshes;
    };
}
