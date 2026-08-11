#pragma once

#include "Texture.h"
#include "TextureRenderTarget.generated.h"

namespace Lumina
{
    REFLECT()
    enum class ERenderTargetFormat : uint8
    {
        // 8-bit RGBA UNORM. The default; fine for masks, blood/decal maps, splat data.
        RGBA8,

        // 16-bit float RGBA. Use for HDR accumulation or values outside [0,1].
        RGBA16F,
    };

    /** Writable Texture2D (UAV + SRV) sized from Width/Height/Format instead of cooked pixels; compute
     *  paints into it and it samples like any CTexture. Contents are runtime-only, rebuilt (cleared) on load. */
    REFLECT()
    class RUNTIME_API CTextureRenderTarget : public CTexture
    {
        GENERATED_BODY()

    public:

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;

        /** (Re)allocates the GPU image from the current Width/Height/Format and clears it to ClearColor. */
        void BuildResource();

        /**
         * Writes pixels into the target, resizes to InWidth x InHeight first if it is not already that shape.
         *
         * Pixels must match the target's Format: 4 bytes per texel for RGBA8, 8 for RGBA16F.
         */
        void Update(const void* Pixels, uint64 SizeBytes, uint32 InWidth, uint32 InHeight);

        /** Resolved RHI format for the friendly Format property. */
        EFormat GetRHIFormat() const;

        uint32 GetWidth() const  { return Width; }
        uint32 GetHeight() const { return Height; }

        PROPERTY(Editable)
        uint32 Width = 1024;

        PROPERTY(Editable)
        uint32 Height = 1024;

        PROPERTY(Editable)
        ERenderTargetFormat Format = ERenderTargetFormat::RGBA8;

        /** Color the target is cleared to on (re)build. */
        PROPERTY(Editable)
        FVector4 ClearColor = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
    };
}
