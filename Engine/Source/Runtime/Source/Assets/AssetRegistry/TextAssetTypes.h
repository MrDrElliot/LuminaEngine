#pragma once
#include "ModuleAPI.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Loose text assets with a stable GUID identity, stored as the raw value so a kind keeps its number once assigned.
    enum class ETextAssetKind : uint8
    {
        None          = 0,
        RmlDocument   = 2,  // .rml
        RmlStyleSheet = 3,  // .rcss
    };

    namespace TextAsset
    {
        // Lowercase extension with leading dot (".rml" / ".rcss") -> kind; None otherwise.
        RUNTIME_API ETextAssetKind KindFromExtension(FStringView Ext);
        RUNTIME_API ETextAssetKind KindFromPath(FStringView Path);

        // Canonical lowercase extension (with dot) for a kind; "" for None.
        RUNTIME_API FStringView ExtensionForKind(ETextAssetKind Kind);

        RUNTIME_API FStringView DisplayName(ETextAssetKind Kind);

        // True iff Path names one of the tracked text-asset extensions.
        RUNTIME_API bool IsTextAssetPath(FStringView Path);

        // Parses a PROPERTY AssetType meta value of extensions or kind names, where empty or "any" allows all kinds.
        RUNTIME_API TVector<ETextAssetKind> ParseAssetTypeMeta(FStringView Meta);
    }
}
