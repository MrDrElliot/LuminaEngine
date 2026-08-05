#pragma once
#include "Assets/Factories/Factory.h"
#include "Containers/String.h"
#include "Assets/AssetTypes/Textures/Texture.h"
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

        bool IsExtensionSupported(FStringView Ext) override
        {
            return Ext == ".png" || Ext == ".jpg" || Ext == ".jpeg" || Ext == ".hdr";
        }
        void GetSupportedExtensions(TVector<FStringView>& OutExtensions) const override
        {
            OutExtensions.insert(OutExtensions.end(), { ".png", ".jpg", ".jpeg", ".hdr" });
        }
        bool CanImport() override { return true; }

        void TryImport(const FFixedString& RawPath, const FFixedString& DestinationPath, const Import::FImportSettings* Settings) override;

        bool CanReimport(const CStruct* AssetClass) const override;
        bool TryReimport(CObject* Asset, const FFixedString& SourceFile, const Import::FImportSettings* Settings) override;
        FString GetReimportSourcePath(const CObject* Asset) const override;

        /** Re-runs Basis compression on Texture->SourcePath; false if path is missing or asset is mesh-embedded. */
        static EDITOR_API bool Recook(CTexture* Texture);

        /** Creates a 4x4 solid-color CTexture asset at Path, cooked via the normal Basis path. Used to mint the
         *  neutral material-import defaults (white for color/MR/AO/emissive, 128,128,255 for flat normals).
         *  Returns the created object (NOT saved); the caller saves + registers it. */
        static EDITOR_API CTexture* CreateSolidColorTexture(FStringView Path, uint8 R, uint8 G, uint8 B, uint8 A, ETextureColorSpace ColorSpace);

    private:

    };
}
