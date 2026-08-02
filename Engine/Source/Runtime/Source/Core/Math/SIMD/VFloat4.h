#pragma once
#include "SIMDConfig.h"

// VFloat4: 4-lane f32 SIMD register (__m128, SSE/SSE4.1), lanes [0..3] low-to-high.
// Comparisons return a per-lane mask; combine with Select/MoveMask/Any/All.

namespace Lumina::SIMD
{
    struct LUMINA_SIMD_ALIGN16 VFloat4
    {
        static constexpr int Width = 4;

        __m128 V;

        VFloat4() = default;
        FORCEINLINE VFloat4(__m128 In) : V(In) {}
        FORCEINLINE explicit VFloat4(float S) : V(_mm_set1_ps(S)) {}
        FORCEINLINE VFloat4(float E0, float E1, float E2, float E3) : V(_mm_setr_ps(E0, E1, E2, E3)) {}

        FORCEINLINE operator __m128() const { return V; }

        static FORCEINLINE VFloat4 Zero()                      { return _mm_setzero_ps(); }
        static FORCEINLINE VFloat4 Broadcast(float S)          { return _mm_set1_ps(S); }
        static FORCEINLINE VFloat4 Load(const float* P)        { return _mm_loadu_ps(P); }
        static FORCEINLINE VFloat4 LoadAligned(const float* P) { return _mm_load_ps(P); }

        FORCEINLINE void Store(float* P) const        { _mm_storeu_ps(P, V); }
        FORCEINLINE void StoreAligned(float* P) const { _mm_store_ps(P, V); }

        FORCEINLINE float operator[](int Lane) const
        {
            LUMINA_SIMD_ALIGN16 float Tmp[4];
            _mm_store_ps(Tmp, V);
            return Tmp[Lane];
        }
    };

