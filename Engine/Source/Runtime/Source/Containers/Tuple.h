#pragma once

#include "Core/Templates/LuminaTemplate.h"
#include "Platform/GenericPlatform.h"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace Lumina
{
    template <typename... Ts>
    class TTuple;

    namespace Private
    {
        /** One element, tagged by position so the pack can be flattened into distinct bases. */
        template <size_t Index, typename T>
        struct TTupleLeaf
        {
            T Value;
        };

        template <typename TIndices, typename... Ts>
        struct TTupleStorage;

        // Flat rather than recursive, so a wide tuple costs one instantiation depth instead of N.
        template <size_t... Indices, typename... Ts>
        struct TTupleStorage<std::index_sequence<Indices...>, Ts...> : TTupleLeaf<Indices, Ts>...
        {
            constexpr TTupleStorage() = default;

            // Indices, Ts and UTypes expand in lockstep, which is what puts each argument in its own leaf.
            template <typename... UTypes>
            constexpr explicit TTupleStorage(UTypes&&... Args)
                : TTupleLeaf<Indices, Ts>{ std::forward<UTypes>(Args) }...
            {
            }
        };

        // Deduced from the unique base carrying this index, which is what keeps the lookup O(1).
        template <size_t Index, typename T>
        constexpr TTupleLeaf<Index, T>& FindLeaf(TTupleLeaf<Index, T>& Leaf) { return Leaf; }

        template <size_t Index, typename T>
        constexpr const TTupleLeaf<Index, T>& FindLeaf(const TTupleLeaf<Index, T>& Leaf) { return Leaf; }

        template <typename T, size_t Index>
        constexpr TTupleLeaf<Index, T>& FindLeafByType(TTupleLeaf<Index, T>& Leaf) { return Leaf; }

        template <typename T, size_t Index>
        constexpr const TTupleLeaf<Index, T>& FindLeafByType(const TTupleLeaf<Index, T>& Leaf) { return Leaf; }

        template <size_t Index, typename... Ts>
        struct TTypeAt;

        template <typename T0, typename... Rest>
        struct TTypeAt<0, T0, Rest...> { using Type = T0; };

        template <size_t Index, typename T0, typename... Rest>
        struct TTypeAt<Index, T0, Rest...> : TTypeAt<Index - 1, Rest...> {};
    }

    /** Element type at a position, so callers do not have to reach for the leaf machinery. */
    template <size_t Index, typename TTupleType>
    struct TTupleElement;

    template <size_t Index, typename... Ts>
    struct TTupleElement<Index, TTuple<Ts...>>
    {
        using Type = typename Private::TTypeAt<Index, Ts...>::Type;
    };

    template <size_t Index, typename TTupleType>
    using TTupleElementT = typename TTupleElement<Index, std::remove_cvref_t<TTupleType>>::Type;

    template <typename TTupleType>
    struct TTupleSize;

    template <typename... Ts>
    struct TTupleSize<TTuple<Ts...>> : std::integral_constant<size_t, sizeof...(Ts)> {};

    template <typename TTupleType>
    inline constexpr size_t TTupleSizeV = TTupleSize<std::remove_cvref_t<TTupleType>>::value;

    /** Fixed heterogeneous aggregate. Holds references as references, so it can carry a view row. */
    template <typename... Ts>
    class TTuple
    {
        using FStorage = Private::TTupleStorage<std::index_sequence_for<Ts...>, Ts...>;

    public:

        static constexpr size_t Size() { return sizeof...(Ts); }

        constexpr TTuple() = default;

        template <typename... UTypes>
        requires (sizeof...(UTypes) == sizeof...(Ts) && sizeof...(Ts) > 0
                  && (std::is_constructible_v<Ts, UTypes&&> && ...))
        constexpr explicit TTuple(UTypes&&... Args)
            : Storage(std::forward<UTypes>(Args)...)
        {
        }

        constexpr TTuple(const TTuple&) = default;
        constexpr TTuple(TTuple&&) = default;
        constexpr TTuple& operator=(const TTuple&) = default;
        constexpr TTuple& operator=(TTuple&&) = default;

        // Elementwise, which is what lets a Tie result write back through its references.
        template <typename... UTypes>
        requires (sizeof...(UTypes) == sizeof...(Ts) && sizeof...(Ts) > 0
                  && (std::is_assignable_v<Ts&, const UTypes&> && ...))
        constexpr TTuple& operator=(const TTuple<UTypes...>& Other)
        {
            AssignFrom(Other, std::index_sequence_for<Ts...>{});
            return *this;
        }

        template <size_t Index>
        NODISCARD constexpr auto& Get() & { return Private::FindLeaf<Index>(Storage).Value; }

        template <size_t Index>
        NODISCARD constexpr const auto& Get() const& { return Private::FindLeaf<Index>(Storage).Value; }

        // Cast rather than Move, because an element that is already a reference must stay one.
        template <size_t Index>
        NODISCARD constexpr auto&& Get() &&
        {
            using FElement = typename Private::TTypeAt<Index, Ts...>::Type;
            return static_cast<FElement&&>(Private::FindLeaf<Index>(Storage).Value);
        }

        template <typename T>
        NODISCARD constexpr T& Get() & { return Private::FindLeafByType<T>(Storage).Value; }

        template <typename T>
        NODISCARD constexpr const T& Get() const& { return Private::FindLeafByType<T>(Storage).Value; }

        NODISCARD friend constexpr bool operator==(const TTuple& Left, const TTuple& Right)
        {
            return EqualsImpl(Left, Right, std::index_sequence_for<Ts...>{});
        }

    private:

        template <typename TOther, size_t... Indices>
        constexpr void AssignFrom(const TOther& Other, std::index_sequence<Indices...>)
        {
            ((Get<Indices>() = Other.template Get<Indices>()), ...);
        }

        template <size_t... Indices>
        static constexpr bool EqualsImpl(const TTuple& Left, const TTuple& Right, std::index_sequence<Indices...>)
        {
            return ((Left.template Get<Indices>() == Right.template Get<Indices>()) && ... && true);
        }

        FStorage Storage{};
    };

    template <typename... Ts>
    TTuple(Ts...) -> TTuple<Ts...>;

    //~ Free accessors, which is what generic code and the structured binding protocol reach for.

    template <size_t Index, typename... Ts>
    NODISCARD constexpr auto& Get(TTuple<Ts...>& Tuple) { return Tuple.template Get<Index>(); }

    template <size_t Index, typename... Ts>
    NODISCARD constexpr const auto& Get(const TTuple<Ts...>& Tuple) { return Tuple.template Get<Index>(); }

    template <size_t Index, typename... Ts>
    NODISCARD constexpr auto&& Get(TTuple<Ts...>&& Tuple) { return static_cast<TTuple<Ts...>&&>(Tuple).template Get<Index>(); }

    template <typename T, typename... Ts>
    NODISCARD constexpr T& Get(TTuple<Ts...>& Tuple) { return Tuple.template Get<T>(); }

    template <typename T, typename... Ts>
    NODISCARD constexpr const T& Get(const TTuple<Ts...>& Tuple) { return Tuple.template Get<T>(); }

    // Lowercase because structured bindings look up exactly this name, by argument dependent lookup.
    template <size_t Index, typename... Ts>
    NODISCARD constexpr auto& get(TTuple<Ts...>& Tuple) { return Tuple.template Get<Index>(); }

    template <size_t Index, typename... Ts>
    NODISCARD constexpr const auto& get(const TTuple<Ts...>& Tuple) { return Tuple.template Get<Index>(); }

    template <size_t Index, typename... Ts>
    NODISCARD constexpr auto&& get(TTuple<Ts...>&& Tuple) { return static_cast<TTuple<Ts...>&&>(Tuple).template Get<Index>(); }

    template <typename... Ts>
    NODISCARD constexpr TTuple<std::unwrap_ref_decay_t<Ts>...> MakeTuple(Ts&&... Args)
    {
        return TTuple<std::unwrap_ref_decay_t<Ts>...>(std::forward<Ts>(Args)...);
    }

    /** Binds lvalues by reference, so assigning through the result writes back to the originals. */
    template <typename... Ts>
    NODISCARD constexpr TTuple<Ts&...> Tie(Ts&... Args)
    {
        return TTuple<Ts&...>(Args...);
    }

    template <typename... Ts>
    NODISCARD constexpr TTuple<Ts&&...> ForwardAsTuple(Ts&&... Args)
    {
        return TTuple<Ts&&...>(std::forward<Ts>(Args)...);
    }

    namespace Private
    {
        template <typename TFunc, typename TTupleType, size_t... Indices>
        constexpr decltype(auto) ApplyImpl(TFunc&& Func, TTupleType&& Tuple, std::index_sequence<Indices...>)
        {
            return std::forward<TFunc>(Func)(Lumina::Get<Indices>(std::forward<TTupleType>(Tuple))...);
        }

        template <typename TLeft, typename TRight, size_t... LeftIndices, size_t... RightIndices>
        constexpr auto CatTwo(TLeft&& Left, TRight&& Right,
            std::index_sequence<LeftIndices...>, std::index_sequence<RightIndices...>)
        {
            return TTuple<
                TTupleElementT<LeftIndices, TLeft>...,
                TTupleElementT<RightIndices, TRight>...>(
                    Lumina::Get<LeftIndices>(std::forward<TLeft>(Left))...,
                    Lumina::Get<RightIndices>(std::forward<TRight>(Right))...);
        }
    }

    template <typename TFunc, typename TTupleType>
    constexpr decltype(auto) Apply(TFunc&& Func, TTupleType&& Tuple)
    {
        return Private::ApplyImpl(std::forward<TFunc>(Func), std::forward<TTupleType>(Tuple),
            std::make_index_sequence<TTupleSizeV<TTupleType>>{});
    }

    NODISCARD constexpr TTuple<> TupleCat() { return TTuple<>{}; }

    template <typename TFirst>
    NODISCARD constexpr auto TupleCat(TFirst&& First)
    {
        return std::remove_cvref_t<TFirst>(std::forward<TFirst>(First));
    }

    template <typename TFirst, typename TSecond, typename... TRest>
    NODISCARD constexpr auto TupleCat(TFirst&& First, TSecond&& Second, TRest&&... Rest)
    {
        auto Joined = Private::CatTwo(std::forward<TFirst>(First), std::forward<TSecond>(Second),
            std::make_index_sequence<TTupleSizeV<TFirst>>{},
            std::make_index_sequence<TTupleSizeV<TSecond>>{});

        if constexpr (sizeof...(TRest) == 0)
        {
            return Joined;
        }
        else
        {
            return TupleCat(Move(Joined), std::forward<TRest>(Rest)...);
        }
    }
}

namespace std
{
    template <typename... Ts>
    struct tuple_size<Lumina::TTuple<Ts...>> : integral_constant<size_t, sizeof...(Ts)> {};

    template <size_t Index, typename... Ts>
    struct tuple_element<Index, Lumina::TTuple<Ts...>>
    {
        using type = Lumina::TTupleElementT<Index, Lumina::TTuple<Ts...>>;
    };
}
