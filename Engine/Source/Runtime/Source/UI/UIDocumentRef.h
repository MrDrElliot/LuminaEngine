#pragma once

#include "Assets/AssetRef.h"
#include "UIDocumentRef.generated.h"

namespace Lumina
{
    // A rename-safe reference to an .rml document, which the details panel picks from the project's UI documents.
    REFLECT()
    struct RUNTIME_API FUIDocumentRef : public FAssetRef
    {
        GENERATED_BODY()

        FUIDocumentRef() = default;
        explicit FUIDocumentRef(FStringView InPath) : FAssetRef(InPath) {}
    };
}
