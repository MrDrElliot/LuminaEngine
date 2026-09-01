#pragma once

#include "QuadInstance.h"
#include "Containers/Vector.h"

namespace Umbral::Font
{
    inline constexpr int32 kGlyphWidth  = 5;
    inline constexpr int32 kGlyphHeight = 7;
    inline constexpr int32 kAdvance     = 6;

    struct FGlyph
    {
        uint8 Rows[kGlyphHeight];
    };

    enum class EAlign : uint8
    {
        Left,
        Center,
        Right,
    };

    inline const FGlyph& GlyphFor(char Character)
    {
        static const FGlyph kDigits[10] =
        {
            { { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
            { { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
            { { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
            { { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
            { { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
            { { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
            { { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
            { { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
            { { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
            { { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
        };

        static const FGlyph kLetters[26] =
        {
            { { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
            { { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
            { { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
            { { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C } },
            { { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
            { { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
            { { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
            { { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
            { { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
            { { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
            { { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
            { { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
            { { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
            { { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
            { { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
            { { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
            { { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
            { { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
            { { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
            { { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
            { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
            { { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
            { { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
            { { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
            { { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
            { { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
        };

        static const FGlyph kBlank      { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
        static const FGlyph kColon      { { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } };
        static const FGlyph kHyphen     { { 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00 } };
        static const FGlyph kBang       { { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } };
        static const FGlyph kPeriod     { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } };
        static const FGlyph kSlash      { { 0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10 } };
        static const FGlyph kPlus       { { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } };
        static const FGlyph kMultiply   { { 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 } };
        static const FGlyph kArrow      { { 0x08, 0x0C, 0x0E, 0x0F, 0x0E, 0x0C, 0x08 } };
        static const FGlyph kBlock      { { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F } };

        if (Character >= '0' && Character <= '9')
        {
            return kDigits[Character - '0'];
        }
        if (Character >= 'A' && Character <= 'Z')
        {
            return kLetters[Character - 'A'];
        }
        if (Character >= 'a' && Character <= 'z')
        {
            return kLetters[Character - 'a'];
        }

        switch (Character)
        {
        case ':':  return kColon;
        case '-':  return kHyphen;
        case '!':  return kBang;
        case '.':  return kPeriod;
        case '/':  return kSlash;
        case '+':  return kPlus;
        case 'x':  return kMultiply;
        case '>':  return kArrow;
        case '#':  return kBlock;
        default:   return kBlank;
        }
    }

    inline int32 Length(const char* Text)
    {
        int32 Count = 0;
        while (Text[Count] != '\0')
        {
            ++Count;
        }
        return Count;
    }

    inline float MeasureWidth(const char* Text, float PixelSize)
    {
        const int32 Count = Length(Text);
        return Count > 0 ? (Count * kAdvance - 1) * PixelSize : 0.0f;
    }

    // Origin is the top-left of the first glyph unless Align moves it, in field units.
    inline void Emit(TVector<FQuadInstance>& Out, const char* Text, const FVector2& Origin, float PixelSize,
                     const FVector4& Color, float Glow, EAlign Align = EAlign::Left, float Rotation = 0.0f)
    {
        float StartX = Origin.x;
        const float Width = MeasureWidth(Text, PixelSize);
        if (Align == EAlign::Center)
        {
            StartX -= Width * 0.5f;
        }
        else if (Align == EAlign::Right)
        {
            StartX -= Width;
        }

        const float Half = PixelSize * 0.5f;

        for (int32 Index = 0; Text[Index] != '\0'; ++Index)
        {
            const FGlyph& Glyph = GlyphFor(Text[Index]);
            const float GlyphX = StartX + Index * kAdvance * PixelSize;

            for (int32 Row = 0; Row < kGlyphHeight; ++Row)
            {
                const uint8 Bits = Glyph.Rows[Row];
                for (int32 Column = 0; Column < kGlyphWidth; ++Column)
                {
                    if ((Bits & (1u << (kGlyphWidth - 1 - Column))) == 0)
                    {
                        continue;
                    }

                    FQuadInstance& Instance = Out.emplace_back();
                    Instance.Center       = { GlyphX + Column * PixelSize + Half, Origin.y + Row * PixelSize + Half };
                    Instance.HalfSize     = { Half * 0.94f, Half * 0.94f };
                    Instance.Color        = Color;
                    Instance.Accent       = Color;
                    Instance.CornerRadius = 0.32f;
                    Instance.Glow         = Glow;
                    Instance.Rotation     = Rotation;
                    Instance.Kind         = 0;
                }
            }
        }
    }

    inline void FormatInt(char* Buffer, int32 Capacity, int32 Value)
    {
        if (Capacity <= 0)
        {
            return;
        }

        char Reversed[16];
        int32 Count = 0;
        int32 Remaining = Value < 0 ? -Value : Value;

        do
        {
            Reversed[Count++] = char('0' + Remaining % 10);
            Remaining /= 10;
        }
        while (Remaining != 0 && Count < 15);

        int32 Written = 0;
        if (Value < 0 && Written < Capacity - 1)
        {
            Buffer[Written++] = '-';
        }
        while (Count > 0 && Written < Capacity - 1)
        {
            Buffer[Written++] = Reversed[--Count];
        }
        Buffer[Written] = '\0';
    }

    inline void FormatPadded(char* Buffer, int32 Capacity, int32 Value, int32 MinDigits)
    {
        char Digits[16];
        FormatInt(Digits, 16, Value < 0 ? 0 : Value);

        const int32 Count = Length(Digits);
        const int32 Pad = MinDigits > Count ? MinDigits - Count : 0;

        int32 Written = 0;
        for (int32 i = 0; i < Pad && Written < Capacity - 1; ++i)
        {
            Buffer[Written++] = '0';
        }
        for (int32 i = 0; i < Count && Written < Capacity - 1; ++i)
        {
            Buffer[Written++] = Digits[i];
        }
        Buffer[Written] = '\0';
    }
}
