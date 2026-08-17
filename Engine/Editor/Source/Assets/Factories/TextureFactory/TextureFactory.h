#pragma once
#include "Assets/Factories/Factory.h"
#include "Containers/String.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Tools/Import/ImportHelpers.h"
#include "TextureFactory.generated.h"


namespace Lumina
{
    REFLECT()
    class CTextureFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;
        CClass* GetAssetClass() const override { return CTexture::StaticClass(); }
        FString GetAssetName() const override { return "Texture"; }
        FStringView GetDefaultAssetCreationName() override { return "NewTexture"; }

        /**
         * Cooks one source (embedded bytes or a file) into Texture: picks the DDS passthrough, HDR float,
         * or Basis path, resolves Auto color spaces, and writes the package thumbnail. Does not create,
         * save or register anything -- the caller owns the asset's lifetime.
         */
        static EDITOR_API bool CookIntoTexture(CTexture* Texture, const Import::Textures::FTextureCookRequest& Request);

        /** Filename-suffix heuristic used to resolve ETextureColorSpace::Auto. */
        static EDITOR_API ETextureColorSpace ClassifyColorSpaceByFilename(FStringView Path);

        /** Re-cooks with the current settings, from SourcePath if it resolves and from RecoverSourceImage if not. */
        static EDITOR_API bool Recook(CTexture* Texture);

        /** Decodes the cooked chain's mip 0 back into an editable image. False if its format has no decoder. */
        static EDITOR_API bool RecoverSourceImage(CTexture* Texture, Import::Textures::FTextureImportResult& OutResult);

        /** Runs the cook over an in-memory source. Source is consumed: it is prepared and moved from. */
        static EDITOR_API bool CookFromSource(CTexture* Texture, Import::Textures::FTextureImportResult& Source);

        /**
         * Cooks one image file into Scratch->TextureResource's CPU mip chain and stops there -- no GPU
         * image is created. For callers assembling several cooks into a single resource, which is what
         * CTextureArrayFactory does per layer. Returns false (and logs) if the file cannot be loaded or
         * its pixel layout is not cookable.
         */
        static EDITOR_API bool CookLayerFromFile(CTexture* Scratch, FStringView SourcePath, ETextureColorSpace ColorSpace,
                                                 FUIntVector2 TargetSize = {});

        /** Creates a 4x4 solid-color CTexture asset at Path, cooked via the normal Basis path. Used to mint the
         *  neutral material-import defaults (white for color/MR/AO/emissive, 128,128,255 for flat normals).
         *  Returns the created object (NOT saved); the caller saves + registers it. */
        static EDITOR_API CTexture* CreateSolidColorTexture(FStringView Path, uint8 R, uint8 G, uint8 B, uint8 A, ETextureColorSpace ColorSpace);

    private:

    };
}
