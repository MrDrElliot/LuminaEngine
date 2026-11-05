#pragma once

#include <new>
#include "Containers/Tuple.h"
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"
#include "Memory/Construct.h"

namespace Lumina::Containers
{
    template <typename... Ts>
    class TVariant;

    namespace Private
    {
        template <size_t... Values>
        NODISCARD constexpr size_t MaxOf() noexcept
        {
            size_t Largest = 0;
            ((Largest = Values > Largest ? Values : Largest), ...);
            return Largest;
        }

        /** Index of T in the pack, or the invalid index when it appears zero times or more than once. */
        template <typename T, typename... Ts>
        NODISCARD constexpr size_t IndexOfType() noexcept
        {
            constexpr bool Matches[] = { std::is_same_v<T, Ts>... };
            size_t Found = ~static_cast<size_t>(0);
            size_t Count = 0;
            for (size_t Index = 0; Index < sizeof...(Ts); ++Index)
            {
                if (Matches[Index])
                {
                    if (Count == 0)
                    {
                        Found = Index;
                    }
                    ++Count;
                }
            }
            return Count == 1 ? Found : ~static_cast<size_t>(0);
        }
    }

    template <size_t Index, typename TVariantType>
    struct TVariantAlternative;

    template <size_t Index, typename... Ts>
    struct TVariantAlternative<Index, TVariant<Ts...>>
    {
        using Type = TTupleElementT<Index, TTuple<Ts...>>;
    };

    template <size_t Index, typename TVariantType>
    using TVariantAlternativeT = typename TVariantAlternative<Index, TVariantType>::Type;

    /** Holds exactly one of Ts at a time, stored inline, with the live index carried alongside. */
    template <typename... Ts>
    class TVariant
    {
        static_assert(sizeof...(Ts) > 0, "A variant needs at least one alternative.");
        static_assert(sizeof...(Ts) < 255, "A variant indexes its alternatives in one byte.");

        static constexpr size_t kCount   = sizeof...(Ts);
        static constexpr size_t kStorage = Private::MaxOf<sizeof(Ts)...>();
        static constexpr size_t kAlign   = Private::MaxOf<alignof(Ts)...>();
        static constexpr uint8  kNone    = 0xFF;

        // All-trivial alternatives make the variant trivial too, so a moved-from one keeps its value; any other leaves it empty.
        static constexpr bool bTrivialCopy    = (std::is_trivially_copyable_v<Ts> && ...);
        static constexpr bool bTrivialDestroy = (std::is_trivially_destructible_v<Ts> && ...);
        static constexpr bool bNothrowMove    = (std::is_nothrow_move_constructible_v<Ts> && ...);

        template <size_t Index>
        using TAlternative = TTupleElementT<Index, TTuple<Ts...>>;

    public:

        static constexpr size_t kInvalidIndex = ~static_cast<size_t>(0);

        template <typename T>
        static constexpr size_t IndexOf = Private::IndexOfType<T, Ts...>();

        constexpr TVariant()
            requires std::is_default_constructible_v<TAlternative<0>>
        {
            Memory::ConstructAt(reinterpret_cast<TAlternative<0>*>(Buffer));
            Index = 0;
        }

        template <typename T, typename TDecayed = std::remove_cvref_t<T>>
        requires (IndexOf<TDecayed> != kInvalidIndex && !std::is_same_v<TDecayed, TVariant>)
        constexpr TVariant(T&& Value)
        {
            Memory::ConstructAt(reinterpret_cast<TDecayed*>(Buffer), std::forward<T>(Value));
            Index = static_cast<uint8>(IndexOf<TDecayed>);
        }

        template <size_t TargetIndex, typename... TArgs>
        constexpr explicit TVariant(std::in_place_index_t<TargetIndex>, TArgs&&... Args)
        {
            Memory::ConstructAt(reinterpret_cast<TAlternative<TargetIndex>*>(Buffer), std::forward<TArgs>(Args)...);
            Index = static_cast<uint8>(TargetIndex);
        }

        TVariant(const TVariant&) requires bTrivialCopy = default;
        TVariant(TVariant&&) requires bTrivialCopy = default;
        TVariant& operator=(const TVariant&) requires bTrivialCopy = default;
        TVariant& operator=(TVariant&&) requires bTrivialCopy = default;

        ~TVariant() requires bTrivialDestroy = default;
        ~TVariant() requires (!bTrivialDestroy) { Reset(); }

        TVariant(const TVariant& Other) requires (!bTrivialCopy)
        {
            CopyFrom(Other);
        }

        TVariant(TVariant&& Other) noexcept(bNothrowMove) requires (!bTrivialCopy)
        {
            MoveFrom(Other);
        }

        TVariant& operator=(const TVariant& Other) requires (!bTrivialCopy)
        {
            if (this != &Other)
            {
                Reset();
                CopyFrom(Other);
            }
            return *this;
        }

