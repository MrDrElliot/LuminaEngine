#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class CClass;
    class IEditorToolContext;
}

namespace Lumina
{
    // One asset whose inbound references are being retargeted, together with the assets that point at it.
    struct FAssetReferenceFixup
    {
        FGuid           AssetGUID;
        FFixedString    AssetPath;
        FName           AssetName;
        CClass*         AssetClass = nullptr;

        TVector<FGuid>  Referencers;

        // Invalid means every reference is cleared instead of retargeted.
        FGuid           ReplacementGUID;
    };

    enum class EReferenceFixupMode : uint8
    {
        BeforeDelete,
        Standalone,
    };

    namespace ReplaceReferences
    {
        // Excludes edges between the listed assets, which need no fixup when the whole set is going away.
        TVector<FAssetReferenceFixup> BuildPlan(const TVector<FFixedString>& Paths);

        // OnResolved receives true only when a BeforeDelete run should go on to delete.
        void OpenModal(IEditorToolContext* Context, TVector<FAssetReferenceFixup> Plan, EReferenceFixupMode Mode,
                       uint32 TargetCount, TFunction<void(bool)> OnResolved);
    }
}
