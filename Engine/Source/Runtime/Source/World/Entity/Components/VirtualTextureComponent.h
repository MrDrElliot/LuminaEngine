#pragma once

#include "Assets/AssetTypes/Textures/RuntimeVirtualTexture.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "VirtualTextureComponent.generated.h"

namespace Lumina
{
    // How a brush combines with what a virtual texture already holds, per channel. Max and Add leave a channel whose value is zero alone.
    REFLECT()
    enum class EVirtualTextureBlend : uint8
    {
        Max,
        Min,
        Add,
        Subtract,
        Replace,
    };

    // What a terrain writes into each texel of a virtual texture, matching VIRTUAL_TEXTURE_LAYOUT_* in VirtualTexture.slang.
    REFLECT()
    enum class EVirtualTextureLayout : uint8
    {
        // The material's base color in RGB and its roughness in A.
        BaseColorRoughness,

        // The ground's world normal packed into RGB and its height over the terrain's range in A, best in an RGBA16F texture.
        NormalHeight,

        // The material's base color in RGB and the height over the terrain's range in A.
        BaseColorHeight,
    };

    // One virtual texture a terrain draws its material into from above.
    REFLECT()
    struct RUNTIME_API SVirtualTextureOutput
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        TStrongObjectPtr<CRuntimeVirtualTexture> Texture;

        PROPERTY(Editable)
        EVirtualTextureLayout Layout = EVirtualTextureLayout::BaseColorRoughness;
    };

    // Paints a soft round brush into a runtime virtual texture where the entity goes, swept along its path so a fast mover leaves an unbroken trail.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SVirtualTextureWriterComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Virtual Texture")
        TStrongObjectPtr<CRuntimeVirtualTexture> Texture;

        PROPERTY(Editable, Category = "Virtual Texture")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Brush", ClampMin = 0.01f, Units = "m")
        float Radius = 0.5f;

        // The RGBA written at the brush center.
        PROPERTY(Editable, Category = "Brush")
        FVector4 Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

        PROPERTY(Editable, Category = "Brush")
        EVirtualTextureBlend Blend = EVirtualTextureBlend::Max;

        // How much of the radius stays at full strength before the edge fades.
        PROPERTY(Editable, Category = "Brush", ClampMin = 0.0f, ClampMax = 1.0f)
        float Hardness = 0.5f;

        // The strength of each write, and for Add and Subtract the amount per second the entity spends in one place.
        PROPERTY(Editable, Category = "Brush", ClampMin = 0.0f)
        float Opacity = 1.0f;

        // Added to the entity's position, such as down to the feet.
        PROPERTY(Editable, Category = "Brush", Units = "m")
        FVector3 Offset = FVector3(0.0f);
    };
}