        TVariant& operator=(TVariant&& Other) noexcept(bNothrowMove) requires (!bTrivialCopy)
        {
            if (this != &Other)
            {
                Reset();
                MoveFrom(Other);
            }
            return *this;
        }

        template <typename T, typename TDecayed = std::remove_cvref_t<T>>
        requires (IndexOf<TDecayed> != kInvalidIndex && !std::is_same_v<TDecayed, TVariant>)
        TVariant& operator=(T&& Value)
        {
            if constexpr (std::is_assignable_v<TDecayed&, T&&>)
            {
                if (IsType<TDecayed>())
                {
                    Get<TDecayed>() = std::forward<T>(Value);
                    return *this;
                }
            }
            TDecayed Staged(std::forward<T>(Value));
            Emplace<TDecayed>(std::move_if_noexcept(Staged));
            return *this;
        }

        NODISCARD FORCEINLINE constexpr size_t GetIndex() const noexcept { return Index == kNone ? kInvalidIndex : Index; }

        /** False only after a throwing emplace left the variant with nothing in it. */
        NODISCARD FORCEINLINE constexpr bool IsValid() const noexcept { return Index != kNone; }

        template <typename T>
        NODISCARD FORCEINLINE constexpr bool IsType() const noexcept
        {
            if constexpr (IndexOf<T> == kInvalidIndex)
            {
                return false;
            }
            else
            {
                return Index == IndexOf<T>;
            }
        }

        template <typename T, typename... TArgs>
        T& Emplace(TArgs&&... Args)
        {
            static_assert(IndexOf<T> != kInvalidIndex, "That type is not one of the variant alternatives.");
            Reset();
            T* Constructed = Memory::ConstructAt(reinterpret_cast<T*>(Buffer), std::forward<TArgs>(Args)...);
            Index = static_cast<uint8>(IndexOf<T>);
            return *Constructed;
        }

        template <size_t TargetIndex, typename... TArgs>
        TAlternative<TargetIndex>& Emplace(TArgs&&... Args)
        {
            return Emplace<TAlternative<TargetIndex>>(std::forward<TArgs>(Args)...);
        }

        template <typename T, typename U>
        void Set(U&& Value) { Emplace<T>(std::forward<U>(Value)); }

        template <size_t TargetIndex, typename TSelf>
        NODISCARD FORCEINLINE auto&& GetByIndex(this TSelf&& Self) noexcept
        {
            LUMINA_CONTAINER_CHECK(Self.Index == TargetIndex);
            return std::forward_like<TSelf>(*std::launder(reinterpret_cast<TAlternative<TargetIndex>*>(const_cast<unsigned char*>(Self.Buffer))));
        }

        template <typename T, typename TSelf>
        NODISCARD FORCEINLINE auto&& Get(this TSelf&& Self) noexcept
        {
            return std::forward<TSelf>(Self).template GetByIndex<IndexOf<T>>();
        }

        template <typename T, typename TSelf>
        NODISCARD FORCEINLINE auto* TryGet(this TSelf&& Self) noexcept
        {
            using FResult = std::conditional_t<std::is_const_v<std::remove_reference_t<TSelf>>, const T, T>;
            return Self.template IsType<T>() ? std::launder(reinterpret_cast<FResult*>(const_cast<unsigned char*>(Self.Buffer))) : nullptr;
        }

        void Reset() noexcept
        {
            if constexpr (!bTrivialDestroy)
            {
                if (Index != kNone)
                {
                    DispatchVoid([this]<size_t I>(std::integral_constant<size_t, I>) noexcept
                    {
                        Memory::DestroyAt(reinterpret_cast<TAlternative<I>*>(Buffer));
                    });
                }
            }
            Index = kNone;
        }

        NODISCARD friend bool operator==(const TVariant& Left, const TVariant& Right)
        {
            if (Left.Index != Right.Index)
            {
                return false;
            }
            if (Left.Index == kNone)
            {
                return true;
            }
            bool bEqual = false;
            Left.DispatchVoid([&]<size_t I>(std::integral_constant<size_t, I>)
            {
                bEqual = Left.template GetByIndex<I>() == Right.template GetByIndex<I>();
            });
            return bEqual;
        }

    private:

        // A compare chain over the index, which the optimizer turns into a jump table and lets inline the body.
        template <typename TFn, size_t I = 0>
        FORCEINLINE void DispatchVoid(TFn&& Fn) const
        {
            if constexpr (I + 1 == kCount)
            {
                Fn(std::integral_constant<size_t, I>{});
            }
            else if (Index == I)
            {
                Fn(std::integral_constant<size_t, I>{});
            }
            else
            {
                DispatchVoid<TFn, I + 1>(std::forward<TFn>(Fn));
            }
        }

