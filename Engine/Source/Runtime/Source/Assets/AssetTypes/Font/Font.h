#pragma once

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/Object.h"
#include "Renderer/RenderResource.h"
#include "Renderer/RHITexture.h"
#include "Font.generated.h"

namespace Lumina
{
    class FArchive;

    // One baked glyph with normalized atlas UVs (v0 is the top row) and em-normalized plane bounds and advance.
    struct FFontGlyph
    {
        uint32   Codepoint = 0;
        FVector4 AtlasUV   = FVector4(0.0f);   // u0, v0, u1, v1  (xy = top-left, zw = bottom-right)
        FVector4 Plane     = FVector4(0.0f);   // left, bottom, right, top of the quad vs the pen origin
        float    Advance   = 0.0f;             // horizontal pen advance
    };

    inline FArchive& operator<<(FArchive& Ar, FFontGlyph& G)
    {
        Ar << G.Codepoint << G.AtlasUV << G.Plane << G.Advance;
        return Ar;
    }

    // One em-space quad produced by ShapeText: Min/Max in em units (baseline at y=0, +y up), UV into the atlas.
    struct FShapedGlyph
    {
        FVector2 Min;
        FVector2 Max;
        FVector4 UV;     // u0, v0, u1, v1
    };

    // Self-contained font asset with the TTF/OTF bytes embedded and an MSDF atlas baked at import.
    REFLECT()
    class RUNTIME_API CFont : public CObject
    {
        GENERATED_BODY()

    public:

        void Serialize(FArchive& Ar) override;
        void PostLoad() override;
        void OnDestroy() override;
        bool IsAsset() const override { return true; }

        const TVector<uint8>& GetFontData() const { return FontData; }
        bool IsValid() const { return !FontData.IsEmpty(); }

        // True once a usable MSDF atlas is baked (pixels + glyph table present).
        bool HasAtlas() const { return !AtlasPixels.IsEmpty() && AtlasWidth > 0 && AtlasHeight > 0 && !Glyphs.IsEmpty(); }

        // Lazily uploads AtlasPixels to the texture heap and returns its ResourceID, or -1 if no atlas is baked.
        int32 GetAtlasResourceID();

        uint32 GetAtlasWidth()  const { return AtlasWidth; }
        uint32 GetAtlasHeight() const { return AtlasHeight; }
        float  GetDistanceRange() const { return DistanceRange; }

        // Bumped whenever the glyph table is (re)built, so cached text layouts re-shape after a re-bake.
        uint32 GetShapeVersion() const { return ShapeVersion; }
        float  GetLineHeight()  const { return LineHeight; }
        float  GetAscender()    const { return Ascender; }
        float  GetDescender()   const { return Descender; }

        const FFontGlyph* FindGlyph(uint32 Codepoint) const;

        // Rebuilds the glyph lookup, which the baker must call itself because the default font never loads.
        void BuildGlyphLookup();

        // Lays out a UTF-8 string into em-space quads with HAlign and VAlign in [0,1], and returns false if no atlas is baked.
        bool ShapeText(const FString& Text, float HAlign, float VAlign, float LineSpacing, TVector<FShapedGlyph>& Out) const;

        // Source file the face was imported from; empty for in-place created assets.
        PROPERTY(Hidden)
        FString SourcePath;

        PROPERTY(Hidden)
        FString FamilyName;

        PROPERTY(Hidden)
        FString StyleName;

        PROPERTY(Hidden)
        int32 NumGlyphs = 0;

        PROPERTY(Hidden)
        bool bIsScalable = false;

        PROPERTY(Hidden)
        bool bHasKerning = false;

        TVector<uint8> FontData;

        TVector<FFontGlyph> Glyphs; 
        TVector<uint8>      AtlasPixels;
        uint32              AtlasWidth   = 0;
        uint32              AtlasHeight  = 0;
        float               DistanceRange = 0.0f; // px distance range used at bake (drives shader AA)
        float               LineHeight   = 0.0f;  // em-normalized
        float               Ascender     = 0.0f;  // em-normalized
        float               Descender    = 0.0f;  // em-normalized (negative below baseline)

    private:

        THashMap<uint32, int32> GlyphLookup;
        RHI::FManagedTexture    AtlasTexture;

        // Monotonic; incremented by BuildGlyphLookup. Consumers compare it to detect a re-baked glyph table.
        uint32                  ShapeVersion = 0;
    };
}
