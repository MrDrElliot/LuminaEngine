#pragma once

#include "Texture.h"
#include "Containers/Array.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "TextureArray.generated.h"

namespace Lumina
{
    /**
     * Texture2DArray built from an ordered list of source images, cooked into a single GPU resource.
     *
     * Derives from CTexture on purpose rather than standing alone: the material texture binding path
     * (FMaterialCompiler::BindTexture, CMaterial::Textures, GetMaterialTexture) is typed on CTexture
     * and is entirely format-agnostic below that, so inheriting makes an array bind and parameterize
     * through the exact same code a plain texture does. The only thing that differs is the view type
     * chosen at PostLoad, which is what lets a shader read the ResourceID as gTextures2DArray[].
     *
     * Every layer MUST share extent, mip count and format -- one VkImage cannot do otherwise. The
     * importer enforces this by cooking layer 0 first and rejecting any later source that disagrees,
     * so the failure is an import error naming the offending file rather than a device loss.
     */
    REFLECT()
    class RUNTIME_API CTextureArray : public CTexture
    {
        GENERATED_BODY()

    public:

        /** Number of array slices; layer i is sampled with Slice = i. */
        uint32 GetNumLayers() const { return TextureResource ? TextureResource->GetNumLayers() : 0u; }

        /**
         * The texture asset backing each layer, in slice order: SourceTextures[i] becomes slice i.
         *
         * Asset references rather than file paths on purpose. A path is absolute and machine-local, so
         * it stops resolving the moment anyone else opens the project or the file moves; a reference
         * survives both, and it puts the array into the dependency graph so the cook packages its
         * layers. It also means each layer already went through the normal texture import, so color
         * space, texture group and reimport all behave exactly as they do for a standalone texture.
         *
         * The pixel data is COPIED into this asset's own TextureResource at build time -- the array
         * does not read these at runtime, and a packaged build never needs them resident.
         */
        PROPERTY(Editable, Category = "Layers")
        TVector<TObjectPtr<CTexture>> SourceTextures;

        /**
         * Resample mismatched layers to layer 0's dimensions instead of refusing them.
         *
         * Off by default: it is a plain stretch, so a source with a different aspect ratio comes out
         * distorted, and doing that silently to someone's art is worse than making them notice.
         *
         * Note this is not free the way a matching layer is. Matching layers are copied straight out of
         * their already-cooked mip chains; a resized one has to be re-cooked from the texture's original
         * SourcePath, because rescaling cooked BCn blocks would mean decode, resample and re-encode.
         * A layer whose SourcePath is gone (a mesh-embedded texture) therefore cannot be resized.
         */
        PROPERTY(Editable, Category = "Layers")
        bool bResizeLayersToFirst = false;
    };
}
