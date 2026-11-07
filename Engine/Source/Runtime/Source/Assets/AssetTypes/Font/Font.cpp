#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "Font.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Archiver.h"

namespace Lumina
{
    void CFont::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Fonts");
        Super::Serialize(Ar);

        Ar << FontData;
        Ar << AtlasWidth;
        Ar << AtlasHeight;
        Ar << DistanceRange;
        Ar << LineHeight;
        Ar << Ascender;
        Ar << Descender;
        Ar << Glyphs;
        Ar << AtlasPixels;
    }

    void CFont::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Fonts");
        BuildGlyphLookup();

        GetAtlasResourceID();
    }

    void CFont::OnDestroy()
    {
        RHI::Textures::Release(AtlasTexture);
        Super::OnDestroy();
    }

    void CFont::BuildGlyphLookup()
    {
        GlyphLookup.Reset();
        GlyphLookup.Reserve(Glyphs.Num());
        for (int32 i = 0; i < Glyphs.Num(); ++i)
        {
            GlyphLookup.TryAdd(Glyphs[i].Codepoint, i);
        }
        ++ShapeVersion;
    }

    const FFontGlyph* CFont::FindGlyph(uint32 Codepoint) const
    {
        auto It = GlyphLookup.Find(Codepoint);
        if (It == nullptr)
        {
            return nullptr;
        }
        return &Glyphs[(*It)];
    }

    int32 CFont::GetAtlasResourceID()
    {
        if (AtlasTexture.IsValid())
        {
            return (int32)AtlasTexture.ResourceID();
        }

        if (!HasAtlas())
        {
            return -1;
        }

        // Linear RGBA8 distance data, never sRGB, sampled bilinearly by the text pixel shader.
        AtlasTexture = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width  = AtlasWidth,
            .Height = AtlasHeight,
            .Format = EFormat::RGBA8_UNORM,
            .DebugName = "Font.MTSDFAtlas"
        });
        RHI::Textures::Upload(AtlasTexture, 0, AtlasPixels.GetData(), AtlasPixels.Num(), AtlasWidth);

        return (int32)AtlasTexture.ResourceID();
    }

    bool CFont::ShapeText(const FString& Text, float HAlign, float VAlign, float LineSpacing, TVector<FShapedGlyph>& Out) const
    {
        Out.Reset();

        if (!HasAtlas())
        {
            return false;
        }

        const float  LineAdvance = LineHeight * (LineSpacing <= 0.0f ? 1.0f : LineSpacing);

        // Two passes, so per-line runs and widths come first and each line aligns independently.
        TVector<TVector<const FFontGlyph*>> Lines;
        TVector<float>                      LineWidths;
        Lines.Emplace();
        LineWidths.Add(0.0f);

        const FStringView View = Text.View();
        for (int32 i = 0; i < View.Len(); )
        {
            const uint32 CP = DecodeUTF8Codepoint(View, i);
            if (CP == '\n')
            {
                Lines.Emplace();
                LineWidths.Add(0.0f);
                continue;
            }
            if (CP == '\r')
            {
                continue;
            }

            if (const FFontGlyph* G = FindGlyph(CP))
            {
                Lines.Last().Add(G);
                LineWidths.Last() += G->Advance;
            }
        }

        const int32 NumLines    = Lines.Num();
        const float BlockHeight = (Ascender - Descender) + (float)(NumLines - 1) * LineAdvance;
        const float StartY      = -Ascender + VAlign * BlockHeight;

        for (int32 Line = 0; Line < NumLines; ++Line)
        {
            const float BaselineY = StartY - (float)Line * LineAdvance;
            float       PenX      = -HAlign * LineWidths[Line];

            for (const FFontGlyph* G : Lines[Line])
            {
                const FVector4& P = G->Plane;
                // Zero-area glyphs such as space still advance the pen below.
                if (P.z > P.x && P.w > P.y)
                {
                    FShapedGlyph& S = Out.EmplaceGetRef();
                    S.Min = FVector2(PenX + P.x, BaselineY + P.y);
                    S.Max = FVector2(PenX + P.z, BaselineY + P.w);
                    S.UV  = G->AtlasUV;
                }
                PenX += G->Advance;
            }
        }

        return true;
    }
}
