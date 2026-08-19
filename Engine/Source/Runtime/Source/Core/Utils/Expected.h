#pragma once

#include <utility>
#include "Core/Variant/Variant.h"
#include "Platform/Platform.h"

namespace Lumina
{
    struct InPlaceT { explicit InPlaceT() = default; };
    inline constexpr InPlaceT InPlace{};

    struct UnexpectT { explicit UnexpectT() = default; };
    inline constexpr UnexpectT Unexpect{};

    template<typename T, typename E>
    class TExpected;

    template<typename TError>
    class TUnexpected
    {
    public:
        
        ~TUnexpected() = default;
        
        constexpr TUnexpected(const TUnexpected&) = default;
        constexpr TUnexpected(TUnexpected&&) = default;

        template<typename Err = TError>
            requires(!std::is_same_v<std::remove_cvref_t<Err>, TUnexpected>)
        constexpr explicit TUnexpected(Err&& InError)
            : Error(std::forward<Err>(InError))
        {} 

        constexpr TUnexpected& operator=(const TUnexpected&) = default;
        constexpr TUnexpected& operator=(TUnexpected&&) = default;

        constexpr const TError& Value() const& noexcept { return Error; }
        constexpr TError& Value() & noexcept { return Error; }
        constexpr const TError&& Value() const&& noexcept { return std::move(Error); }
        constexpr TError&& Value() && noexcept { return std::move(Error); }

    private:
        TError Error;
    };

    template<typename TError>
    TUnexpected(TError) -> TUnexpected<TError>;

    template<typename T, typename E>
    class TExpected
    {
    public:
        using ValueType = T;
        using ErrorType = E;
        using UnexpectedType = TUnexpected<E>;

        constexpr TExpected() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::is_default_constructible_v<T>
            : Storage(std::in_place_index<0>)
        {
        }

        constexpr TExpected(const TExpected&)
            requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
        = default;

        constexpr TExpected(TExpected&&)
            noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
            requires(std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
        = default;

        template<typename U = T>
        constexpr explicit(!std::is_convertible_v<U, T>) TExpected(U&& InValue)
            noexcept(std::is_nothrow_constructible_v<T, U>)
            requires(!std::is_same_v<std::remove_cvref_t<U>, TExpected> &&
                     !std::is_same_v<std::remove_cvref_t<U>, InPlaceT> &&
                     !std::is_same_v<std::remove_cvref_t<U>, TUnexpected<E>> &&
                     std::is_constructible_v<T, U>)
            : Storage(std::in_place_index<0>, std::forward<U>(InValue))
        {
        }

        template<typename G>
        constexpr explicit(!std::is_convertible_v<const G&, E>) TExpected(const TUnexpected<G>& Unex)
            noexcept(std::is_nothrow_constructible_v<E, const G&>)
            requires std::is_constructible_v<E, const G&>
            : Storage(std::in_place_index<1>, Unex.Value())
        {
        }

        template<typename G>
        constexpr explicit(!std::is_convertible_v<G, E>) TExpected(TUnexpected<G>&& Unex)
            noexcept(std::is_nothrow_constructible_v<E, G>)
            requires std::is_constructible_v<E, G>
            : Storage(std::in_place_index<1>, std::move(Unex.Value()))
        {
        }

        template<typename... Args>
        constexpr explicit TExpected(InPlaceT, Args&&... InArgs)
            noexcept(std::is_nothrow_constructible_v<T, Args...>)
            requires std::is_constructible_v<T, Args...>
            : Storage(std::in_place_index<0>, std::forward<Args>(InArgs)...)
        {
        }

        template<typename... Args>
        constexpr explicit TExpected(UnexpectT, Args&&... InArgs)
            noexcept(std::is_nothrow_constructible_v<E, Args...>)
            requires std::is_constructible_v<E, Args...>
            : Storage(std::in_place_index<1>, std::forward<Args>(InArgs)...)
        {
        }

        ~TExpected() = default;

        constexpr TExpected& operator=(const TExpected&)
            requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T> &&
                     std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E>)
        = default;

        constexpr TExpected& operator=(TExpected&&)
            noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
                     std::is_nothrow_move_constructible_v<E> && std::is_nothrow_move_assignable_v<E>)
            requires(std::is_move_constructible_v<T> && std::is_move_assignable_v<T> &&
                     std::is_move_constructible_v<E> && std::is_move_assignable_v<E>)
        = default;

