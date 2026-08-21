#include "RuntimePCH.h"
#include "FontAtlasBaker.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Font.h"

#include <cmath>

#include <msdfgen/msdfgen.h>
#include <msdfgen/ext/import-font.h>

namespace Lumina
{
    bool BakeFontAtlas(CFont* Font)
    {
        using namespace msdfgen;

        if (Font == nullptr || Font->FontData.empty())
        {
            return false;
        }

        FreetypeHandle* FT = initializeFreetype();
        if (FT == nullptr)
        {
            return false;
        }

        FontHandle* MFont = loadFontData(FT, Font->FontData.data(), (int)Font->FontData.size());
        if (MFont == nullptr)
        {
            deinitializeFreetype(FT);
            return false;
        }

        FontMetrics Metrics{};
        getFontMetrics(Metrics, MFont, FONT_SCALING_EM_NORMALIZED);

        constexpr double EmPixelSize = 48.0;        // atlas pixels per em
        constexpr double PxRange     = 4.0;         // MSDF distance range, in atlas pixels
        const double     RangeEm     = PxRange / EmPixelSize;
        const double     PadEm       = RangeEm;     // tile border so the field never clips
        constexpr int    GlyphPad    = 1;           // gutter between atlas tiles (avoids bilinear bleed)

        struct FBaked
        {
            uint32           CP      = 0;
            float            Advance = 0.0f;
            bool             HasInk  = false;
            double           PL = 0, PB = 0, PR = 0, PT = 0;   // expanded plane bounds (em)
            int              W = 0, H = 0;
            Bitmap<float, 4> Bmp;
        };
        TVector<FBaked> Baked;

        auto AddRange = [&](uint32 First, uint32 Last)
        {
            for (uint32 CP = First; CP <= Last; ++CP)
            {
                Shape Glyph;
                double Advance = 0.0;
                if (!loadGlyph(Glyph, MFont, (unicode_t)CP, FONT_SCALING_EM_NORMALIZED, &Advance))
                {
                    continue;   // font has no glyph for this code point
                }

                FBaked B;
                B.CP      = CP;
                B.Advance = (float)Advance;

                Glyph.normalize();
                const Shape::Bounds Bounds = Glyph.getBounds();

                // Whitespace / degenerate shapes carry an advance but no tile.
                if (Glyph.contours.empty() || !(Bounds.r > Bounds.l && Bounds.t > Bounds.b))
                {
                    B.HasInk = false;
                    Baked.push_back(Move(B));
                    continue;
                }

                edgeColoringSimple(Glyph, 3.0);

                B.PL = Bounds.l - PadEm;
                B.PB = Bounds.b - PadEm;
                B.PR = Bounds.r + PadEm;
                B.PT = Bounds.t + PadEm;
                B.W  = Math::Max((int)std::ceil((B.PR - B.PL) * EmPixelSize), 1);
                B.H  = Math::Max((int)std::ceil((B.PT - B.PB) * EmPixelSize), 1);
                B.HasInk = true;

                B.Bmp = Bitmap<float, 4>(B.W, B.H);

                const Projection Proj(Vector2(EmPixelSize), Vector2(-B.PL, -B.PB));
                generateMTSDF(B.Bmp, Glyph, Proj, Range(RangeEm), MSDFGeneratorConfig(true /*overlapSupport*/));

                // Rasterizes with the non-zero fill rule and flips disagreeing texels, fixing bowl-stem holes.
                distanceSignCorrection(B.Bmp, Glyph, Proj);

                Baked.push_back(Move(B));
            }
        };

        AddRange(0x20, 0x7E);   // ASCII printable
        AddRange(0xA0, 0xFF);   // Latin-1 supplement

        destroyFont(MFont);
        deinitializeFreetype(FT);

        if (Baked.empty())
        {
            return false;
        }

        // Shelf-pack the inked tiles into a fixed-width atlas, growing the height as rows fill.
        constexpr int AtlasW = 1024;
        int PenX = GlyphPad, PenY = GlyphPad, RowH = 0;

        struct FPlaced { int X = 0, Y = 0; };
        TVector<FPlaced> Placed; Placed.resize(Baked.size());

        for (size_t i = 0; i < Baked.size(); ++i)
        {
            if (!Baked[i].HasInk)
            {
                continue;
            }
            if (PenX + Baked[i].W + GlyphPad > AtlasW)
            {
                PenX = GlyphPad;
                PenY += RowH + GlyphPad;
                RowH = 0;
            }
            Placed[i] = { PenX, PenY };
            PenX += Baked[i].W + GlyphPad;
            RowH = Math::Max(RowH, Baked[i].H);
        }
        int AtlasH = (PenY + RowH + GlyphPad + 3) & ~3;   // round up to 4 rows

        // Zeroed, so the cleared border reads as fully outside once resolved.
        TVector<uint8> Pixels;
        Pixels.resize((size_t)AtlasW * AtlasH * 4, 0);

        auto ToByte = [](float V) -> uint8
        {
            return (uint8)Math::Clamp((int)(V * 255.0f + 0.5f), 0, 255);
        };

        TVector<FFontGlyph> Glyphs;
        Glyphs.reserve(Baked.size());

        for (size_t i = 0; i < Baked.size(); ++i)
        {
            const FBaked& B = Baked[i];

            FFontGlyph G;
            G.Codepoint = B.CP;
            G.Advance   = B.Advance;

            if (B.HasInk)
            {
                const int GX = Placed[i].X;
                const int GY = Placed[i].Y;

                // msdfgen bitmaps are Y-up while the atlas is top-down, so the blit flips.
                for (int y = 0; y < B.H; ++y)
                {
                    const float* Src = B.Bmp(0, B.H - 1 - y);
                    uint8*       Dst = &Pixels[((size_t)(GY + y) * AtlasW + GX) * 4];
                    for (int x = 0; x < B.W; ++x)
                    {
                        Dst[x * 4 + 0] = ToByte(Src[x * 4 + 0]);
                        Dst[x * 4 + 1] = ToByte(Src[x * 4 + 1]);
                        Dst[x * 4 + 2] = ToByte(Src[x * 4 + 2]);
                        Dst[x * 4 + 3] = ToByte(Src[x * 4 + 3]);
                    }
                }

                G.AtlasUV = FVector4((float)GX / AtlasW,         (float)GY / AtlasH,
                                     (float)(GX + B.W) / AtlasW, (float)(GY + B.H) / AtlasH);
                G.Plane   = FVector4((float)B.PL, (float)B.PB, (float)B.PR, (float)B.PT);
            }

            Glyphs.push_back(G);
        }

        Font->Glyphs        = Move(Glyphs);
        Font->AtlasPixels   = Move(Pixels);
        Font->AtlasWidth    = (uint32)AtlasW;
        Font->AtlasHeight   = (uint32)AtlasH;
        Font->DistanceRange = (float)PxRange;
        Font->LineHeight    = (float)Metrics.lineHeight;
        Font->Ascender      = (float)Metrics.ascenderY;
        Font->Descender     = (float)Metrics.descenderY;

        // An in-memory bake never hits PostLoad, so FindGlyph would find nothing without this.
        Font->BuildGlyphLookup();

        return true;
    }
}