        void CopyFrom(const TVariant& Other)
        {
            if (Other.Index != kNone)
            {
                Other.DispatchVoid([this, &Other]<size_t I>(std::integral_constant<size_t, I>)
                {
                    Memory::ConstructAt(reinterpret_cast<TAlternative<I>*>(Buffer), Other.template GetByIndex<I>());
                });
                Index = Other.Index;
            }
        }

        void MoveFrom(TVariant& Other) noexcept(bNothrowMove)
        {
            if (Other.Index != kNone)
            {
                Other.DispatchVoid([this, &Other]<size_t I>(std::integral_constant<size_t, I>)
                {
                    Memory::ConstructAt(reinterpret_cast<TAlternative<I>*>(Buffer), std::move(Other).template GetByIndex<I>());
                });
                Index = Other.Index;
                Other.Reset();
            }
        }

        alignas(kAlign) unsigned char Buffer[kStorage];
        uint8 Index = kNone;
    };

    namespace Private
    {
        // Every alternative's result converts implicitly to the first one's, as the old dispatch table's entries did.
        template <typename TRet, size_t I, size_t Last, typename TVisitor, typename TVariantRef>
        FORCEINLINE constexpr TRet VisitFrom(TVisitor& Visitor, TVariantRef&& Variant, size_t Index)
        {
            if constexpr (I == Last)
            {
                return Visitor(std::forward<TVariantRef>(Variant).template GetByIndex<I>());
            }
            else
            {
                if (Index == I)
                {
                    return Visitor(std::forward<TVariantRef>(Variant).template GetByIndex<I>());
                }
                return VisitFrom<TRet, I + 1, Last>(Visitor, std::forward<TVariantRef>(Variant), Index);
            }
        }

        template <typename TVisitor, typename TVariantRef, size_t... Indices>
        constexpr decltype(auto) VisitImpl(TVisitor&& Visitor, TVariantRef&& Variant, std::index_sequence<Indices...>)
        {
            using FReturn = decltype(Visitor(std::forward<TVariantRef>(Variant).template GetByIndex<0>()));
            LUMINA_CONTAINER_CHECK(Variant.IsValid());
            return VisitFrom<FReturn, 0, sizeof...(Indices) - 1>(Visitor, std::forward<TVariantRef>(Variant), Variant.GetIndex());
        }
    }

    template <typename TVisitor, typename... Ts>
    constexpr decltype(auto) Visit(TVisitor&& Visitor, TVariant<Ts...>& Variant)
    {
        return Private::VisitImpl(std::forward<TVisitor>(Visitor), Variant, std::index_sequence_for<Ts...>{});
    }

    template <typename TVisitor, typename... Ts>
    constexpr decltype(auto) Visit(TVisitor&& Visitor, const TVariant<Ts...>& Variant)
    {
        return Private::VisitImpl(std::forward<TVisitor>(Visitor), Variant, std::index_sequence_for<Ts...>{});
    }

    template <typename TVisitor, typename... Ts>
    constexpr decltype(auto) Visit(TVisitor&& Visitor, TVariant<Ts...>&& Variant)
    {
        return Private::VisitImpl(std::forward<TVisitor>(Visitor), std::move(Variant), std::index_sequence_for<Ts...>{});
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE T& Get(TVariant<Ts...>& Variant) noexcept { return Variant.template Get<T>(); }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE const T& Get(const TVariant<Ts...>& Variant) noexcept { return Variant.template Get<T>(); }

    template <size_t Index, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) Get(TVariant<Ts...>& Variant) noexcept
    {
        return Variant.template GetByIndex<Index>();
    }

    template <size_t Index, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) Get(const TVariant<Ts...>& Variant) noexcept
    {
        return Variant.template GetByIndex<Index>();
    }

    template <size_t Index, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) Get(TVariant<Ts...>&& Variant) noexcept
    {
        return std::move(Variant).template GetByIndex<Index>();
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE T* GetIf(TVariant<Ts...>* Variant) noexcept
    {
        return Variant != nullptr ? Variant->template TryGet<T>() : nullptr;
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE const T* GetIf(const TVariant<Ts...>* Variant) noexcept
    {
        return Variant != nullptr ? Variant->template TryGet<T>() : nullptr;
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE bool HoldsAlternative(const TVariant<Ts...>& Variant) noexcept
    {
        return Variant.template IsType<T>();
    }

    template <typename... Ts>
    FORCEINLINE void swap(TVariant<Ts...>& Left, TVariant<Ts...>& Right) noexcept((std::is_nothrow_move_constructible_v<Ts> && ...))
    {
        TVariant<Ts...> Staged(std::move(Left));
        Left = std::move(Right);
        Right = std::move(Staged);
    }
}

namespace Lumina
{
    template <typename... Ts>
    struct TIsTriviallyRelocatable<Containers::TVariant<Ts...>>
    {
        static constexpr bool Value = (TIsTriviallyRelocatable_V<Ts> && ...);
    };
}
