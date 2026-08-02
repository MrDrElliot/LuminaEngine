#pragma once

#include "Platform/GenericPlatform.h"
#include "Vector/VectorTypes.h"
#include <cstring>

// Bit-packing helpers (half-float pack/unpack) the engine uses.

namespace Lumina::Math
{
    namespace Detail
    {
        [[nodiscard]] inline uint16 FloatToHalf(float F)
        {
            uint32 Bits;
            std::memcpy(&Bits, &F, sizeof(Bits));

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

        [[nodiscard]] inline float HalfToFloat(uint16 H)
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

            float F;
            std::memcpy(&F, &Bits, sizeof(F));
            return F;
        }
    }

    // PackHalf2x16: x -> low 16 bits, y -> high 16 bits.
    [[nodiscard]] inline uint32 PackHalf2x16(const TVec<float, 2>& V)
    {
        return static_cast<uint32>(Detail::FloatToHalf(V.x)) |
               (static_cast<uint32>(Detail::FloatToHalf(V.y)) << 16);
    }

    [[nodiscard]] inline TVec<float, 2> UnpackHalf2x16(uint32 Packed)
    {
        return TVec<float, 2>(
            Detail::HalfToFloat(static_cast<uint16>(Packed & 0xFFFFu)),
            Detail::HalfToFloat(static_cast<uint16>(Packed >> 16)));
    }
}
