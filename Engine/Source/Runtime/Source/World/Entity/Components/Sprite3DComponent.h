#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "Sprite3DComponent.generated.h"

namespace Lumina
{
    class CTexture;

    REFLECT()
    enum class ESpriteBillboardMode : uint8
    {
        /** The quad uses the entity's own orientation. */
        Disabled,
        /** The quad always faces the camera. */
        Enabled,
        /** The quad turns around world up only, so it stays upright. */
        YBillboard,
    };

    REFLECT()
    enum class ESpriteAlphaCut : uint8
    {
        /** Straight alpha blending. */
        Disabled,
        /** Fragments below the threshold are discarded instead of blended. */
        Discard,
    };

    /** Draws a texture as an unlit quad in the world, optionally sliced into a sprite sheet. */
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SSprite3DComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Sprite")
        TObjectPtr<CTexture> Texture;

        /** World units per texture pixel, so the quad measures frame size times this. */
        PROPERTY(Editable, Category = "Sprite", ClampMin = 0.0001f)
        float PixelSize = 0.01f;

        PROPERTY(Editable, Category = "Sprite", Color)
        FVector4 Modulate = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

        /** Centers the quad on the entity origin rather than hanging it below and right. */
        PROPERTY(Editable, Category = "Sprite")
        bool bCentered = true;

        /** Shifts the quad in texture pixels, applied after centering. */
        PROPERTY(Editable, Category = "Sprite")
        FVector2 Offset = FVector2(0.0f, 0.0f);

        PROPERTY(Editable, Category = "Sprite")
        bool bFlipH = false;

        PROPERTY(Editable, Category = "Sprite")
        bool bFlipV = false;

        /** Columns in the sprite sheet. 1x1 uses the whole texture. */
        PROPERTY(Editable, Category = "Sprite Sheet", ClampMin = 1)
        int32 HFrames = 1;

        PROPERTY(Editable, Category = "Sprite Sheet", ClampMin = 1)
        int32 VFrames = 1;

        /** Cell to display, counted left to right then top to bottom. */
        PROPERTY(Editable, Category = "Sprite Sheet", ClampMin = 0)
        int32 Frame = 0;

        /** Replaces the frame grid with one explicit rect. */
        PROPERTY(Editable, Category = "Sprite Sheet")
        bool bRegionEnabled = false;

        /** x, y, width, height in texture pixels. */
        PROPERTY(Editable, Category = "Sprite Sheet", EditCondition = "bRegionEnabled")
        FVector4 RegionRect = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

        PROPERTY(Editable, Category = "Rendering")
        ESpriteBillboardMode BillboardMode = ESpriteBillboardMode::Disabled;

        PROPERTY(Editable, Category = "Rendering")
        ESpriteAlphaCut AlphaCut = ESpriteAlphaCut::Disabled;

        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f, ClampMax = 1.0f)
        float AlphaCutThreshold = 0.5f;

        /** Off draws through geometry, for markers that must stay visible. */
        PROPERTY(Editable, Category = "Rendering")
        bool bDepthTest = true;

        PROPERTY(Editable, Category = "Rendering")
        bool bDoubleSided = true;

        /** Higher draws later; ties break back to front. */
        PROPERTY(Editable, Category = "Rendering")
        int32 SortOrder = 0;
    };
}
