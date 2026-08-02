#pragma once
#include "SIMDConfig.h"

// VFloat8: 8-lane f32 SIMD register (__m256, AVX), lanes [0..7] low-to-high.
// Comparisons return a per-lane mask; combine with Select/MoveMask/Any/All.

namespace Lumina::SIMD
{
    struct LUMINA_SIMD_ALIGN32 VFloat8
    {
        static constexpr int Width = 8;

        __m256 V;

        VFloat8() = default;
        FORCEINLINE VFloat8(__m256 In) : V(In) {}
        FORCEINLINE explicit VFloat8(float S) : V(_mm256_set1_ps(S)) {}
        FORCEINLINE VFloat8(float E0, float E1, float E2, float E3, float E4, float E5, float E6, float E7)
            : V(_mm256_setr_ps(E0, E1, E2, E3, E4, E5, E6, E7)) {}

        FORCEINLINE operator __m256() const { return V; }

        static FORCEINLINE VFloat8 Zero()                      { return _mm256_setzero_ps(); }
        static FORCEINLINE VFloat8 Broadcast(float S)          { return _mm256_set1_ps(S); }
        static FORCEINLINE VFloat8 Load(const float* P)        { return _mm256_loadu_ps(P); }
        static FORCEINLINE VFloat8 LoadAligned(const float* P) { return _mm256_load_ps(P); }

        FORCEINLINE void Store(float* P) const        { _mm256_storeu_ps(P, V); }
        FORCEINLINE void StoreAligned(float* P) const { _mm256_store_ps(P, V); }

        FORCEINLINE float operator[](int Lane) const
        {
            LUMINA_SIMD_ALIGN32 float Tmp[8];
            _mm256_store_ps(Tmp, V);
            return Tmp[Lane];
        }
    };

