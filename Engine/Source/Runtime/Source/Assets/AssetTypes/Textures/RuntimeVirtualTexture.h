#pragma once

#include "TextureRenderTarget.h"
#include "RuntimeVirtualTexture.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EVirtualTexturePlacement : uint8
    {
        // A window of the world around the view that scrolls with it, losing what it leaves behind.
        FollowCamera,

        // One square of the world centered on FixedCenter, which keeps everything written inside it.
        Fixed,
    };

    // A render target laid over the world's XZ plane, which terrain materials, writer components and stamps fill and any material samples by world position.
    REFLECT()
    class RUNTIME_API CRuntimeVirtualTexture : public CTextureRenderTarget
    {
        GENERATED_BODY()

    public:

        void PostLoad() override;
        void PostPropertyChange(FProperty* ChangedProperty) override;

        uint32 GetResolution() const { return Width; }
        float  GetWorldSize() const  { return TexelSize * (float)Width; }

        // Meters per texel, so the texture reaches Width times this across the world.
        PROPERTY(Editable, Category = "Virtual Texture", ClampMin = 0.01f, Units = "m")
        float TexelSize = 0.25f;

        PROPERTY(Editable, Category = "Virtual Texture")
        EVirtualTexturePlacement Placement = EVirtualTexturePlacement::FollowCamera;

        // World X and Z at the middle of a Fixed texture.
        PROPERTY(Editable, Category = "Virtual Texture")
        FVector2 FixedCenter = FVector2(0.0f);

        // The share of the way written values fade back to ClearColor each second, so a trail lasts about one over this.
        PROPERTY(Editable, Category = "Virtual Texture", ClampMin = 0.0f)
        float DecayPerSecond = 0.0f;
    };
}
