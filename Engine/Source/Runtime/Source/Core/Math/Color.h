#pragma once
#include <algorithm>
#include "Core/Serialization/Archiver.h"
#include "Core/Math/Vector/Vector.h"

namespace Lumina
{
    struct RUNTIME_API FColor
    {
        float R, G, B, A;
    
        constexpr FColor() : R(0.0f), G(0.0f), B(0.0f), A(1.0f) {}
        constexpr FColor(float init) : R(init), G(init), B(init), A(init) {}
        constexpr FColor(float r, float g, float b, float a = 1.0f) : R(r), G(g), B(b), A(a) {}
        constexpr FColor(const FVector3& Vec) : R(Vec.r), G(Vec.g), B(Vec.b), A(1.0f) {}
        constexpr FColor(const FVector4& Vec) :R(Vec.r), G(Vec.g), B(Vec.b), A(Vec.a) {}


        operator FVector4() const
        {
            return FVector4(R, G, B, A);
        }

        operator FVector3() const
        {
            return FVector3(R, G, B);
        }

        friend FArchive& operator << (FArchive& Ar, FColor& data)
        {
            Ar << data.R;
            Ar << data.G;
            Ar << data.B;
            Ar << data.A;
            return Ar;
        }
    
        float GetR() const { return R; }
        float GetG() const { return G; }
        float GetB() const { return B; }
        float GetA() const { return A; }
    
        void SetR(float r) { R = r; }
        void SetG(float g) { G = g; }
        void SetB(float b) { B = b; }
        void SetA(float a) { A = a; }
    
        float ToGrayscale() const
        {
            return 0.2126f * R + 0.7152f * G + 0.0722f * B;
        }
    
        void Clamp()
        {
            R = std::clamp(R, 0.0f, 1.0f);
            G = std::clamp(G, 0.0f, 1.0f);
            B = std::clamp(B, 0.0f, 1.0f);
            A = std::clamp(A, 0.0f, 1.0f);
        }
    
        FColor operator+(const FColor& other) const
        {
            return FColor(R + other.R, G + other.G, B + other.B, A + other.A);
        }

        FColor operator-(const FColor& other) const
        {
            return FColor(R - other.R, G - other.G, B - other.B, A - other.A);
        }

        FColor operator*(float scalar) const
        {
            return FColor(R * scalar, G * scalar, B * scalar, A * scalar);
        }

        FColor operator*(const FColor& other) const
        {
            return FColor(R * other.R, G * other.G, B * other.B, A * other.A);
        }

        static FColor Lerp(const FColor& start, const FColor& end, float t)
        {
            return FColor(
                start.R + t * (end.R - start.R),
                start.G + t * (end.G - start.G),
                start.B + t * (end.B - start.B),
                start.A + t * (end.A - start.A)
            );
        }

        FString ToString() const
        {
            return "R: " + eastl::to_string(R) + " G: " + eastl::to_string(G) + 
                   " B: " + eastl::to_string(B) + " A: " + eastl::to_string(A);
        }

        bool operator == (const FColor& Other) const
        {
            return R == Other.R
                && G == Other.G
                && B == Other.B
                && A == Other.A;
        }

        bool operator != (const FColor& Color) const
        {
            return !(*this == Color);
        }
    
        static FColor FromGrayscale(float value, float alpha = 1.0f)
        {
            return FColor(value, value, value, alpha);
        }

        static FColor MakeRandom(float alpha = 1.0f);

        static FColor MakeRandomWithAlpha();

        /** Higher saturation. */
        static FColor MakeRandomVibrant(float alpha = 1.0f);

        /** Lower saturation, higher lightness. */
        static FColor MakeRandomPastel(float alpha = 1.0f);

        static void RGBtoHSL(const FColor& color, float& h, float& s, float& l)
        {
            float r = color.R;
            float g = color.G;
            float b = color.B;
    
            float max = std::max({r, g, b});
            float min = std::min({r, g, b});
            float delta = max - min;
    
            h = 0.0f;
            if (delta != 0.0f) {
                if (max == r) {
                    h = (g - b) / delta;
                } else if (max == g) {
                    h = (b - r) / delta + 2.0f;
                } else {
                    h = (r - g) / delta + 4.0f;
                }
            }
    
            l = (max + min) / 2.0f;
            s = (max == min) ? 0.0f : (max - min) / (1.0f - std::abs(2.0f * l - 1.0f));
            h /= 6.0f;
        }

        static FColor HSLtoRGB(float h, float s, float l, float alpha = 1.0f)
        {
            float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
            float x = c * (1.0f - std::abs(fmod(h * 6.0f, 2.0f) - 1.0f));
            float m = l - c / 2.0f;
    
            float r = 0.0f, g = 0.0f, b = 0.0f;
    
            if (h < 1.0f / 6.0f)
            {
                r = c;
                g = x;
            }
            else if (h < 2.0f / 6.0f)
            {
                r = x;
                g = c;
            }
            else if (h < 3.0f / 6.0f)
            {
                g = c;
                b = x;
            }
            else if (h < 4.0f / 6.0f)
            {
                g = x;
                b = c;
            }
            else if (h < 5.0f / 6.0f)
            {
                r = x;
                b = c;
            }
            else
            {
                r = c;
                b = x;
            }
    
            return FColor((r + m), (g + m), (b + m), alpha);
        }

        static const FColor Red;
        static const FColor Green;
        static const FColor Blue;
        static const FColor Yellow;
        static const FColor White;
        static const FColor Black;
        
    };
}
