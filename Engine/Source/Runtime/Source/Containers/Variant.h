#pragma once

#include <new>
#include "Containers/Tuple.h"
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"

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

        static constexpr size_t kCount   = sizeof...(Ts);
        static constexpr size_t kStorage = Private::MaxOf<sizeof(Ts)...>();
        static constexpr size_t kAlign   = Private::MaxOf<alignof(Ts)...>();

        template <size_t Index>
        using TAlternative = TTupleElementT<Index, TTuple<Ts...>>;

    public:

        static constexpr size_t kInvalidIndex = ~static_cast<size_t>(0);

        template <typename T>
        static constexpr size_t IndexOf = Private::IndexOfType<T, Ts...>();

        constexpr TVariant()
            requires std::is_default_constructible_v<TAlternative<0>>
        {
            new (Buffer) TAlternative<0>();
            Index = 0;
        }

        template <typename T, typename TDecayed = std::remove_cvref_t<T>>
        requires (IndexOf<TDecayed> != kInvalidIndex && !std::is_same_v<TDecayed, TVariant>)
        constexpr TVariant(T&& Value)
        {
            new (Buffer) TDecayed(std::forward<T>(Value));
            Index = IndexOf<TDecayed>;
        }

        template <size_t TargetIndex, typename... TArgs>
        constexpr explicit TVariant(std::in_place_index_t<TargetIndex>, TArgs&&... Args)
        {
            new (Buffer) TAlternative<TargetIndex>(std::forward<TArgs>(Args)...);
            Index = TargetIndex;
        }

        TVariant(const TVariant& Other)
        {
            if (Other.Index != kInvalidIndex)
            {
                CopyTable()[Other.Index](Buffer, Other.Buffer);
                Index = Other.Index;
            }
        }

        TVariant(TVariant&& Other) noexcept
        {
            if (Other.Index != kInvalidIndex)
            {
                MoveTable()[Other.Index](Buffer, Other.Buffer);
                Index = Other.Index;
                Other.Reset();
            }
        }

        ~TVariant() { Reset(); }

        TVariant& operator=(const TVariant& Other)
        {
            if (this != &Other)
            {
                Reset();
                if (Other.Index != kInvalidIndex)
                {
                    CopyTable()[Other.Index](Buffer, Other.Buffer);
                    Index = Other.Index;
                }
            }
            return *this;
        }

        TVariant& operator=(TVariant&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                if (Other.Index != kInvalidIndex)
                {
                    MoveTable()[Other.Index](Buffer, Other.Buffer);
                    Index = Other.Index;
                    Other.Reset();
                }
            }
            return *this;
        }

        template <typename T, typename TDecayed = std::remove_cvref_t<T>>
        requires (IndexOf<TDecayed> != kInvalidIndex && !std::is_same_v<TDecayed, TVariant>)
        TVariant& operator=(T&& Value)
        {
            Emplace<TDecayed>(std::forward<T>(Value));
            return *this;
        }

        NODISCARD FORCEINLINE constexpr size_t GetIndex() const noexcept { return Index; }
        NODISCARD FORCEINLINE constexpr size_t index() const noexcept { return Index; }

        /** False only after a throwing emplace left the variant with nothing in it. */
        NODISCARD FORCEINLINE constexpr bool IsValid() const noexcept { return Index != kInvalidIndex; }
        NODISCARD FORCEINLINE constexpr bool valueless_by_exception() const noexcept { return Index == kInvalidIndex; }

        template <typename T>
        NODISCARD FORCEINLINE constexpr bool Is() const noexcept { return Index == IndexOf<T>; }

        template <typename T>
        NODISCARD FORCEINLINE constexpr bool HoldsAlternative() const noexcept { return Is<T>(); }

        template <typename T, typename... TArgs>
        T& Emplace(TArgs&&... Args)
        {
            static_assert(IndexOf<T> != kInvalidIndex, "That type is not one of the variant alternatives.");
            Reset();
            T* Constructed = new (Buffer) T(std::forward<TArgs>(Args)...);
            Index = IndexOf<T>;
            return *Constructed;
        }

        template <size_t TargetIndex, typename... TArgs>
        TAlternative<TargetIndex>& Emplace(TArgs&&... Args)
        {
            return Emplace<TAlternative<TargetIndex>>(std::forward<TArgs>(Args)...);
        }

        template <typename T, typename... TArgs>
        FORCEINLINE T& emplace(TArgs&&... Args) { return Emplace<T>(std::forward<TArgs>(Args)...); }

        template <size_t TargetIndex, typename... TArgs>
        FORCEINLINE TAlternative<TargetIndex>& emplace(TArgs&&... Args)
        {
            return Emplace<TargetIndex>(std::forward<TArgs>(Args)...);
        }

        template <size_t TargetIndex>
        NODISCARD FORCEINLINE TAlternative<TargetIndex>& GetByIndex() & noexcept
        {
            LUMINA_CONTAINER_CHECK(Index == TargetIndex);
            return *reinterpret_cast<TAlternative<TargetIndex>*>(Buffer);
        }

        template <size_t TargetIndex>
        NODISCARD FORCEINLINE const TAlternative<TargetIndex>& GetByIndex() const& noexcept
        {
            LUMINA_CONTAINER_CHECK(Index == TargetIndex);
            return *reinterpret_cast<const TAlternative<TargetIndex>*>(Buffer);
        }

        template <size_t TargetIndex>
        NODISCARD FORCEINLINE TAlternative<TargetIndex>&& GetByIndex() && noexcept
        {
            LUMINA_CONTAINER_CHECK(Index == TargetIndex);
            return std::move(*reinterpret_cast<TAlternative<TargetIndex>*>(Buffer));
        }

        template <typename T>
        NODISCARD FORCEINLINE T& Get() & noexcept { return GetByIndex<IndexOf<T>>(); }

        template <typename T>
        NODISCARD FORCEINLINE const T& Get() const& noexcept { return GetByIndex<IndexOf<T>>(); }

        template <typename T>
        NODISCARD FORCEINLINE T&& Get() && noexcept { return std::move(*this).template GetByIndex<IndexOf<T>>(); }

        template <typename T>
        NODISCARD FORCEINLINE T* GetIf() noexcept
        {
            return Is<T>() ? reinterpret_cast<T*>(Buffer) : nullptr;
        }

        template <typename T>
        NODISCARD FORCEINLINE const T* GetIf() const noexcept
        {
            return Is<T>() ? reinterpret_cast<const T*>(Buffer) : nullptr;
        }

        void Reset() noexcept
        {
            if (Index != kInvalidIndex)
            {
                DestroyTable()[Index](Buffer);
                Index = kInvalidIndex;
            }
        }

        void swap(TVariant& Other) noexcept
        {
            TVariant Staged(std::move(*this));
            *this = std::move(Other);
            Other = std::move(Staged);
        }

        NODISCARD friend bool operator==(const TVariant& Left, const TVariant& Right)
        {
            if (Left.Index != Right.Index)
            {
                return false;
            }
            if (Left.Index == kInvalidIndex)
            {
                return true;
            }
            return EqualTable()[Left.Index](Left.Buffer, Right.Buffer);
        }

    private:

        using FDestroyFn = void (*)(void*) noexcept;
        using FCopyFn    = void (*)(void*, const void*);
        using FMoveFn    = void (*)(void*, void*);
        using FEqualFn   = bool (*)(const void*, const void*);

        // Function-pointer tables keep dispatch to one indexed indirect call, with no recursion over the pack.
        NODISCARD static const FDestroyFn* DestroyTable() noexcept
        {
            static constexpr FDestroyFn Table[kCount] =
            {
                +[](void* Target) noexcept { static_cast<Ts*>(Target)->~Ts(); }...
            };
            return Table;
        }

        NODISCARD static const FCopyFn* CopyTable() noexcept
        {
            static constexpr FCopyFn Table[kCount] =
            {
                +[](void* Target, const void* Source) { new (Target) Ts(*static_cast<const Ts*>(Source)); }...
            };
            return Table;
        }

        NODISCARD static const FMoveFn* MoveTable() noexcept
        {
            static constexpr FMoveFn Table[kCount] =
            {
                +[](void* Target, void* Source) { new (Target) Ts(std::move(*static_cast<Ts*>(Source))); }...
            };
            return Table;
        }

        NODISCARD static const FEqualFn* EqualTable() noexcept
        {
            static constexpr FEqualFn Table[kCount] =
            {
                +[](const void* Left, const void* Right)
                {
                    return *static_cast<const Ts*>(Left) == *static_cast<const Ts*>(Right);
                }...
            };
            return Table;
        }

        alignas(kAlign) unsigned char Buffer[kStorage];
        size_t Index = kInvalidIndex;
    };

    namespace Private
    {
        template <typename TVisitor, typename TVariantRef, size_t... Indices>
        constexpr decltype(auto) VisitImpl(TVisitor&& Visitor, TVariantRef&& Variant, std::index_sequence<Indices...>)
        {
            using FReturn = decltype(Visitor(std::forward<TVariantRef>(Variant).template GetByIndex<0>()));
            using FEntry = FReturn (*)(TVisitor&&, TVariantRef&&);

            static constexpr FEntry Table[] =
            {
                +[](TVisitor&& Vis, TVariantRef&& Var) -> FReturn
                {
                    return Vis(std::forward<TVariantRef>(Var).template GetByIndex<Indices>());
                }...
            };

            LUMINA_CONTAINER_CHECK(Variant.IsValid());
            return Table[Variant.GetIndex()](std::forward<TVisitor>(Visitor), std::forward<TVariantRef>(Variant));
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
        return Variant != nullptr ? Variant->template GetIf<T>() : nullptr;
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE const T* GetIf(const TVariant<Ts...>* Variant) noexcept
    {
        return Variant != nullptr ? Variant->template GetIf<T>() : nullptr;
    }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE bool HoldsAlternative(const TVariant<Ts...>& Variant) noexcept
    {
        return Variant.template Is<T>();
    }

    // Lowercase surface, so existing call sites that relied on argument-dependent lookup keep working.
    template <typename TArg, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) get(TVariant<Ts...>& Variant) noexcept { return Get<TArg>(Variant); }

    template <typename TArg, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) get(const TVariant<Ts...>& Variant) noexcept { return Get<TArg>(Variant); }

    template <size_t Index, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) get(TVariant<Ts...>& Variant) noexcept { return Get<Index>(Variant); }

    template <size_t Index, typename... Ts>
    NODISCARD FORCEINLINE decltype(auto) get(const TVariant<Ts...>& Variant) noexcept { return Get<Index>(Variant); }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE T* get_if(TVariant<Ts...>* Variant) noexcept { return GetIf<T>(Variant); }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE const T* get_if(const TVariant<Ts...>* Variant) noexcept { return GetIf<T>(Variant); }

    template <typename T, typename... Ts>
    NODISCARD FORCEINLINE bool holds_alternative(const TVariant<Ts...>& Variant) noexcept { return Variant.template Is<T>(); }

    template <typename TVisitor, typename... Ts>
    constexpr decltype(auto) visit(TVisitor&& Visitor, TVariant<Ts...>& Variant)
    {
        return Visit(std::forward<TVisitor>(Visitor), Variant);
    }

    template <typename TVisitor, typename... Ts>
    constexpr decltype(auto) visit(TVisitor&& Visitor, const TVariant<Ts...>& Variant)
    {
        return Visit(std::forward<TVisitor>(Visitor), Variant);
    }

    template <typename... Ts>
    FORCEINLINE void swap(TVariant<Ts...>& Left, TVariant<Ts...>& Right) noexcept
    {
        Left.swap(Right);
    }
}
