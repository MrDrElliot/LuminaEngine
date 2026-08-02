#pragma once

#include "Core/Math/Vector/Vector.h"
#include "Core/Math/SIMD/SIMD.h"
#include <type_traits>

// Lumina matrices. COLUMN-MAJOR storage (C columns, each a TVec<T,R>)

namespace Lumina
{
    // C columns, R rows -> mathematically an R-by-C matrix. m[c] is column c.
    template<typename T, int C, int R>
    struct TMat
    {
        static_assert(C > 0 && R > 0, "TMat requires positive dimensions");

        using ColumnType = TVec<T, R>;
        using ScalarType = T;
        static constexpr int Columns = C;
        static constexpr int Rows    = R;

        ColumnType Cols[C];

        // Trivial default ctor: uninitialized. Use TMat(1) or
        // TMat::Identity() for the identity matrix.
        TMat() = default;

        explicit constexpr TMat(T Diagonal) : Cols{}
        {
            constexpr int N = C < R ? C : R;
            for (int i = 0; i < N; ++i)
            {
                Cols[i][i] = Diagonal;
            }
        }

        // From C column vectors: TMat(col0, col1, ...).
        template<typename... Cs>
        requires (sizeof...(Cs) == C && C >= 2 && (std::is_convertible_v<Cs, ColumnType> && ...))
        constexpr TMat(const Cs&... InCols) : Cols{ ColumnType(InCols)... } {}

        // From C*R scalars in column-major order.
        template<typename... Ss>
        requires (sizeof...(Ss) == C * R && C * R != C && (std::is_convertible_v<Ss, T> && ...))
        constexpr TMat(Ss... InScalars)
        {
            const T Vals[C * R] = { static_cast<T>(InScalars)... };
            for (int c = 0; c < C; ++c)
            {
                for (int r = 0; r < R; ++r)
                {
                    Cols[c][r] = Vals[c * R + r];
                }
            }
        }

        template<typename U>
        explicit constexpr TMat(const TMat<U, C, R>& M)
        {
            for (int c = 0; c < C; ++c)
            {
                Cols[c] = ColumnType(M.Cols[c]);
            }
        }
        
        template<int C2, int R2>
        requires (C2 != C || R2 != R)
        explicit constexpr TMat(const TMat<T, C2, R2>& M) : TMat(T(1))
        {
            constexpr int CC = C < C2 ? C : C2;
            constexpr int RR = R < R2 ? R : R2;
            for (int c = 0; c < CC; ++c)
            {
                for (int r = 0; r < RR; ++r)
                {
                    Cols[c][r] = M.Cols[c][r];
                }
            }
        }

        constexpr ColumnType&       operator[](int i)       { return Cols[i]; }
        constexpr const ColumnType& operator[](int i) const { return Cols[i]; }

        // Contiguous column-major scalars, for GPU upload / interop.
        T*       GetData()       { return &Cols[0][0]; }
        const T* GetData() const { return &Cols[0][0]; }

        static constexpr TMat Identity() { return TMat(T(1)); }
    };
    
    // M (R x C) * column vector (C) -> column vector (R): linear combination of columns.
    template<typename T, int C, int R>
    constexpr TVec<T, R> operator*(const TMat<T, C, R>& M, const TVec<T, C>& V)
    {
        TVec<T, R> Result{};
        for (int c = 0; c < C; ++c)
        {
            Result += M.Cols[c] * V[c];
        }
        return Result;
    }

    // A (R x C) * B (C x C2) -> (R x C2). Each result column is A times B's column.
    template<typename T, int C, int R, int C2>
    constexpr TMat<T, C2, R> operator*(const TMat<T, C, R>& A, const TMat<T, C2, C>& B)
    {
        TMat<T, C2, R> Result(T(0));
        for (int j = 0; j < C2; ++j)
        {
            Result.Cols[j] = A * B.Cols[j];
        }
        return Result;
    }

