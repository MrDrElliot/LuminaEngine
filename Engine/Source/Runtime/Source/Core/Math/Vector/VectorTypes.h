#pragma once

#include "Platform/GenericPlatform.h"
#include "Core/Reflection/ReflectionMacros.h"
#include <cstddef>
#include <type_traits>

// Lumina vector types. Left-handed.

#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4201) // nonstandard: nameless struct/union (the x/r/s aliasing)
#endif

namespace Lumina
{
    template<typename T, int N>
    requires (N > 0)
    struct TVec
    {
        using ScalarType = T;
        static constexpr int Dimensions = N;

        T Data[N];
        
        TVec() = default;

        explicit constexpr TVec(T Scalar) : Data{}
        {
            for (int i = 0; i < N; ++i)
            {
                Data[i] = Scalar;
            }
        }

        template<typename... Args>
        requires (sizeof...(Args) == N && sizeof...(Args) >= 2 && (std::is_arithmetic_v<Args> && ...))
        constexpr TVec(Args... InArgs) : Data{ static_cast<T>(InArgs)... } {}

        constexpr T&       operator[](int i)       { return Data[i]; }
        constexpr const T& operator[](int i) const { return Data[i]; }
    };

    template<typename T>
    struct TVec<T, 2>
    {
        using ScalarType = T;
        static constexpr int Dimensions = 2;

        union
        {
            struct
            {
                PROPERTY(Editable)
                T x;

                PROPERTY(Editable)
                T y;
            };
            struct { T r, g; };
            struct { T s, t; };
            T Data[2];
        };

        TVec() noexcept = default;
        explicit constexpr TVec(T Scalar) noexcept : x(Scalar), y(Scalar) {}

        // Per-component; accepts mixed/int args, the cast removes brace-narrowing.
        template<typename A, typename B>
        requires (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>)
        constexpr TVec(A InX, B InY) noexcept : x(T(InX)), y(T(InY)) {}

        // Implicit truncation from larger vectors.
        constexpr TVec(const TVec<T, 3>& V) : x(V.x), y(V.y) {}
        constexpr TVec(const TVec<T, 4>& V) : x(V.x), y(V.y) {}

        // Cross-precision conversion (implicit).
        template<typename U>
        constexpr TVec(const TVec<U, 2>& V) : x(T(V.x)), y(T(V.y)) {}

        constexpr T& operator[](int i)
        {
            switch (i) { case 0: return x; default: return y; }
        }
        constexpr const T& operator[](int i) const
        {
            switch (i) { case 0: return x; default: return y; }
        }
    };

    template<typename T>
    struct TVec<T, 3>
    {
        using ScalarType = T;
        static constexpr int Dimensions = 3;

        union
        {
            struct
            {
                PROPERTY(Editable)
                T x;

                PROPERTY(Editable)
                T y;

                PROPERTY(Editable)
                T z;
            };
            struct { T r, g, b; };
            struct { T s, t, p; };
            T Data[3];
        };

        TVec() noexcept = default;
        explicit constexpr TVec(T Scalar) noexcept : x(Scalar), y(Scalar), z(Scalar) {}

        template<typename A, typename B, typename C>
        requires (std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && std::is_arithmetic_v<C>)
        constexpr TVec(A InX, B InY, C InZ) : x(T(InX)), y(T(InY)), z(T(InZ)) {}

        constexpr TVec(const TVec<T, 2>& XY, T InZ) : x(XY.x), y(XY.y), z(InZ) {}
        constexpr TVec(const TVec<T, 4>& V) : x(V.x), y(V.y), z(V.z) {} // implicit truncate

        template<typename U>
        constexpr TVec(const TVec<U, 3>& V) : x(T(V.x)), y(T(V.y)), z(T(V.z)) {}

        constexpr T& operator[](int i)
        {
            switch (i) { case 0: return x; case 1: return y; default: return z; }
        }
        constexpr const T& operator[](int i) const
        {
            switch (i) { case 0: return x; case 1: return y; default: return z; }
        }
        
        constexpr T Right()   const { return x; }
        constexpr T Up()      const { return y; }
        constexpr T Forward() const { return z; }
    };

    template<typename T>
    struct TVec<T, 4>
    {
        using ScalarType = T;
        static constexpr int Dimensions = 4;

        union
        {
            struct
            {
                PROPERTY(Editable)
                T x;

                PROPERTY(Editable)
                T y;

                PROPERTY(Editable)
                T z;

                PROPERTY(Editable)
                T w;
            };
            struct { T r, g, b, a; };
            struct { T s, t, p, q; };
            T Data[4];
        };

        TVec() noexcept = default;
        explicit constexpr TVec(T Scalar) noexcept : x(Scalar), y(Scalar), z(Scalar), w(Scalar) {}

        template<typename A, typename B, typename C, typename D>
        requires (std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && std::is_arithmetic_v<C> && std::is_arithmetic_v<D>)
        constexpr TVec(A InX, B InY, C InZ, D InW) : x(T(InX)), y(T(InY)), z(T(InZ)), w(T(InW)) {}

