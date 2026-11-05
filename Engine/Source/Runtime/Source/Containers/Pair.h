#pragma once

#include <compare>
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"

namespace Lumina::Containers
{
    /** Two values held together. Interchangeable with the standard pair through its converting constructors. */
    template <typename T1, typename T2>
    struct TPair
    {
        using first_type  = T1;
        using second_type = T2;

        T1 Key;
        T2 Value;

        constexpr TPair()
            requires (std::is_default_constructible_v<T1> && std::is_default_constructible_v<T2>)
            : Key()
            , Value()
        {}

        constexpr TPair(const T1& InFirst, const T2& InSecond)
            : Key(InFirst)
            , Value(InSecond)
        {}

        template <typename U1, typename U2>
        requires (std::is_constructible_v<T1, U1&&> && std::is_constructible_v<T2, U2&&>)
        constexpr TPair(U1&& InFirst, U2&& InSecond)
            : Key(std::forward<U1>(InFirst))
            , Value(std::forward<U2>(InSecond))
        {}

        template <typename U1, typename U2>
        requires (std::is_constructible_v<T1, const U1&> && std::is_constructible_v<T2, const U2&>)
        constexpr TPair(const TPair<U1, U2>& Other)
            : Key(Other.Key)
            , Value(Other.Value)
        {}

        template <typename U1, typename U2>
        requires (std::is_constructible_v<T1, U1&&> && std::is_constructible_v<T2, U2&&>)
        constexpr TPair(TPair<U1, U2>&& Other)
            : Key(std::move(Other.Key))
            , Value(std::move(Other.Value))
        {}

        template <typename U1, typename U2>
        requires (std::is_constructible_v<T1, const U1&> && std::is_constructible_v<T2, const U2&>)
        constexpr TPair(const std::pair<U1, U2>& Other)
            : Key(Other.first)
            , Value(Other.second)
        {}

        template <typename U1, typename U2>
        requires (std::is_constructible_v<T1, U1&&> && std::is_constructible_v<T2, U2&&>)
        constexpr TPair(std::pair<U1, U2>&& Other)
            : Key(std::move(Other.first))
            , Value(std::move(Other.second))
        {}

        /** Builds both members in place from their own argument packs, which is what a map insert needs. */
        template <typename... TArgs1, typename... TArgs2>
        constexpr TPair(std::piecewise_construct_t, TTuple<TArgs1...> FirstArgs, TTuple<TArgs2...> SecondArgs)
            : TPair(FirstArgs, SecondArgs, std::index_sequence_for<TArgs1...>{}, std::index_sequence_for<TArgs2...>{})
        {}

        constexpr TPair(const TPair&) = default;
        constexpr TPair(TPair&&) = default;
        constexpr TPair& operator=(const TPair&) = default;
        constexpr TPair& operator=(TPair&&) = default;
        ~TPair() = default;

        NODISCARD constexpr operator std::pair<T1, T2>() const { return std::pair<T1, T2>(Key, Value); }

        friend constexpr void swap(TPair& Left, TPair& Right)
            noexcept(std::is_nothrow_swappable_v<T1> && std::is_nothrow_swappable_v<T2>)
        {
            using std::swap;
            swap(Left.Key, Right.Key);
            swap(Left.Value, Right.Value);
        }

        NODISCARD friend constexpr bool operator==(const TPair& Left, const TPair& Right)
        {
            return Left.Key == Right.Key && Left.Value == Right.Value;
        }

        NODISCARD friend constexpr auto operator<=>(const TPair& Left, const TPair& Right)
        {
            if (const auto Order = Left.Key <=> Right.Key; Order != 0)
            {
                return Order;
            }
            return Left.Value <=> Right.Value;
        }

    private:

        template <typename TFirstTuple, typename TSecondTuple, size_t... FirstIndices, size_t... SecondIndices>
        constexpr TPair(TFirstTuple& FirstArgs, TSecondTuple& SecondArgs,
                        std::index_sequence<FirstIndices...>, std::index_sequence<SecondIndices...>)
            : Key(Lumina::Get<FirstIndices>(Move(FirstArgs))...)
            , Value(Lumina::Get<SecondIndices>(Move(SecondArgs))...)
        {}
    };

    template <typename T1, typename T2>
    TPair(T1, T2) -> TPair<T1, T2>;

    template <typename T1, typename T2>
    NODISCARD constexpr TPair<std::decay_t<T1>, std::decay_t<T2>> MakePair(T1&& First, T2&& Second)
    {
        return TPair<std::decay_t<T1>, std::decay_t<T2>>(std::forward<T1>(First), std::forward<T2>(Second));
    }


    namespace Private
    {
        template <typename T>
        inline constexpr bool bIsPair = false;

        template <typename T1, typename T2>
        inline constexpr bool bIsPair<TPair<T1, T2>> = true;
    }

    template <size_t Index, typename TSelf>
    requires Private::bIsPair<std::remove_cvref_t<TSelf>>
    NODISCARD constexpr auto&& get(TSelf&& Pair) noexcept
    {
        static_assert(Index < 2, "A pair has exactly two members.");
        if constexpr (Index == 0)
        {
            return std::forward_like<TSelf>(Pair.Key);
        }
        else
        {
            return std::forward_like<TSelf>(Pair.Value);
        }
    }
}

// The tuple protocol, so structured bindings work on our pair the way they do on the standard one.
namespace std
{
    template <typename T1, typename T2>
    struct tuple_size<Lumina::Containers::TPair<T1, T2>> : integral_constant<size_t, 2> {};

    template <typename T1, typename T2>
    struct tuple_element<0, Lumina::Containers::TPair<T1, T2>> { using type = T1; };

    template <typename T1, typename T2>
    struct tuple_element<1, Lumina::Containers::TPair<T1, T2>> { using type = T2; };
}

namespace Lumina
{
    template <typename K, typename V>
    using TPair = Containers::TPair<K, V>;

    template <typename K, typename V>
    struct TIsTriviallyRelocatable<Containers::TPair<K, V>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<std::remove_cv_t<K>> && TIsTriviallyRelocatable_V<std::remove_cv_t<V>>;
    };
}