    // SIMD fast paths for 4x4 float (hot case); exact-type overloads win over the generic
    // templates above and match the scalar result.
    [[nodiscard]] inline TVec<float, 4> operator*(const TMat<float, 4, 4>& M, const TVec<float, 4>& V)
    {
        using namespace SIMD;
        const VFloat4 C0 = VFloat4::Load(&M.Cols[0][0]);
        const VFloat4 C1 = VFloat4::Load(&M.Cols[1][0]);
        const VFloat4 C2 = VFloat4::Load(&M.Cols[2][0]);
        const VFloat4 C3 = VFloat4::Load(&M.Cols[3][0]);
        const VFloat4 Vv = VFloat4::Load(&V[0]);

        const VFloat4 R = MulAdd(SplatW(Vv), C3, MulAdd(SplatZ(Vv), C2, MulAdd(SplatY(Vv), C1, SplatX(Vv) * C0)));

        TVec<float, 4> Out;
        R.Store(&Out[0]);
        return Out;
    }

    [[nodiscard]] inline TMat<float, 4, 4> operator*(const TMat<float, 4, 4>& A, const TMat<float, 4, 4>& B)
    {
        using namespace SIMD;
        const VFloat4 A0 = VFloat4::Load(&A.Cols[0][0]);
        const VFloat4 A1 = VFloat4::Load(&A.Cols[1][0]);
        const VFloat4 A2 = VFloat4::Load(&A.Cols[2][0]);
        const VFloat4 A3 = VFloat4::Load(&A.Cols[3][0]);

        TMat<float, 4, 4> Out;
        for (int j = 0; j < 4; ++j)
        {
            const VFloat4 Bc = VFloat4::Load(&B.Cols[j][0]);
            const VFloat4 R  = MulAdd(SplatW(Bc), A3, MulAdd(SplatZ(Bc), A2, MulAdd(SplatY(Bc), A1, SplatX(Bc) * A0)));
            R.Store(&Out.Cols[j][0]);
        }
        return Out;
    }

    template<typename T, int C, int R>
    constexpr TMat<T, C, R> operator*(const TMat<T, C, R>& M, std::type_identity_t<T> S)
    {
        TMat<T, C, R> Result(T(0));
        for (int c = 0; c < C; ++c)
        {
            Result.Cols[c] = M.Cols[c] * S;
        }
        return Result;
    }

    template<typename T, int C, int R>
    constexpr TMat<T, C, R> operator*(std::type_identity_t<T> S, const TMat<T, C, R>& M)
    {
        return M * S;
    }

    template<typename T, int C, int R>
    constexpr TMat<T, C, R> operator+(const TMat<T, C, R>& A, const TMat<T, C, R>& B)
    {
        TMat<T, C, R> Result(T(0));
        for (int c = 0; c < C; ++c)
        {
            Result.Cols[c] = A.Cols[c] + B.Cols[c];
        }
        return Result;
    }

    template<typename T, int C, int R>
    constexpr TMat<T, C, R> operator-(const TMat<T, C, R>& A, const TMat<T, C, R>& B)
    {
        TMat<T, C, R> Result(T(0));
        for (int c = 0; c < C; ++c)
        {
            Result.Cols[c] = A.Cols[c] - B.Cols[c];
        }
        return Result;
    }

    template<typename T, int C, int R>
    constexpr bool operator==(const TMat<T, C, R>& A, const TMat<T, C, R>& B)
    {
        for (int c = 0; c < C; ++c)
        {
            if (A.Cols[c] != B.Cols[c]) { return false; }
        }
        return true;
    }

    template<typename T, int C, int R>
    constexpr bool operator!=(const TMat<T, C, R>& A, const TMat<T, C, R>& B)
    {
        return !(A == B);
    }
    
    using FMatrix2 = TMat<float, 2, 2>;
    using FMatrix3 = TMat<float, 3, 3>;
    using FMatrix4 = TMat<float, 4, 4>;
    using FMatrix  = FMatrix4;

    using FDoubleMatrix3 = TMat<double, 3, 3>;
    using FDoubleMatrix4 = TMat<double, 4, 4>;
}