        constexpr TVec(const TVec<T, 3>& XYZ, T InW) : x(XYZ.x), y(XYZ.y), z(XYZ.z), w(InW) {}
        constexpr TVec(const TVec<T, 2>& XY, T InZ, T InW) : x(XY.x), y(XY.y), z(InZ), w(InW) {}

        template<typename U>
        constexpr TVec(const TVec<U, 4>& V) : x(T(V.x)), y(T(V.y)), z(T(V.z)), w(T(V.w)) {}

        constexpr T& operator[](int i)
        {
            switch (i) { case 0: return x; case 1: return y; case 2: return z; default: return w; }
        }
        constexpr const T& operator[](int i) const
        {
            switch (i) { case 0: return x; case 1: return y; case 2: return z; default: return w; }
        }
    };

    // Anything that looks like a TVec: a scalar type and a compile-time dimension.
    template<typename V>
    concept CVec = requires
    {
        typename V::ScalarType;
        { V::Dimensions } -> std::convertible_to<int>;
    };

    // Scalar operand is non-deduced (type_identity_t) so `v * 2` works when T is float.

    #define LUMINA_VEC_BINARY_OP(Op)                                                                   \
        template<typename T, int N>                                                                    \
        constexpr TVec<T, N> operator Op (const TVec<T, N>& A, const TVec<T, N>& B)                     \
        { TVec<T, N> R{}; for (int i = 0; i < N; ++i) { R[i] = A[i] Op B[i]; } return R; }                \
        template<typename T, int N>                                                                    \
        constexpr TVec<T, N> operator Op (const TVec<T, N>& A, std::type_identity_t<T> S)               \
        { TVec<T, N> R{}; for (int i = 0; i < N; ++i) { R[i] = A[i] Op S; } return R; }                   \
        template<typename T, int N>                                                                    \
        constexpr TVec<T, N> operator Op (std::type_identity_t<T> S, const TVec<T, N>& B)               \
        { TVec<T, N> R{}; for (int i = 0; i < N; ++i) { R[i] = S Op B[i]; } return R; }

    LUMINA_VEC_BINARY_OP(+)
    LUMINA_VEC_BINARY_OP(-)
    LUMINA_VEC_BINARY_OP(*)
    LUMINA_VEC_BINARY_OP(/)
    #undef LUMINA_VEC_BINARY_OP

    #define LUMINA_VEC_ASSIGN_OP(Op)                                                                   \
        template<typename T, int N>                                                                    \
        constexpr TVec<T, N>& operator Op (TVec<T, N>& A, const TVec<T, N>& B)                          \
        { for (int i = 0; i < N; ++i) { A[i] Op B[i]; } return A; }                                     \
        template<typename T, int N>                                                                    \
        constexpr TVec<T, N>& operator Op (TVec<T, N>& A, std::type_identity_t<T> S)                    \
        { for (int i = 0; i < N; ++i) { A[i] Op S; } return A; }

    LUMINA_VEC_ASSIGN_OP(+=)
    LUMINA_VEC_ASSIGN_OP(-=)
    LUMINA_VEC_ASSIGN_OP(*=)
    LUMINA_VEC_ASSIGN_OP(/=)
    #undef LUMINA_VEC_ASSIGN_OP

    template<typename T, int N>
    constexpr TVec<T, N> operator-(const TVec<T, N>& V)
    {
        TVec<T, N> R{};
        for (int i = 0; i < N; ++i) { R[i] = -V[i]; }
        return R;
    }

    template<typename T, int N>
    constexpr bool operator==(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        for (int i = 0; i < N; ++i) { if (A[i] != B[i]) { return false; } }
        return true;
    }

    template<typename T, int N>
    constexpr bool operator!=(const TVec<T, N>& A, const TVec<T, N>& B)
    {
        return !(A == B);
    }

    REFLECT(NoCSharp, CSharpValueMirror)
    using FVector2 = TVec<float, 2>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FVector3 = TVec<float, 3>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FVector4 = TVec<float, 4>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FIntVector2 = TVec<int32, 2>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FIntVector3 = TVec<int32, 3>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FUIntVector2 = TVec<uint32, 2>;

    REFLECT(NoCSharp, CSharpValueMirror)
    using FUIntVector3 = TVec<uint32, 3>;

    using FIntVector4 = TVec<int32, 4>;

    using FUIntVector4 = TVec<uint32, 4>;

    using FU8Vector2 = TVec<uint8, 2>;
    using FU8Vector3 = TVec<uint8, 3>;
    using FU8Vector4 = TVec<uint8, 4>;

    using FU16Vector2 = TVec<uint16, 2>;
    using FU16Vector3 = TVec<uint16, 3>;
    using FU16Vector4 = TVec<uint16, 4>;

    using FDoubleVector2 = TVec<double, 2>;
    using FDoubleVector3 = TVec<double, 3>;
    using FDoubleVector4 = TVec<double, 4>;

}

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
