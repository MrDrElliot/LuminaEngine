#pragma once

#include "Core/Math/Packing.h"
#include <type_traits>

namespace Lumina
{
    // Storage type; arithmetic happens in float through the implicit conversions.
    struct FHalf
    {
        static constexpr uint16 SignMask = 0x8000;

        uint16 Bits;

        FHalf() = default;
        constexpr FHalf(float F) : Bits(Math::Detail::FloatToHalf(F)) {}

        [[nodiscard]] static constexpr FHalf FromBits(uint16 InBits)
        {
            FHalf Half;
            Half.Bits = InBits;
            return Half;
        }

        constexpr operator float() const { return Math::Detail::HalfToFloat(Bits); }

        [[nodiscard]] constexpr FHalf operator-() const { return FromBits(static_cast<uint16>(Bits ^ SignMask)); }

        constexpr FHalf& operator+=(float F) { return *this = *this + F; }
        constexpr FHalf& operator-=(float F) { return *this = *this - F; }
        constexpr FHalf& operator*=(float F) { return *this = *this * F; }
        constexpr FHalf& operator/=(float F) { return *this = *this / F; }
    };

    [[nodiscard]] constexpr FHalf operator""_h(long double Value)
    {
        return FHalf(static_cast<float>(Value));
    }

    [[nodiscard]] constexpr FHalf operator""_h(unsigned long long Value)
    {
        return FHalf(static_cast<float>(Value));
    }

    using FHalfVector2 = TVec<FHalf, 2>;
    using FHalfVector3 = TVec<FHalf, 3>;
    using FHalfVector4 = TVec<FHalf, 4>;

    static_assert(sizeof(FHalf) == 2 && std::is_trivially_copyable_v<FHalf>);
    static_assert(sizeof(FHalfVector2) == 4 && sizeof(FHalfVector3) == 6 && sizeof(FHalfVector4) == 8);
    static_assert(FHalf(1.0f).Bits == 0x3C00 && float(FHalf(-2.5f)) == -2.5f);
    static_assert((0.5_h).Bits == 0x3800 && (2_h).Bits == 0x4000);
    static_assert((-0.5_h).Bits == 0xB800 && std::is_same_v<decltype(-0.5_h), FHalf>);
}
