#pragma once

#include "Tools/Import/Importer.h"
#include "Tools/Import/ImportHelpers.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "TextureImporter.generated.h"

namespace Lumina
{
    namespace Import::Textures
    {
        // Creates and cooks a texture but leaves saving and registering to the caller, so a batch import saves in one pass.
        EDITOR_API CTexture* ImportTextureAsset(const FFixedString& PackagePath, const FTextureCookRequest& Request, ETextureGroup Group = ETextureGroup::World);
    }

    REFLECT()
    class EDITOR_API CTextureImporter : public CImporter
    {
        GENERATED_BODY()
    public:

        // Storage interpretation, where Auto resolves from the filename suffix at import time.
        PROPERTY(Editable, Category = "Texture")
        ETextureColorSpace ColorSpace = ETextureColorSpace::Auto;

        // Mip policy, filtering and streaming behavior for this texture.
        PROPERTY(Editable, Category = "Texture")
        ETextureGroup Group = ETextureGroup::World;

        FStringView GetImporterDisplayName() const override { return "Texture"; }

        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.Append({ ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".dds" });
        }

        void BuildAssets(const FImportRequest& Request, FImportResult& OutResult, FScopedSlowTask* Progress) override;

        bool CanReimport(const CStruct* AssetClass) const override;
        bool ReimportAsset(CObject* Asset, const FImportRequest& Request, FScopedSlowTask* Progress) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;
    };
}
