#pragma once

#include <compare>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

#include "ContainerTraits.h"
#include "Memory/Construct.h"

namespace Lumina::Containers
{
    struct FNullOpt
    {
        explicit constexpr FNullOpt(int) noexcept {}
    };

    inline constexpr FNullOpt NullOpt{ 0 };

    struct FInPlace
    {
        explicit constexpr FInPlace() = default;
    };

    inline constexpr FInPlace InPlace{};

    namespace Private
    {
        template <typename T, bool bTriviallyDestructible = std::is_trivially_destructible_v<T>>
        struct TOptionalStorage
        {
            union
            {
                char Empty;
                T    Value;
            };
            bool bEngaged = false;

            constexpr TOptionalStorage() noexcept : Empty() {}

            template <typename... TArgs>
            constexpr explicit TOptionalStorage(FInPlace, TArgs&&... Args) : Value(std::forward<TArgs>(Args)...), bEngaged(true) {}

            ~TOptionalStorage()
            {
                if (bEngaged)
                {
                    Value.~T();
                }
            }
            TOptionalStorage(const TOptionalStorage&) = delete;
            TOptionalStorage& operator=(const TOptionalStorage&) = delete;
            TOptionalStorage(TOptionalStorage&&) = delete;
            TOptionalStorage& operator=(TOptionalStorage&&) = delete;
        };

        /** Keeps TOptional trivially destructible when T is, so it stays cheap in an array or a struct. */
        template <typename T>
        struct TOptionalStorage<T, true>
        {
            union
            {
                char Empty;
                T    Value;
            };
            bool bEngaged = false;

            constexpr TOptionalStorage() noexcept : Empty() {}

            // Value and flag built together, so the compiler can write them as one store a later whole-object copy reads back.
            template <typename... TArgs>
            constexpr explicit TOptionalStorage(FInPlace, TArgs&&... Args) : Value(std::forward<TArgs>(Args)...), bEngaged(true) {}

            ~TOptionalStorage() = default;
        };
    }

    /** A T that may or may not be there, with the T stored inline rather than on the heap. */
    template <typename T>
    requires(!std::is_reference_v<T>)
    class TOptional : private Private::TOptionalStorage<T>
    {
        using FStorage = Private::TOptionalStorage<T>;

        // Trivially copyable with such a T, so a moved-from one keeps its value as std::optional's does; any other T leaves it unset.
        static constexpr bool bTrivial = std::is_trivially_copyable_v<T>;

    public:

        using value_type = T;

        constexpr TOptional() noexcept = default;
        constexpr TOptional(FNullOpt) noexcept {}

        template <typename U = T>
        requires (std::is_constructible_v<T, U&&>
               && !std::is_same_v<std::remove_cvref_t<U>, TOptional>
               && !std::is_same_v<std::remove_cvref_t<U>, FNullOpt>
               && !std::is_same_v<std::remove_cvref_t<U>, FInPlace>)
        constexpr TOptional(U&& Value)
            : FStorage(InPlace, std::forward<U>(Value))
        {
        }

        template <typename... TArgs>
        requires std::is_constructible_v<T, TArgs...>
        constexpr explicit TOptional(FInPlace, TArgs&&... Args)
            : FStorage(InPlace, std::forward<TArgs>(Args)...)
        {
        }

        ~TOptional() = default;

        TOptional(const TOptional&) requires bTrivial = default;
        TOptional(TOptional&&) requires bTrivial = default;
        TOptional& operator=(const TOptional&) requires bTrivial = default;
        TOptional& operator=(TOptional&&) requires bTrivial = default;

        constexpr TOptional(const TOptional& Other) requires (!bTrivial)
        {
            if (Other.bEngaged)
            {
                Construct(Other.Value);
            }
        }

        constexpr TOptional(TOptional&& Other) noexcept(std::is_nothrow_move_constructible_v<T>) requires (!bTrivial)
        {
            if (Other.bEngaged)
            {
                Construct(std::move(Other.Value));
                Other.Reset();
            }
        }

        constexpr TOptional& operator=(const TOptional& Other) requires (!bTrivial)
        {
            if (this != &Other)
            {
                if (Other.bEngaged)
                {
                    AssignValue(Other.Value);
                }
                else
                {
                    Reset();
                }
            }
            return *this;
        }

