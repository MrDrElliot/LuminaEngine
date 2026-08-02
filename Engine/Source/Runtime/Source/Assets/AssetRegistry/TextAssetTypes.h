#pragma once
#include "ModuleAPI.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Loose text assets that get a stable GUID identity (so references survive rename/move).
    enum class ETextAssetKind : uint8
    {
        None = 0,
        LuaScript,      // .luau
        RmlDocument,    // .rml
        RmlStyleSheet,  // .rcss
    };

    namespace TextAsset
    {
        // Lowercase extension with leading dot (".luau" / ".rml" / ".rcss") -> kind; None otherwise.
        RUNTIME_API ETextAssetKind KindFromExtension(FStringView Ext);
        RUNTIME_API ETextAssetKind KindFromPath(FStringView Path);

        // Canonical lowercase extension (with dot) for a kind; "" for None.
        RUNTIME_API FStringView ExtensionForKind(ETextAssetKind Kind);

        RUNTIME_API FStringView DisplayName(ETextAssetKind Kind);

        // True iff Path names one of the tracked text-asset extensions.
        RUNTIME_API bool IsTextAssetPath(FStringView Path);

        // Parse a PROPERTY(AssetType="luau,rml") meta value into the kinds it allows.
        // Tokens may be extensions (with or without dot) or kind names; empty/"any" -> all kinds.
        RUNTIME_API TVector<ETextAssetKind> ParseAssetTypeMeta(FStringView Meta);
    }
}
