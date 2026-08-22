#pragma once

#include <type_traits>

#include "Platform/GenericPlatform.h"
#include "Core/Templates/NumericLimits.h"

namespace Lumina
{
    /** A number to compare by value, which bool, the character types and enums are not. */
    template <typename T>
    concept ValueComparableInteger =
        std::is_integral_v<T>
        && !std::is_same_v<std::remove_cv_t<T>, bool>
        && !std::is_same_v<std::remove_cv_t<T>, char>
        && !std::is_same_v<std::remove_cv_t<T>, wchar_t>
        && !std::is_same_v<std::remove_cv_t<T>, char8_t>
        && !std::is_same_v<std::remove_cv_t<T>, char16_t>
        && !std::is_same_v<std::remove_cv_t<T>, char32_t>;

    namespace CmpDetail
    {
        // Asked of the type rather than looked up in a table, so long and every other spelling answers too.
        template <typename T>
        inline constexpr bool bIsSignedInteger = T(-1) < T(0);

        template <SIZE_T Size>
        struct TUnsignedBySize;

        template <> struct TUnsignedBySize<1> { using Type = uint8; };
        template <> struct TUnsignedBySize<2> { using Type = uint16; };
        template <> struct TUnsignedBySize<4> { using Type = uint32; };
        template <> struct TUnsignedBySize<8> { using Type = uint64; };

        template <typename T>
        using TUnsignedOf = typename TUnsignedBySize<sizeof(T)>::Type;
    }

    // Integer comparison by value, where a plain Index < Container.size() would let a negative index through.
    namespace Cmp
    {
        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool Equal(TLeft Left, TRight Right) noexcept
        {
            if constexpr (CmpDetail::bIsSignedInteger<TLeft> == CmpDetail::bIsSignedInteger<TRight>)
            {
                return Left == Right;
            }
            else if constexpr (CmpDetail::bIsSignedInteger<TLeft>)
            {
                return Left >= 0 && CmpDetail::TUnsignedOf<TLeft>(Left) == Right;
            }
            else
            {
                return Right >= 0 && Left == CmpDetail::TUnsignedOf<TRight>(Right);
            }
        }

        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool Less(TLeft Left, TRight Right) noexcept
        {
            if constexpr (CmpDetail::bIsSignedInteger<TLeft> == CmpDetail::bIsSignedInteger<TRight>)
            {
                return Left < Right;
            }
            else if constexpr (CmpDetail::bIsSignedInteger<TLeft>)
            {
                // Anything negative is below every unsigned value, and the cast is safe once it is not.
                return Left < 0 || CmpDetail::TUnsignedOf<TLeft>(Left) < Right;
            }
            else
            {
                return Right >= 0 && Left < CmpDetail::TUnsignedOf<TRight>(Right);
            }
        }

        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool NotEqual(TLeft Left, TRight Right) noexcept
        {
            return !Equal(Left, Right);
        }

        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool Greater(TLeft Left, TRight Right) noexcept
        {
            return Less(Right, Left);
        }

        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool LessEqual(TLeft Left, TRight Right) noexcept
        {
            return !Less(Right, Left);
        }

        template <ValueComparableInteger TLeft, ValueComparableInteger TRight>
        NODISCARD constexpr bool GreaterEqual(TLeft Left, TRight Right) noexcept
        {
            return !Less(Left, Right);
        }

        // True when Value survives the trip into TTarget still meaning the same number.
        template <ValueComparableInteger TTarget, ValueComparableInteger TValue>
        NODISCARD constexpr bool InRange(TValue Value) noexcept
        {
            return GreaterEqual(Value, TNumericLimits<TTarget>::Lowest())
                && LessEqual(Value, TNumericLimits<TTarget>::Max());
        }
    }

    namespace CmpDetail
    {
        // Checked by the compiler in every translation unit, since a sign mistake here is silent at runtime.
        static_assert(!Cmp::Equal(-1, 1u));
        static_assert(!Cmp::Equal(-1, TNumericLimits<uint64>::Max()));
        static_assert(Cmp::Equal(0, 0u));
        static_assert(Cmp::Equal(255, uint8(255)));
        static_assert(!Cmp::Equal(int64(-1), uint32(0xFFFFFFFFu)));

        static_assert(Cmp::Less(-1, 1u));
        static_assert(Cmp::Less(TNumericLimits<int64>::Lowest(), 0u));
        static_assert(!Cmp::Less(1u, -1));
        static_assert(Cmp::Less(1, 2));
        static_assert(!Cmp::Less(2u, 1u));

        static_assert(!Cmp::Greater(-1, 1u));
        static_assert(Cmp::Greater(1u, -1));
        static_assert(!Cmp::GreaterEqual(-1, 0u));
        static_assert(Cmp::GreaterEqual(0, 0u));
        static_assert(Cmp::LessEqual(-1, 0u));
        static_assert(Cmp::NotEqual(-1, 1u));

        static_assert(!Cmp::InRange<uint8>(-1));
        static_assert(Cmp::InRange<uint8>(255));
        static_assert(!Cmp::InRange<uint8>(256));
        static_assert(Cmp::InRange<int8>(-128));
        static_assert(!Cmp::InRange<int8>(-129));
        static_assert(!Cmp::InRange<int32>(TNumericLimits<uint32>::Max()));
    }
}