    [[nodiscard]] FORCEINLINE VFloat4 operator+(VFloat4 A, VFloat4 B) { return _mm_add_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 operator-(VFloat4 A, VFloat4 B) { return _mm_sub_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 operator*(VFloat4 A, VFloat4 B) { return _mm_mul_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 operator/(VFloat4 A, VFloat4 B) { return _mm_div_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 operator-(VFloat4 A)            { return _mm_sub_ps(_mm_setzero_ps(), A.V); }

    FORCEINLINE VFloat4& operator+=(VFloat4& A, VFloat4 B) { A = A + B; return A; }
    FORCEINLINE VFloat4& operator-=(VFloat4& A, VFloat4 B) { A = A - B; return A; }
    FORCEINLINE VFloat4& operator*=(VFloat4& A, VFloat4 B) { A = A * B; return A; }
    FORCEINLINE VFloat4& operator/=(VFloat4& A, VFloat4 B) { A = A / B; return A; }

    [[nodiscard]] FORCEINLINE VFloat4 Min(VFloat4 A, VFloat4 B) { return _mm_min_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Max(VFloat4 A, VFloat4 B) { return _mm_max_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Abs(VFloat4 A)           { return _mm_andnot_ps(_mm_set1_ps(-0.0f), A.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Sqrt(VFloat4 A)         { return _mm_sqrt_ps(A.V); }

    // Accurate 1/x and 1/sqrt(x); the *Fast variants use the ~12-bit hardware
    // approximations (rcp/rsqrt) for when precision doesn't matter.
    [[nodiscard]] FORCEINLINE VFloat4 Reciprocal(VFloat4 A)     { return _mm_div_ps(_mm_set1_ps(1.0f), A.V); }
    [[nodiscard]] FORCEINLINE VFloat4 ReciprocalFast(VFloat4 A) { return _mm_rcp_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat4 InvSqrt(VFloat4 A)        { return _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(A.V)); }
    [[nodiscard]] FORCEINLINE VFloat4 InvSqrtFast(VFloat4 A)    { return _mm_rsqrt_ps(A.V); }

    [[nodiscard]] FORCEINLINE VFloat4 Floor(VFloat4 A)    { return _mm_floor_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Ceil(VFloat4 A)     { return _mm_ceil_ps(A.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Round(VFloat4 A)    { return _mm_round_ps(A.V, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
    [[nodiscard]] FORCEINLINE VFloat4 Truncate(VFloat4 A) { return _mm_round_ps(A.V, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC); }

    // A*B + C, fused when the toolchain targets FMA, otherwise mul+add.
    [[nodiscard]] FORCEINLINE VFloat4 MulAdd(VFloat4 A, VFloat4 B, VFloat4 C)
    {
    #if LUMINA_SIMD_HAS_FMA
        return _mm_fmadd_ps(A.V, B.V, C.V);
    #else
        return _mm_add_ps(_mm_mul_ps(A.V, B.V), C.V);
    #endif
    }

    [[nodiscard]] FORCEINLINE VFloat4 CmpEq(VFloat4 A, VFloat4 B) { return _mm_cmpeq_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 CmpNe(VFloat4 A, VFloat4 B) { return _mm_cmpneq_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 CmpLt(VFloat4 A, VFloat4 B) { return _mm_cmplt_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 CmpLe(VFloat4 A, VFloat4 B) { return _mm_cmple_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 CmpGt(VFloat4 A, VFloat4 B) { return _mm_cmpgt_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 CmpGe(VFloat4 A, VFloat4 B) { return _mm_cmpge_ps(A.V, B.V); }

    [[nodiscard]] FORCEINLINE VFloat4 And(VFloat4 A, VFloat4 B)    { return _mm_and_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Or(VFloat4 A, VFloat4 B)     { return _mm_or_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 Xor(VFloat4 A, VFloat4 B)    { return _mm_xor_ps(A.V, B.V); }
    [[nodiscard]] FORCEINLINE VFloat4 AndNot(VFloat4 A, VFloat4 B) { return _mm_andnot_ps(A.V, B.V); } // ~A & B

    // Per lane: Mask high bit set -> A, else B.
    [[nodiscard]] FORCEINLINE VFloat4 Select(VFloat4 Mask, VFloat4 A, VFloat4 B) { return _mm_blendv_ps(B.V, A.V, Mask.V); }

    [[nodiscard]] FORCEINLINE int  MoveMask(VFloat4 Mask) { return _mm_movemask_ps(Mask.V); }
    [[nodiscard]] FORCEINLINE bool Any(VFloat4 Mask)      { return _mm_movemask_ps(Mask.V) != 0; }
    [[nodiscard]] FORCEINLINE bool All(VFloat4 Mask)      { return _mm_movemask_ps(Mask.V) == 0xF; }
    [[nodiscard]] FORCEINLINE bool None(VFloat4 Mask)     { return _mm_movemask_ps(Mask.V) == 0; }

    [[nodiscard]] FORCEINLINE float HorizontalSum(VFloat4 A)
    {
        __m128 Shuf = _mm_movehdup_ps(A.V);     // [1,1,3,3]
        __m128 Sum  = _mm_add_ps(A.V, Shuf);
        Shuf        = _mm_movehl_ps(Shuf, Sum);
        Sum         = _mm_add_ss(Sum, Shuf);
        return _mm_cvtss_f32(Sum);
    }

    [[nodiscard]] FORCEINLINE float HorizontalMin(VFloat4 A)
    {
        __m128 S = _mm_min_ps(A.V, _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(2, 3, 0, 1)));
        S        = _mm_min_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(S);
    }

    [[nodiscard]] FORCEINLINE float HorizontalMax(VFloat4 A)
    {
        __m128 S = _mm_max_ps(A.V, _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(2, 3, 0, 1)));
        S        = _mm_max_ps(S, _mm_shuffle_ps(S, S, _MM_SHUFFLE(1, 0, 3, 2)));
        return _mm_cvtss_f32(S);
    }

    // 4-component dot product (uses SSE4.1 dpps), broadcast then extracted.
    [[nodiscard]] FORCEINLINE float Dot(VFloat4 A, VFloat4 B) { return _mm_cvtss_f32(_mm_dp_ps(A.V, B.V, 0xFF)); }

    // Broadcast one lane across all four (for matrix-vector linear combinations).
    [[nodiscard]] FORCEINLINE VFloat4 SplatX(VFloat4 A) { return _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(0, 0, 0, 0)); }
    [[nodiscard]] FORCEINLINE VFloat4 SplatY(VFloat4 A) { return _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(1, 1, 1, 1)); }
    [[nodiscard]] FORCEINLINE VFloat4 SplatZ(VFloat4 A) { return _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(2, 2, 2, 2)); }
    [[nodiscard]] FORCEINLINE VFloat4 SplatW(VFloat4 A) { return _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(3, 3, 3, 3)); }

    // Lane-indexed permute/shuffle. Permute<L0,L1,L2,L3>(A) = { A[L0], A[L1], A[L2], A[L3] }.
    // Shuffle takes lanes 0,1 from A and lanes 2,3 from B: { A[L0], A[L1], B[L2], B[L3] }.
    template<int L0, int L1, int L2, int L3>
    [[nodiscard]] FORCEINLINE VFloat4 Permute(VFloat4 A) { return _mm_shuffle_ps(A.V, A.V, _MM_SHUFFLE(L3, L2, L1, L0)); }

    template<int L0, int L1, int L2, int L3>
    [[nodiscard]] FORCEINLINE VFloat4 Shuffle(VFloat4 A, VFloat4 B) { return _mm_shuffle_ps(A.V, B.V, _MM_SHUFFLE(L3, L2, L1, L0)); }
}