    [[nodiscard]] FORCEINLINE VFloat8 operator+(VFloat8 A, VFloat8 B) { return _mm256_add_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 operator-(VFloat8 A, VFloat8 B) { return _mm256_sub_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 operator*(VFloat8 A, VFloat8 B) { return _mm256_mul_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 operator/(VFloat8 A, VFloat8 B) { return _mm256_div_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 operator-(VFloat8 A)            { return _mm256_sub_ps(_mm256_setzero_ps(), A.V); }

    FORCEINLINE VFloat8& operator+=(VFloat8& A, VFloat8 B) { A = A + B; return A; }
    FORCEINLINE VFloat8& operator-=(VFloat8& A, VFloat8 B) { A = A - B; return A; }
    FORCEINLINE VFloat8& operator*=(VFloat8& A, VFloat8 B) { A = A * B; return A; }
    FORCEINLINE VFloat8& operator/=(VFloat8& A, VFloat8 B) { A = A / B; return A; }

    [[nodiscard]] FORCEINLINE VFloat8 Min(VFloat8 A, VFloat8 B) { return _mm256_min_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Max(VFloat8 A, VFloat8 B) { return _mm256_max_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Abs(VFloat8 A)           { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), A.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Sqrt(VFloat8 A)         { return _mm256_sqrt_ps(A.V); }

    [[nodiscard]] FORCEINLINE VFloat8 Reciprocal(VFloat8 A)     { return _mm256_div_ps(_mm256_set1_ps(1.0f), A.V); }
    [[nodiscard]] FORCEINLINE VFloat8 ReciprocalFast(VFloat8 A) { return _mm256_rcp_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat8 InvSqrt(VFloat8 A)        { return _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_sqrt_ps(A.V)); }
    [[nodiscard]] FORCEINLINE VFloat8 InvSqrtFast(VFloat8 A)    { return _mm256_rsqrt_ps(A.V); }

    [[nodiscard]] FORCEINLINE VFloat8 Floor(VFloat8 A)    { return _mm256_floor_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Ceil(VFloat8 A)     { return _mm256_ceil_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Round(VFloat8 A)    { return _mm256_round_ps(A.V, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    [[nodiscard]] FORCEINLINE VFloat8 Truncate(VFloat8 A) { return _mm256_round_ps(A.V, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); }

    // A*B + C, fused when the toolchain targets FMA (AVX2), otherwise mul+add.
    [[nodiscard]] FORCEINLINE VFloat8 MulAdd(VFloat8 A, VFloat8 B, VFloat8 C)
    {
    #if LUMINA_SIMD_HAS_FMA
        return _mm256_fmadd_ps(A.V, B.V, C.V);
    #else
        return _mm256_add_ps(_mm256_mul_ps(A.V, B.V), C.V);
    #endif
    }

    [[nodiscard]] FORCEINLINE VFloat8 CmpEq(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_EQ_OQ); }
    [[nodiscard]] FORCEINLINE VFloat8 CmpNe(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_NEQ_UQ); }
    [[nodiscard]] FORCEINLINE VFloat8 CmpLt(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_LT_OQ); }
    [[nodiscard]] FORCEINLINE VFloat8 CmpLe(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_LE_OQ); }
    [[nodiscard]] FORCEINLINE VFloat8 CmpGt(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_GT_OQ); }
    [[nodiscard]] FORCEINLINE VFloat8 CmpGe(VFloat8 A, VFloat8 B) { return _mm256_cmp_ps(A.V, B.V, _CMP_GE_OQ); }

    [[nodiscard]] FORCEINLINE VFloat8 And(VFloat8 A, VFloat8 B)    { return _mm256_and_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Or(VFloat8 A, VFloat8 B)     { return _mm256_or_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 Xor(VFloat8 A, VFloat8 B)    { return _mm256_xor_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat8 AndNot(VFloat8 A, VFloat8 B) { return _mm256_andnot_ps(A.V, B.V); } // ~A & B

    // Per lane: Mask high bit set -> A, else B.
    [[nodiscard]] FORCEINLINE VFloat8 Select(VFloat8 Mask, VFloat8 A, VFloat8 B) { return _mm256_blendv_ps(B.V, A.V, Mask.V); }

    [[nodiscard]] FORCEINLINE int  MoveMask(VFloat8 Mask) { return _mm256_movemask_ps(Mask.V); }
    [[nodiscard]] FORCEINLINE bool Any(VFloat8 Mask)      { return _mm256_movemask_ps(Mask.V) != 0; }
    [[nodiscard]] FORCEINLINE bool All(VFloat8 Mask)      { return _mm256_movemask_ps(Mask.V) == 0xFF; }
    [[nodiscard]] FORCEINLINE bool None(VFloat8 Mask)     { return _mm256_movemask_ps(Mask.V) == 0; }

    [[nodiscard]] FORCEINLINE float HorizontalSum(VFloat8 A)
    {
        __m128 Lo  = _mm256_castps256_ps128(A.V);
        __m128 Hi  = _mm256_extractf128_ps(A.V, 1);
        __m128 Sum = _mm_add_ps(Lo, Hi);
        Sum        = _mm_hadd_ps(Sum, Sum);
        Sum        = _mm_hadd_ps(Sum, Sum);
        return _mm_cvtss_f32(Sum);
    }

    [[nodiscard]] FORCEINLINE float HorizontalMin(VFloat8 A)
    {
        __m128 S = _mm_min_ps(_mm256_castps256_ps128(A.V), _mm256_extractf128_ps(A.V, 1));
        S        = _mm_min_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(2, 3, 0, 1)));
        S        = _mm_min_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(S);
    }

    [[nodiscard]] FORCEINLINE float HorizontalMax(VFloat8 A)
    {
        __m128 S = _mm_max_ps(_mm256_castps256_ps128(A.V), _mm256_extractf128_ps(A.V, 1));
        S        = _mm_max_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(2, 3, 0, 1)));
        S        = _mm_max_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(S);
    }
}