        template<typename U = T>
        constexpr TExpected& operator=(U&& InValue)
            requires(!std::is_same_v<std::remove_cvref_t<U>, TExpected> &&
                     !std::is_same_v<std::remove_cvref_t<U>, TUnexpected<E>> &&
                     std::is_constructible_v<T, U> &&
                     std::is_assignable_v<T&, U>)
        {
            Storage.template emplace<0>(std::forward<U>(InValue));
            return *this;
        }

        template<typename G>
        constexpr TExpected& operator=(const TUnexpected<G>& Unex)
        {
            Storage.template emplace<1>(Unex.Value());
            return *this;
        }

        template<typename G>
        constexpr TExpected& operator=(TUnexpected<G>&& Unex)
        {
            Storage.template emplace<1>(std::move(Unex.Value()));
            return *this;
        }

        NODISCARD constexpr const T* operator->() const noexcept 
        { 
            return &Containers::Get<0>(Storage); 
        }
        
        NODISCARD constexpr T* operator->() noexcept 
        { 
            return &Containers::Get<0>(Storage); 
        }

        NODISCARD constexpr const T& operator*() const& noexcept 
        { 
            return Containers::Get<0>(Storage); 
        }
        
        NODISCARD constexpr T& operator*() & noexcept 
        { 
            return Containers::Get<0>(Storage); 
        }
        
        NODISCARD constexpr const T&& operator*() const&& noexcept 
        { 
            return std::move(Containers::Get<0>(Storage)); 
        }
        
        NODISCARD constexpr T&& operator*() && noexcept 
        { 
            return std::move(Containers::Get<0>(Storage)); 
        }

        constexpr explicit operator bool() const noexcept 
        { 
            return Storage.index() == 0; 
        }

        NODISCARD constexpr bool HasValue() const noexcept 
        { 
            return Storage.index() == 0; 
        }

        NODISCARD constexpr const T& Value() const& 
        { 
            return Containers::Get<0>(Storage); 
        }
        
        NODISCARD constexpr T& Value() & 
        { 
            return Containers::Get<0>(Storage); 
        }
        
        NODISCARD constexpr const T&& Value() const&& 
        { 
            return std::move(Containers::Get<0>(Storage)); 
        }
        
        NODISCARD constexpr T&& Value() && 
        { 
            return std::move(Containers::Get<0>(Storage)); 
        }
        
        NODISCARD constexpr bool IsError() const noexcept
        {
            return Storage.index() == 1;
        }

        NODISCARD constexpr const E& Error() const& 
        { 
            return Containers::Get<1>(Storage); 
        }
        
        NODISCARD constexpr E& Error() & 
        { 
            return Containers::Get<1>(Storage); 
        }
        
        NODISCARD constexpr const E&& Error() const&& 
        { 
            return std::move(Containers::Get<1>(Storage)); 
        }
        
        NODISCARD constexpr E&& Error() && 
        { 
            return std::move(Containers::Get<1>(Storage)); 
        }

        template<typename U>
        NODISCARD constexpr T ValueOr(U&& DefaultValue) const&
        {
            return HasValue() ? **this : static_cast<T>(std::forward<U>(DefaultValue));
        }

        template<typename U>
        NODISCARD constexpr T ValueOr(U&& DefaultValue) &&
        {
            return HasValue() ? std::move(**this) : static_cast<T>(std::forward<U>(DefaultValue));
        }

        // Emplace
        template<typename... Args>
        NODISCARD constexpr T& Emplace(Args&&... InArgs)
            noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            return Storage.template emplace<0>(std::forward<Args>(InArgs)...);
        }

    private:
        TVariant<T, E> Storage;
    };

    template<typename T, typename E>
    constexpr bool operator==(const TExpected<T, E>& Lhs, const TExpected<T, E>& Rhs)
    {
        if (Lhs.HasValue() != Rhs.HasValue())
        {
            return false;
        }
        return Lhs.HasValue() ? *Lhs == *Rhs : Lhs.Error() == Rhs.Error();
    }

    template<typename T, typename E>
    constexpr bool operator!=(const TExpected<T, E>& Lhs, const TExpected<T, E>& Rhs)
    {
        return !(Lhs == Rhs);
    }

    template<typename T, typename E, typename U>
    constexpr bool operator==(const TExpected<T, E>& Exp, const U& Value)
    {
        return Exp.HasValue() && *Exp == Value;
    }

    template<typename T, typename E, typename G>
    constexpr bool operator==(const TExpected<T, E>& Exp, const TUnexpected<G>& Unex)
    {
        return !Exp.HasValue() && Exp.Error() == Unex.Value();
    }

}