        constexpr TOptional& operator=(TOptional&& Other)
            noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>) requires (!bTrivial)
        {
            if (this != &Other)
            {
                if (Other.bEngaged)
                {
                    AssignValue(std::move(Other.Value));
                    Other.Reset();
                }
                else
                {
                    Reset();
                }
            }
            return *this;
        }

        constexpr TOptional& operator=(FNullOpt) noexcept
        {
            Reset();
            return *this;
        }

        template <typename U = T>
        requires (std::is_constructible_v<T, U&&> && !std::is_same_v<std::remove_cvref_t<U>, TOptional>)
        constexpr TOptional& operator=(U&& Value)
        {
            AssignValue(std::forward<U>(Value));
            return *this;
        }

        NODISCARD FORCEINLINE constexpr bool IsSet() const noexcept { return this->bEngaged; }
        NODISCARD FORCEINLINE constexpr explicit operator bool() const noexcept { return this->bEngaged; }

        template <typename TSelf>
        NODISCARD FORCEINLINE constexpr auto&& GetValue(this TSelf&& Self) noexcept
        {
            LUMINA_CONTAINER_CHECK(Self.bEngaged);
            return std::forward_like<TSelf>(Self.Value);
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE constexpr auto&& operator*(this TSelf&& Self) noexcept
        {
            return std::forward<TSelf>(Self).GetValue();
        }

        /** The value when set, else Fallback. */
        template <typename TSelf, typename U>
        NODISCARD constexpr T Get(this TSelf&& Self, U&& Fallback)
        {
            return Self.bEngaged ? T(std::forward_like<TSelf>(Self.Value)) : static_cast<T>(std::forward<U>(Fallback));
        }

        template <typename TSelf>
        NODISCARD FORCEINLINE constexpr auto* operator->(this TSelf&& Self) noexcept
        {
            LUMINA_CONTAINER_CHECK(Self.bEngaged);
            return &Self.Value;
        }

        template <typename... TArgs>
        constexpr T& Emplace(TArgs&&... Args)
        {
            Reset();
            Construct(std::forward<TArgs>(Args)...);
            return this->Value;
        }

        constexpr void Reset() noexcept
        {
            if (this->bEngaged)
            {
                Memory::DestroyAt(&this->Value);
                this->bEngaged = false;
            }
        }

        friend constexpr void swap(TOptional& Left, TOptional& Right)
            noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T>)
        {
            if (Left.bEngaged && Right.bEngaged)
            {
                using std::swap;
                swap(Left.Value, Right.Value);
            }
            else if (Left.bEngaged)
            {
                Right.Construct(std::move(Left.Value));
                Left.Reset();
            }
            else if (Right.bEngaged)
            {
                Left.Construct(std::move(Right.Value));
                Right.Reset();
            }
        }

        NODISCARD friend constexpr bool operator==(const TOptional& Left, const TOptional& Right)
        {
            if (Left.bEngaged != Right.bEngaged)
            {
                return false;
            }
            return !Left.bEngaged || Left.Value == Right.Value;
        }

        NODISCARD friend constexpr bool operator==(const TOptional& Left, FNullOpt) { return !Left.bEngaged; }

        NODISCARD friend constexpr bool operator==(const TOptional& Left, const T& Right)
        {
            return Left.bEngaged && Left.Value == Right;
        }

    private:

        template <typename... TArgs>
        constexpr void Construct(TArgs&&... Args)
        {
            Memory::ConstructAt(&this->Value, std::forward<TArgs>(Args)...);
            this->bEngaged = true;
        }

        template <typename U>
        constexpr void AssignValue(U&& NewValue)
        {
            if (this->bEngaged)
            {
                this->Value = std::forward<U>(NewValue);
            }
            else
            {
                Construct(std::forward<U>(NewValue));
            }
        }
    };

    template <typename T>
    TOptional(T) -> TOptional<T>;

    template <typename T>
    NODISCARD constexpr TOptional<std::decay_t<T>> MakeOptional(T&& Value)
    {
        return TOptional<std::decay_t<T>>(std::forward<T>(Value));
    }

    template <typename T, typename... TArgs>
    NODISCARD constexpr TOptional<T> MakeOptional(TArgs&&... Args)
    {
        return TOptional<T>(InPlace, std::forward<TArgs>(Args)...);
    }
}

namespace Lumina
{
    template <typename T>
    struct TIsTriviallyRelocatable<Containers::TOptional<T>>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<T>;
    };
}
