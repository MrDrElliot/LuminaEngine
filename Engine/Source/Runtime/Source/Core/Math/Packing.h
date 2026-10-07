#pragma once

#include "Platform/GenericPlatform.h"
#include "Vector/VectorTypes.h"
#include <bit>
#include <immintrin.h>
#include <cstring>

// Bit-packing helpers (half-float pack/unpack) the engine uses.

namespace Lumina::Math
{
    namespace Detail
    {
        [[nodiscard]] constexpr uint16 FloatToHalfConstant(float F)
        {
            const uint32 Bits = std::bit_cast<uint32>(F);

            const uint32 Sign = (Bits >> 16) & 0x8000u;
            int32 Exp = static_cast<int32>((Bits >> 23) & 0xFFu) - 127 + 15;
            uint32 Mant = Bits & 0x007FFFFFu;

            if (Exp <= 0)
            {
                // Subnormal / underflow to zero.
                if (Exp < -10) { return static_cast<uint16>(Sign); }
                Mant |= 0x00800000u;
                const uint32 Shift = static_cast<uint32>(14 - Exp);
                uint32 HalfMant = Mant >> Shift;
                // Round to nearest even.
                if ((Mant >> (Shift - 1)) & 1u) { ++HalfMant; }
                return static_cast<uint16>(Sign | HalfMant);
            }
            if (Exp >= 0x1F)
            {
                // Overflow / inf / nan.
                if (((Bits >> 23) & 0xFFu) == 0xFFu && Mant != 0)
                {
                    return static_cast<uint16>(Sign | 0x7E00u); // nan
                }
                return static_cast<uint16>(Sign | 0x7C00u);     // inf
            }

            uint16 Half = static_cast<uint16>(Sign | (static_cast<uint32>(Exp) << 10) | (Mant >> 13));
            if (Mant & 0x00001000u) { ++Half; } // round to nearest even
            return Half;
        }

        [[nodiscard]] constexpr float HalfToFloatConstant(uint16 H)
        {
            const uint32 Sign = static_cast<uint32>(H & 0x8000u) << 16;
            uint32 Exp = (H >> 10) & 0x1Fu;
            uint32 Mant = H & 0x03FFu;
            uint32 Bits;

            if (Exp == 0)
            {
                if (Mant == 0)
                {
                    Bits = Sign;
                }
                else
                {
                    Exp = 1;
                    while ((Mant & 0x0400u) == 0) { Mant <<= 1; --Exp; }
                    Mant &= 0x03FFu;
                    Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
                }
            }
            else if (Exp == 0x1F)
            {
                Bits = Sign | 0x7F800000u | (Mant << 13);
            }
            else
            {
                Bits = Sign | ((Exp + (127 - 15)) << 23) | (Mant << 13);
            }

            return std::bit_cast<float>(Bits);
        }

        // The software path exists only for constant evaluation; at runtime F16C converts in one instruction.
        [[nodiscard]] constexpr uint16 FloatToHalf(float F)
        {
            if consteval
            {
                return FloatToHalfConstant(F);
            }
            else
            {
                return static_cast<uint16>(_mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(F), _MM_FROUND_TO_NEAREST_INT)));
            }
        }

        [[nodiscard]] constexpr float HalfToFloat(uint16 H)
        {
            if consteval
            {
                return HalfToFloatConstant(H);
            }
            else
            {
                return _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(H)));
            }
        }
    }

    // PackHalf2x16: x -> low 16 bits, y -> high 16 bits.
    [[nodiscard]] constexpr uint32 PackHalf2x16(const TVec<float, 2>& V)
    {
        if consteval
        {
            return static_cast<uint32>(Detail::FloatToHalfConstant(V.x)) |
                   (static_cast<uint32>(Detail::FloatToHalfConstant(V.y)) << 16);
        }
        else
        {
            const __m128i Halves = _mm_cvtps_ph(_mm_setr_ps(V.x, V.y, 0.0f, 0.0f), _MM_FROUND_TO_NEAREST_INT);
            return static_cast<uint32>(_mm_cvtsi128_si32(Halves));
        }
    }

    [[nodiscard]] constexpr TVec<float, 2> UnpackHalf2x16(uint32 Packed)
    {
        if consteval
        {
            return TVec<float, 2>(
                Detail::HalfToFloatConstant(static_cast<uint16>(Packed & 0xFFFFu)),
                Detail::HalfToFloatConstant(static_cast<uint16>(Packed >> 16)));
        }
        else
        {
            const __m128 Floats = _mm_cvtph_ps(_mm_cvtsi32_si128(static_cast<int>(Packed)));
            return TVec<float, 2>(_mm_cvtss_f32(Floats), _mm_cvtss_f32(_mm_movehdup_ps(Floats)));
        }
    }
}
