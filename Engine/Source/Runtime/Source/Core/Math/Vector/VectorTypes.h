#pragma once

#include "Platform/GenericPlatform.h"
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
            struct { T x, y; };
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
            struct { T x, y, z; };
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
            struct { T x, y, z, w; };
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

#ifndef REFLECTION_PARSER
    using FVector2 = TVec<float, 2>;
    using FVector3 = TVec<float, 3>;
    using FVector4 = TVec<float, 4>;

    // Hidden from the parser; reflected via the stub structs below.
    using FIntVector2 = TVec<int32, 2>;
    using FIntVector3 = TVec<int32, 3>;
    using FUIntVector2 = TVec<uint32, 2>;
    using FUIntVector3 = TVec<uint32, 3>;
#endif

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

#ifndef REFLECTION_PARSER
    // The REFLECT(ManualStub) shims at the bottom of this file describe these types to the
    // reflector field by field, because it cannot see through an alias to a template. Nothing about
    // that arrangement makes the description follow the type: TVec could gain a member or change
    // its scalar and the shims would keep declaring the old shape, which reflection would then use
    // to serialize and to drive the property editor. These assertions are what makes that drift a
    // compile error instead. Edit a stub and its assertion together, or neither.
    #define LUMINA_VERIFY_VECTOR_STUB(Type, Scalar, ExpectedSize)                                  \
        static_assert(sizeof(Type) == (ExpectedSize), #Type " no longer matches its REFLECT(ManualStub) shim: size changed."); \
        static_assert(std::is_same_v<Type::ScalarType, Scalar>, #Type " no longer matches its REFLECT(ManualStub) shim: scalar type changed."); \
        static_assert(offsetof(Type, x) == 0, #Type "::x moved; the shim declares it first.")

    LUMINA_VERIFY_VECTOR_STUB(FVector2, float, 8);
    LUMINA_VERIFY_VECTOR_STUB(FVector3, float, 12);
    LUMINA_VERIFY_VECTOR_STUB(FVector4, float, 16);
    LUMINA_VERIFY_VECTOR_STUB(FIntVector2, int32, 8);
    LUMINA_VERIFY_VECTOR_STUB(FIntVector3, int32, 12);
    LUMINA_VERIFY_VECTOR_STUB(FUIntVector2, uint32, 8);
    LUMINA_VERIFY_VECTOR_STUB(FUIntVector3, uint32, 12);

    #undef LUMINA_VERIFY_VECTOR_STUB

    // The shims declare y/z/w in order; a union member reordering would keep the sizes above but
    // silently renumber the reflected properties.
    static_assert(offsetof(FVector3, y) == 4 && offsetof(FVector3, z) == 8, "FVector3 member order drifted from its shim.");
    static_assert(offsetof(FVector4, y) == 4 && offsetof(FVector4, z) == 8 && offsetof(FVector4, w) == 12,
        "FVector4 member order drifted from its shim.");
#endif
}

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

// Reflection-parser-only shims for FVector2/3/4; ManualStub tells codegen to skip StaticStruct().
// REFLECT/PROPERTY defined locally (not via ObjectMacros.h) to avoid an include cycle through Math.
#ifdef REFLECTION_PARSER
#ifndef REFLECT
#define REFLECT(...)
#define PROPERTY(...)
#define FUNCTION(...)
#define GENERATED_BODY(...)
#endif
namespace Lumina
{
    REFLECT(ManualStub)
    struct FVector2
    {
        PROPERTY(Editable) float x;
        PROPERTY(Editable) float y;
    };

    REFLECT(ManualStub)
    struct FVector3
    {
        PROPERTY(Editable) float x;
        PROPERTY(Editable) float y;
        PROPERTY(Editable) float z;
    };

    REFLECT(ManualStub)
    struct FVector4
    {
        PROPERTY(Editable) float x;
        PROPERTY(Editable) float y;
        PROPERTY(Editable) float z;
        PROPERTY(Editable) float w;
    };

    REFLECT(ManualStub)
    struct FIntVector2
    {
        PROPERTY(Editable) int32 x;
        PROPERTY(Editable) int32 y;
    };

    REFLECT(ManualStub)
    struct FIntVector3
    {
        PROPERTY(Editable) int32 x;
        PROPERTY(Editable) int32 y;
        PROPERTY(Editable) int32 z;
    };

    REFLECT(ManualStub)
    struct FUIntVector2
    {
        PROPERTY(Editable) uint32 x;
        PROPERTY(Editable) uint32 y;
    };

    REFLECT(ManualStub)
    struct FUIntVector3
    {
        PROPERTY(Editable) uint32 x;
        PROPERTY(Editable) uint32 y;
        PROPERTY(Editable) uint32 z;
    };
}
#endif
