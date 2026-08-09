#pragma once

#include "Tools/Import/Importer.h"
#include "Tools/Import/ImportHelpers.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "TextureImporter.generated.h"

namespace Lumina
{
    namespace Import::Textures
    {
        /**
         * Creates a CTexture at PackagePath (extension already appended) and cooks Request into it. The
         * asset is NOT saved or registered: the caller owns it, so a batch import can save everything in
         * one pass instead of writing and re-reading each texture. Null on failure.
         */
        EDITOR_API CTexture* ImportTextureAsset(const FFixedString& PackagePath, const FTextureCookRequest& Request);
    }

    REFLECT()
    class EDITOR_API CTextureImporter : public CImporter
    {
        GENERATED_BODY()
    public:

        /** Storage interpretation. Auto resolves from the filename suffix at import time. */
        PROPERTY(Editable, Category = "Texture")
        ETextureColorSpace ColorSpace = ETextureColorSpace::Auto;

        /** Mip policy, filtering and streaming behavior for this texture. */
        PROPERTY(Editable, Category = "Texture")
        ETextureGroup Group = ETextureGroup::World;

        FStringView GetImporterDisplayName() const override { return "Texture"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds" });
        }

        void BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress) override;

        bool CanReimport(const CStruct* AssetClass) const override;
        bool ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;
    };
}
