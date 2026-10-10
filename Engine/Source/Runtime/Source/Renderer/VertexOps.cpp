#include "VertexOps.h"

#include "Core/Math/Packing.h"
#include "Core/Math/SIMD/SIMDConfig.h"
#include "Renderer/Vertex.h"

namespace Lumina::VertexOps
{
    namespace
    {
        constexpr size_t kWidth = 8;

        FORCEINLINE __m256 Abs(__m256 V)
        {
            return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), V);
        }

        // Negative zero picks one, as the scalar helpers' greater-or-equal test does.
        FORCEINLINE __m256 SignOrOne(__m256 V)
        {
            return _mm256_blendv_ps(_mm256_set1_ps(-1.0f), _mm256_set1_ps(1.0f), _mm256_cmp_ps(V, _mm256_setzero_ps(), _CMP_GE_OQ));
        }

        // Math::Clamp's comparisons in the same order, so a NaN passes through exactly as it does there.
        FORCEINLINE __m256 Clamp(__m256 V, float Lo, float Hi)
        {
            const __m256 VLo = _mm256_set1_ps(Lo);
            const __m256 VHi = _mm256_set1_ps(Hi);
            V = _mm256_blendv_ps(V, VLo, _mm256_cmp_ps(V, VLo, _CMP_LT_OQ));
            return _mm256_blendv_ps(V, VHi, _mm256_cmp_ps(VHi, V, _CMP_LT_OQ));
        }

        // std::round takes ties away from zero where the hardware rounding mode takes them to even.
        FORCEINLINE __m256 RoundHalfAwayFromZero(__m256 V)
        {
            const __m256 Truncated = _mm256_round_ps(V, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
            const __m256 RoundsAway = _mm256_cmp_ps(Abs(_mm256_sub_ps(V, Truncated)), _mm256_set1_ps(0.5f), _CMP_GE_OQ);
            return _mm256_add_ps(Truncated, _mm256_and_ps(RoundsAway, SignOrOne(V)));
        }

        struct FOctahedral
        {
            __m256 X;
            __m256 Y;
        };

        FORCEINLINE FOctahedral OctahedralEncode(__m256 X, __m256 Y, __m256 Z)
        {
            const __m256 Sum = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(Abs(X), Abs(Y)), Abs(Z)), _mm256_set1_ps(1e-12f));
            X = _mm256_div_ps(X, Sum);
            Y = _mm256_div_ps(Y, Sum);
            Z = _mm256_div_ps(Z, Sum);

            const __m256 One = _mm256_set1_ps(1.0f);
            const __m256 Folded = _mm256_cmp_ps(Z, _mm256_setzero_ps(), _CMP_LT_OQ);
            const __m256 FoldX = _mm256_mul_ps(_mm256_sub_ps(One, Abs(Y)), SignOrOne(X));
            const __m256 FoldY = _mm256_mul_ps(_mm256_sub_ps(One, Abs(X)), SignOrOne(Y));
            return { _mm256_blendv_ps(X, FoldX, Folded), _mm256_blendv_ps(Y, FoldY, Folded) };
        }

        FORCEINLINE __m256i Quantize(__m256 V, float Scale)
        {
            return _mm256_cvttps_epi32(RoundHalfAwayFromZero(_mm256_mul_ps(Clamp(V, -1.0f, 1.0f), _mm256_set1_ps(Scale))));
        }

        struct FUnitVectors
        {
            __m256 X;
            __m256 Y;
            __m256 Z;
        };

        FORCEINLINE FUnitVectors DecodeNormals(const uint32* Packed)
        {
            const __m256i Bits = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(Packed));
            const __m256 Range = _mm256_set1_ps(32767.0f);
            const __m256 Ex = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(Bits, 16), 16)), Range);
            const __m256 Ey = _mm256_div_ps(_mm256_cvtepi32_ps(_mm256_srai_epi32(Bits, 16)), Range);

            const __m256 One = _mm256_set1_ps(1.0f);
            const __m256 Z = _mm256_sub_ps(_mm256_sub_ps(One, Abs(Ex)), Abs(Ey));
            const __m256 Folded = _mm256_cmp_ps(Z, _mm256_setzero_ps(), _CMP_LT_OQ);
            const __m256 X = _mm256_blendv_ps(Ex, _mm256_mul_ps(_mm256_sub_ps(One, Abs(Ey)), SignOrOne(Ex)), Folded);
            const __m256 Y = _mm256_blendv_ps(Ey, _mm256_mul_ps(_mm256_sub_ps(One, Abs(Ex)), SignOrOne(Ey)), Folded);

            // Unfused, matching Math::Normalize's multiply-then-add dot product and reciprocal scale.
            const __m256 LengthSquared = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(X, X), _mm256_mul_ps(Y, Y)), _mm256_mul_ps(Z, Z));
            const __m256 Length = _mm256_sqrt_ps(LengthSquared);
            const __m256 InvLength = _mm256_div_ps(One, Length);
            const __m256 NonZero = _mm256_cmp_ps(Length, _mm256_setzero_ps(), _CMP_GT_OQ);
            return { _mm256_and_ps(_mm256_mul_ps(X, InvLength), NonZero),
                     _mm256_and_ps(_mm256_mul_ps(Y, InvLength), NonZero),
                     _mm256_and_ps(_mm256_mul_ps(Z, InvLength), NonZero) };
        }

        FORCEINLINE __m256i StrideIndices(int Stride)
        {
            return _mm256_mullo_epi32(_mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7), _mm256_set1_epi32(Stride));
        }
    }

    void UnpackNormals(const uint32* Packed, FVector3* Out, size_t Count)
    {
        size_t i = 0;
        for (; i + kWidth <= Count; i += kWidth)
        {
            const FUnitVectors N = DecodeNormals(Packed + i);
            alignas(32) float X[kWidth], Y[kWidth], Z[kWidth];
            _mm256_store_ps(X, N.X);
            _mm256_store_ps(Y, N.Y);
            _mm256_store_ps(Z, N.Z);
            for (size_t Lane = 0; Lane < kWidth; ++Lane)
            {
                Out[i + Lane] = FVector3(X[Lane], Y[Lane], Z[Lane]);
            }
        }

        for (; i < Count; ++i)
        {
            Out[i] = UnpackNormal(Packed[i]);
        }
    }

    void PackNormals(const FVector3* Normals, uint32* Out, size_t Count)
    {
        const __m256i Stride3 = StrideIndices(3);
        const __m256i Low16 = _mm256_set1_epi32(0xFFFF);

        size_t i = 0;
        for (; i + kWidth <= Count; i += kWidth)
        {
            const float* Base = &Normals[i].x;
            const FOctahedral E = OctahedralEncode(_mm256_i32gather_ps(Base, Stride3, 4),
                                                  _mm256_i32gather_ps(Base + 1, Stride3, 4),
                                                  _mm256_i32gather_ps(Base + 2, Stride3, 4));
            const __m256i Qx = _mm256_and_si256(Quantize(E.X, 32767.0f), Low16);
            const __m256i Qy = _mm256_and_si256(Quantize(E.Y, 32767.0f), Low16);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(Out + i), _mm256_or_si256(Qx, _mm256_slli_epi32(Qy, 16)));
        }

        for (; i < Count; ++i)
        {
            Out[i] = PackNormal(Normals[i]);
        }
    }

    void PackTangents(const float* TangentsXYZW, uint32* Out, size_t Count)
    {
        const __m256i Stride4 = StrideIndices(4);
        const __m256i Low15 = _mm256_set1_epi32(0x7FFF);
        const __m256i RightHanded = _mm256_set1_epi32(1 << 30);

        size_t i = 0;
        for (; i + kWidth <= Count; i += kWidth)
        {
            const float* Base = TangentsXYZW + i * 4;
            const FOctahedral E = OctahedralEncode(_mm256_i32gather_ps(Base, Stride4, 4),
                                                  _mm256_i32gather_ps(Base + 1, Stride4, 4),
                                                  _mm256_i32gather_ps(Base + 2, Stride4, 4));
            const __m256 Sign = _mm256_i32gather_ps(Base + 3, Stride4, 4);

            const __m256i Qx = _mm256_and_si256(Quantize(E.X, 16383.0f), Low15);
            const __m256i Qy = _mm256_and_si256(Quantize(E.Y, 16383.0f), Low15);
            const __m256i Hand = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(Sign, _mm256_setzero_ps(), _CMP_GE_OQ)), RightHanded);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(Out + i), _mm256_or_si256(_mm256_or_si256(Qx, _mm256_slli_epi32(Qy, 15)), Hand));
        }

        for (; i < Count; ++i)
        {
            const float* T = TangentsXYZW + i * 4;
            Out[i] = PackTangent(FVector3(T[0], T[1], T[2]), T[3]);
        }
    }

    void UnpackHalf2x16s(const uint32* Packed, FVector2* Out, size_t Count)
    {
        // Four packed pairs are eight halves, which widen to eight floats laid out exactly as four FVector2.
        constexpr size_t kPairsPerStep = 4;

        size_t i = 0;
        for (; i + kPairsPerStep <= Count; i += kPairsPerStep)
        {
            const __m128i Halves = _mm_loadu_si128(reinterpret_cast<const __m128i*>(Packed + i));
            _mm256_storeu_ps(&Out[i].x, _mm256_cvtph_ps(Halves));
        }

        for (; i < Count; ++i)
        {
            Out[i] = Math::UnpackHalf2x16(Packed[i]);
        }
    }
}
