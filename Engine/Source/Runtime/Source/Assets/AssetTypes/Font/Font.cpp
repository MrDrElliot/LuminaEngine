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
        GlyphLookup.clear();
        GlyphLookup.reserve(Glyphs.size());
        for (int32 i = 0; i < (int32)Glyphs.size(); ++i)
        {
            GlyphLookup.emplace(Glyphs[i].Codepoint, i);
        }
        ++ShapeVersion;
    }

    const FFontGlyph* CFont::FindGlyph(uint32 Codepoint) const
    {
        auto It = GlyphLookup.find(Codepoint);
        if (It == GlyphLookup.end())
        {
            return nullptr;
        }
        return &Glyphs[It->second];
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
        RHI::Textures::Upload(AtlasTexture, 0, AtlasPixels.data(), AtlasPixels.size(), AtlasWidth);

        return (int32)AtlasTexture.ResourceID();
    }

    // Advances Index past one code point and returns it, or the replacement char on bad input.
    static uint32 DecodeUTF8(const char* Str, size_t Length, size_t& Index)
    {
        const uint8 C0 = (uint8)Str[Index++];
        if (C0 < 0x80) return C0;

        auto Cont = [&](uint32 Acc, int N) -> uint32
        {
            for (int i = 0; i < N; ++i)
            {
                if (Index >= Length || ((uint8)Str[Index] & 0xC0) != 0x80) return 0xFFFD;
                Acc = (Acc << 6) | ((uint8)Str[Index++] & 0x3F);
            }
            return Acc;
        };

        if ((C0 & 0xE0) == 0xC0) return Cont(C0 & 0x1F, 1);
        if ((C0 & 0xF0) == 0xE0) return Cont(C0 & 0x0F, 2);
        if ((C0 & 0xF8) == 0xF0) return Cont(C0 & 0x07, 3);
        return 0xFFFD;
    }

    bool CFont::ShapeText(const FString& Text, float HAlign, float VAlign, float LineSpacing, TVector<FShapedGlyph>& Out) const
    {
        Out.clear();

        if (!HasAtlas())
        {
            return false;
        }

        const char*  Str    = Text.c_str();
        const size_t Length = Text.size();
        const float  LineAdvance = LineHeight * (LineSpacing <= 0.0f ? 1.0f : LineSpacing);

        // Two passes, so per-line runs and widths come first and each line aligns independently.
        TVector<TVector<const FFontGlyph*>> Lines;
        TVector<float>                      LineWidths;
        Lines.emplace_back();
        LineWidths.push_back(0.0f);

        for (size_t i = 0; i < Length; )
        {
            const uint32 CP = DecodeUTF8(Str, Length, i);
            if (CP == '\n')
            {
                Lines.emplace_back();
                LineWidths.push_back(0.0f);
                continue;
            }
            if (CP == '\r')
            {
                continue;
            }

            if (const FFontGlyph* G = FindGlyph(CP))
            {
                Lines.back().push_back(G);
                LineWidths.back() += G->Advance;
            }
        }

        const int32 NumLines    = (int32)Lines.size();
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
                    FShapedGlyph& S = Out.emplace_back();
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
