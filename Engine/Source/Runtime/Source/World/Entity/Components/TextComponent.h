#pragma once

#include "Assets/AssetTypes/Font/Font.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "TextComponent.generated.h"

namespace Lumina
{
    class CFont;

    REFLECT()
    enum class ETextHorizontalAlign : uint8
    {
        Left,
        Center,
        Right,
    };

    REFLECT()
    enum class ETextVerticalAlign : uint8
    {
        Top,
        Middle,
        Bottom,
    };

    // Render-thread cache of the shaped (em-space) glyph layout for one text component. ShapeText is the
    // expensive part of world-text extraction (UTF-8 decode + per-glyph atlas lookups + allocation), and the
    // layout depends only on the text, font, alignment, and line spacing -- not on color/size/transform --
    // so the extractor reshapes only when one of those changes. Not serialized; rebuilt on demand.
    struct FTextRenderCache
    {
        TVector<FShapedGlyph> Glyphs;
        float                 EmExtent = 0.0f;   // max |x|,|y| over Glyphs; sizes the cull bounding sphere

        // Signature of the inputs that affect layout. A mismatch triggers a reshape.
        uint64                TextHash    = 0;
        uint32                TextLength  = 0;
        const CFont*          Font        = nullptr;
        uint32                FontVersion = 0;
        ETextHorizontalAlign  HAlign      = ETextHorizontalAlign::Center;
        ETextVerticalAlign    VAlign      = ETextVerticalAlign::Middle;
        float                 LineSpacing = 1.0f;
        bool                  bValid      = false;
    };

    // Renders a string as crisp MSDF glyph quads in the world. The font's baked atlas (see FontFactory)
    // is sampled per-glyph; WorldSize is the world height of one em, so text scales without blurring.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API STextComponent
    {
        GENERATED_BODY()

        /** Font asset to draw with. Must have a baked MSDF atlas (re-import older fonts to bake one). */
        PROPERTY(Editable, Category = "Text")
        TObjectPtr<CFont> Font;

        /** Text to display. Supports newlines (wraps to multiple lines); UTF-8. */
        PROPERTY(Editable, Category = "Text", Multiline)
        FString Text = "Text";

        /** World height of one em (the overall text scale), in meters. */
        PROPERTY(Editable, Category = "Text", ClampMin = 0.0001f, Units = "m")
        float WorldSize = 1.0f;

        /** RGBA color the glyphs are tinted with. */
        PROPERTY(Editable, Color, Category = "Text")
        FVector4 Color = FVector4(1.0f);

        PROPERTY(Editable, Category = "Text")
        ETextHorizontalAlign HorizontalAlign = ETextHorizontalAlign::Center;

        PROPERTY(Editable, Category = "Text")
        ETextVerticalAlign VerticalAlign = ETextVerticalAlign::Middle;

        /** Multiplier on the font's line height for multi-line text. */
        PROPERTY(Editable, Category = "Text", ClampMin = 0.1f)
        float LineSpacing = 1.0f;

        /** When true the text always faces the camera; otherwise it lies in the entity's local XY plane. */
        PROPERTY(Editable, Category = "Text")
        bool bBillboard = false;

        /** When true the text is depth-tested against the scene (occluded by closer geometry) and writes
         *  depth; otherwise it always draws on top of everything. */
        PROPERTY(Editable, Category = "Text")
        bool bDepthTest = true;

        //~ Script setters (reflected fields aren't directly assignable from Lua). Font is optional -- a null
        //  font falls back to the engine default world-text font (CFontManager).

        FUNCTION(Script)
        void SetText(const FString& InText) { Text = InText; }

        FUNCTION(Script)
        void SetWorldSize(float InSize) { WorldSize = InSize; }

        FUNCTION(Script)
        void SetColor(const FVector4& InColor) { Color = InColor; }

        FUNCTION(Script)
        void SetBillboard(bool bInBillboard) { bBillboard = bInBillboard; }

        // Render-thread shaped-glyph cache; not serialized, not edited. Rebuilt by the text extractor when
        // the text/font/alignment/spacing changes (see FTextRenderCache).
        FTextRenderCache RenderCache;
    };
}
