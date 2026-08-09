#pragma once
#include "Assets/Factories/Factory.h"
#include "Containers/String.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "TextureArrayFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CTextureArrayFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;
        CClass* GetAssetClass() const override { return CTextureArray::StaticClass(); }
        FString GetAssetName() const override { return "Texture Array"; }
        FString GetAssetDescription() const override
        {
            return "A Texture2DArray built from an ordered list of images. Sample it in a material with "
                   "TextureSampleArray, picking a layer with the Slice pin.";
        }
        FString GetCategory() const override { return "Textures"; }
        FStringView GetDefaultAssetCreationName() override { return "NewTextureArray"; }

        // Deliberately NOT an importer: layers are existing texture ASSETS, not files, so there is
        // nothing for TryImport to read. They are dropped onto the asset in the texture editor, which
        // then calls Rebuild below.

        /**
         * Assembles Array->SourceTextures into a single Tex2DArray, in list order, and republishes the
         * GPU resource.
         *
         * Layers are already cooked, so the normal path just COPIES each one's mip chain across -- no
         * re-encode, and the source textures keep owning their pixels and stay usable on their own.
         *
         * Layer 0 sets the contract (extent, mip count, format). A later layer that disagrees is
         * REJECTED by name, because one VkImage cannot hold slices of differing size or format. With
         * bResizeLayersToFirst it is instead re-cooked from that texture's original SourcePath at the
         * contract size; format mismatches are still refused, since no amount of rescaling reconciles
         * BC5 with BC7.
         *
         * Returns false and leaves the asset untouched if any layer fails, so a bad layer cannot leave
         * a half-built array behind.
         */
        static EDITOR_API bool Rebuild(CTextureArray* Array);
    };
}
