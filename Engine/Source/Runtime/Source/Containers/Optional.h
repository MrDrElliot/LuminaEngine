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
            ~TOptionalStorage()
            {
                if (bEngaged)
                {
                    Value.~T();
                }
            }
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
            ~TOptionalStorage() = default;
        };
    }

    /** A T that may or may not be there, with the T stored inline rather than on the heap. */
    template <typename T>
    requires(!std::is_reference_v<T>)
    class TOptional : private Private::TOptionalStorage<T>
    {
        using FStorage = Private::TOptionalStorage<T>;
    
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
        {
            Construct(std::forward<U>(Value));
        }

        template <typename... TArgs>
        requires std::is_constructible_v<T, TArgs...>
        constexpr explicit TOptional(FInPlace, TArgs&&... Args)
        {
            Construct(std::forward<TArgs>(Args)...);
        }

        TOptional(const TOptional& Other)
        {
            if (Other.bEngaged)
            {
                Construct(Other.Value);
            }
        }

        TOptional(TOptional&& Other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (Other.bEngaged)
            {
                Construct(std::move(Other.Value));
                Other.reset();
            }
        }

        TOptional& operator=(FNullOpt) noexcept
        {
            reset();
            return *this;
        }

        TOptional& operator=(const TOptional& Other)
        {
            if (this != &Other)
            {
                Assign(Other);
            }
            return *this;
        }

        TOptional& operator=(TOptional&& Other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (this != &Other)
            {
                if (Other.bEngaged)
                {
                    AssignValue(std::move(Other.Value));
                    Other.reset();
                }
                else
                {
                    reset();
                }
            }
            return *this;
        }

        template <typename U = T>
        requires (std::is_constructible_v<T, U&&> && !std::is_same_v<std::remove_cvref_t<U>, TOptional>)
        TOptional& operator=(U&& Value)
        {
            AssignValue(std::forward<U>(Value));
            return *this;
        }

        NODISCARD FORCEINLINE constexpr bool has_value() const noexcept { return this->bEngaged; }
        NODISCARD FORCEINLINE constexpr bool IsSet() const noexcept { return this->bEngaged; }
        NODISCARD FORCEINLINE constexpr explicit operator bool() const noexcept { return this->bEngaged; }

        NODISCARD FORCEINLINE constexpr T& value() & noexcept
        {
            LUMINA_CONTAINER_CHECK(this->bEngaged);
            return this->Value;
        }

        NODISCARD FORCEINLINE constexpr const T& value() const& noexcept
        {
            LUMINA_CONTAINER_CHECK(this->bEngaged);
            return this->Value;
        }

        NODISCARD FORCEINLINE constexpr T&& value() && noexcept
        {
            LUMINA_CONTAINER_CHECK(this->bEngaged);
            return std::move(this->Value);
        }

        NODISCARD FORCEINLINE constexpr T& GetValue() & noexcept { return value(); }
        NODISCARD FORCEINLINE constexpr const T& GetValue() const& noexcept { return value(); }

        template <typename U>
        NODISCARD constexpr T value_or(U&& Fallback) const&
        {
            return this->bEngaged ? this->Value : static_cast<T>(std::forward<U>(Fallback));
        }

        template <typename U>
        NODISCARD constexpr T value_or(U&& Fallback) &&
        {
            return this->bEngaged ? std::move(this->Value) : static_cast<T>(std::forward<U>(Fallback));
        }

        template <typename U>
        NODISCARD FORCEINLINE constexpr T Get(U&& Fallback) const { return value_or(std::forward<U>(Fallback)); }

        NODISCARD FORCEINLINE constexpr T& operator*() & noexcept { return value(); }
        NODISCARD FORCEINLINE constexpr const T& operator*() const& noexcept { return value(); }
        NODISCARD FORCEINLINE constexpr T&& operator*() && noexcept { return std::move(value()); }

        NODISCARD FORCEINLINE constexpr T* operator->() noexcept
        {
            LUMINA_CONTAINER_CHECK(this->bEngaged);
            return &this->Value;
        }

        NODISCARD FORCEINLINE constexpr const T* operator->() const noexcept
        {
            LUMINA_CONTAINER_CHECK(this->bEngaged);
            return &this->Value;
        }

        template <typename... TArgs>
        T& emplace(TArgs&&... Args)
        {
            reset();
            Construct(std::forward<TArgs>(Args)...);
            return this->Value;
        }

        template <typename... TArgs>
        FORCEINLINE T& Emplace(TArgs&&... Args) { return emplace(std::forward<TArgs>(Args)...); }

        void reset() noexcept
        {
            if (this->bEngaged)
            {
                this->Value.~T();
                this->bEngaged = false;
            }
        }

        FORCEINLINE void Reset() noexcept { reset(); }

        void swap(TOptional& Other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (this->bEngaged && Other.bEngaged)
            {
                using std::swap;
                swap(this->Value, Other.Value);
            }
            else if (this->bEngaged)
            {
                Other.Construct(std::move(this->Value));
                reset();
            }
            else if (Other.bEngaged)
            {
                Construct(std::move(Other.Value));
                Other.reset();
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
        void Construct(TArgs&&... Args)
        {
            Memory::ConstructAt(&this->Value, std::forward<TArgs>(Args)...);
            this->bEngaged = true;
        }

        void Assign(const TOptional& Other)
        {
            if (Other.bEngaged)
            {
                AssignValue(Other.Value);
            }
            else
            {
                reset();
            }
        }

        template <typename U>
        void AssignValue(U&& NewValue)
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

    template <typename T>
    FORCEINLINE void swap(TOptional<T>& Left, TOptional<T>& Right) noexcept(noexcept(Left.swap(Right)))
    {
        Left.swap(Right);
    }
}
