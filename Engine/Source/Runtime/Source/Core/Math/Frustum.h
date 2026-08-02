#pragma once

#include "Core/Math/Vector/Vector.h"
#include "Core/Math/Matrix/Matrix.h"

#include "AABB.h"
#include "Containers/Array.h"
#include "Core/Profiler/Profile.h"
#include "Platform/Platform.h"

namespace Lumina
{
    // CPU-side frustum. Carries the 6 AoS planes PLUS a SoA mirror (SoaN*/SoaD, 8 lanes: 6 used +
    // 2 inert) so the per-object tests evaluate all planes at once, branchless. This is NOT the
    // GPU upload type -- the shader's `struct FFrustum { float4 Planes[6]; }` is mirrored by the
    // 96-byte FGPUFrustum (SceneRenderTypes.h); convert via AsGPU/FromGPU. Never embed FFrustum
    // (with its SoA tail) in a GPU-uploaded struct.
    struct FFrustum
    {
        enum ESide { LEFT = 0, RIGHT = 1, TOP = 2, BOTTOM = 3, BACK = 4, FRONT = 5, NUM = 6};

        TArray<FVector4, NUM> Planes;

        static constexpr int PlaneMask = (1 << NUM) - 1;
        float SoaNx[8] = {};
        float SoaNy[8] = {};
        float SoaNz[8] = {};
        float SoaD[8]  = {};

        // Repacks the AoS planes into the SoA mirror. Call after any direct edit to Planes.
        void RebuildSoA()
        {
            for (int i = 0; i < NUM; ++i)
            {
                SoaNx[i] = Planes[i].x;
                SoaNy[i] = Planes[i].y;
                SoaNz[i] = Planes[i].z;
                SoaD[i]  = Planes[i].w;
            }
            for (int i = NUM; i < 8; ++i)
            {
                SoaNx[i] = 0.0f;
                SoaNy[i] = 0.0f;
                SoaNz[i] = 0.0f;
                SoaD[i]  = 1.0f; // inert: positive distance, never rejects; masked out regardless
            }
        }

        // Signed distance of a SIMD center (x,y,z in lanes 0..2; w ignored) to all 8 planes.
        // Center stays register-resident -- lanes are broadcast via shuffles, no store/reload.
        FORCEINLINE SIMD::VFloat8 SignedDistances(SIMD::VFloat4 Center) const
        {
            using namespace SIMD;
            const __m128 Cx4 = SplatX(Center), Cy4 = SplatY(Center), Cz4 = SplatZ(Center);
            const VFloat8 Cx = _mm256_insertf128_ps(_mm256_castps128_ps256(Cx4), Cx4, 1);
            const VFloat8 Cy = _mm256_insertf128_ps(_mm256_castps128_ps256(Cy4), Cy4, 1);
            const VFloat8 Cz = _mm256_insertf128_ps(_mm256_castps128_ps256(Cz4), Cz4, 1);
            return MulAdd(VFloat8::Load(SoaNx), Cx,
                   MulAdd(VFloat8::Load(SoaNy), Cy,
                   MulAdd(VFloat8::Load(SoaNz), Cz, VFloat8::Load(SoaD))));
        }

        FORCEINLINE SIMD::VFloat8 SignedDistances(const FVector3& Point) const
        {
            using namespace SIMD;
            return MulAdd(VFloat8::Load(SoaNx), VFloat8::Broadcast(Point.x),
                   MulAdd(VFloat8::Load(SoaNy), VFloat8::Broadcast(Point.y),
                   MulAdd(VFloat8::Load(SoaNz), VFloat8::Broadcast(Point.z), VFloat8::Load(SoaD))));
        }

        /** Conservative sphere-vs-frustum; false positives near corners are acceptable for broad-phase. */
        NODISCARD bool IntersectsSphere(SIMD::VFloat4 Center, float Radius) const
        {
            using namespace SIMD;
            return (MoveMask(CmpLt(SignedDistances(Center), VFloat8::Broadcast(-Radius))) & PlaneMask) == 0;
        }

        NODISCARD bool IntersectsSphere(const FVector3& Center, float Radius) const
        {
            using namespace SIMD;
            return (MoveMask(CmpLt(SignedDistances(Center), VFloat8::Broadcast(-Radius))) & PlaneMask) == 0;
        }

        // Point-in-frustum is the radius-0 sphere test (reject on any negative signed distance).
        NODISCARD bool IsInside(SIMD::VFloat4 Point) const   { return IntersectsSphere(Point, 0.0f); }
        NODISCARD bool IsInside(const FVector3& Point) const { return IntersectsSphere(Point, 0.0f); }

        NODISCARD bool IsInside(const FAABB& aabb) const
        {
            using namespace SIMD;
            const VFloat8 Nx = VFloat8::Load(SoaNx);
            const VFloat8 Ny = VFloat8::Load(SoaNy);
            const VFloat8 Nz = VFloat8::Load(SoaNz);

            // Positive vertex per plane: pick Max where the normal component is >= 0, else Min.
            const VFloat8 Px = Select(CmpGe(Nx, VFloat8::Zero()), VFloat8::Broadcast(aabb.Max.x), VFloat8::Broadcast(aabb.Min.x));
            const VFloat8 Py = Select(CmpGe(Ny, VFloat8::Zero()), VFloat8::Broadcast(aabb.Max.y), VFloat8::Broadcast(aabb.Min.y));
            const VFloat8 Pz = Select(CmpGe(Nz, VFloat8::Zero()), VFloat8::Broadcast(aabb.Max.z), VFloat8::Broadcast(aabb.Min.z));

            const VFloat8 Dist = MulAdd(Nx, Px, MulAdd(Ny, Py, MulAdd(Nz, Pz, VFloat8::Load(SoaD))));
            return (MoveMask(CmpLt(Dist, VFloat8::Zero())) & PlaneMask) == 0;
        }

        /** Sweeps the frustum along SweepDir for shadow culling; SweepDir points toward the light. */
        NODISCARD FFrustum Extruded(const FVector3& SweepDir, float SweepDistance) const
        {
            FFrustum Out;
            for (int i = 0; i < NUM; ++i)
            {
                const FVector4& P = Planes[i];
                const FVector3 N(P.x, P.y, P.z);
                const float Push = Math::Max(0.0f, SweepDistance * Math::Dot(N, -SweepDir));
                Out.Planes[i] = FVector4(N, P.w + Push);
            }
            Out.RebuildSoA();
            return Out;
        }

        /** Extracts 6 normalized world-space planes; signed distance >= 0 inside. Matches GPU InFrustum convention. */
        static FFrustum FromViewProjection(const FMatrix4& VP)
        {
            FFrustum Out = {};
            Out.Planes[LEFT]   = FVector4(VP[0].w + VP[0].x, VP[1].w + VP[1].x, VP[2].w + VP[2].x, VP[3].w + VP[3].x);
            Out.Planes[RIGHT]  = FVector4(VP[0].w - VP[0].x, VP[1].w - VP[1].x, VP[2].w - VP[2].x, VP[3].w - VP[3].x);
            Out.Planes[TOP]    = FVector4(VP[0].w - VP[0].y, VP[1].w - VP[1].y, VP[2].w - VP[2].y, VP[3].w - VP[3].y);
            Out.Planes[BOTTOM] = FVector4(VP[0].w + VP[0].y, VP[1].w + VP[1].y, VP[2].w + VP[2].y, VP[3].w + VP[3].y);
            Out.Planes[BACK]   = FVector4(VP[0].w + VP[0].z, VP[1].w + VP[1].z, VP[2].w + VP[2].z, VP[3].w + VP[3].z);
            Out.Planes[FRONT]  = FVector4(VP[0].w - VP[0].z, VP[1].w - VP[1].z, VP[2].w - VP[2].z, VP[3].w - VP[3].z);

            for (int i = 0; i < NUM; ++i)
            {
                const FVector3 N(Out.Planes[i].x, Out.Planes[i].y, Out.Planes[i].z);
                const float Length = Math::Length(N);
                if (Length > 0.0f)
                {
                    Out.Planes[i] /= Length;
                }
            }
            Out.RebuildSoA();
            return Out;
        }

        static void ComputeFrustumCorners(const FMatrix4& ViewProjection, FVector3 OutCorners[8])
        {
            LUMINA_PROFILE_SCOPE();
            using namespace SIMD;

            const FMatrix4 InverseVP = Math::Inverse(ViewProjection);

            // Columns of the inverse VP, loaded once. NDC corner coords are all +/-1, so
            // each corner is C3 +/- C0 +/- C1 +/- C2.
            const VFloat4 C0 = VFloat4::Load(&InverseVP.Cols[0][0]);
            const VFloat4 C1 = VFloat4::Load(&InverseVP.Cols[1][0]);
            const VFloat4 C2 = VFloat4::Load(&InverseVP.Cols[2][0]);
            const VFloat4 C3 = VFloat4::Load(&InverseVP.Cols[3][0]);

            for (int z = 0; z < 2; ++z)
            {
                const VFloat4 Pz = z ? C3 + C2 : C3 - C2;
                for (int y = 0; y < 2; ++y)
                {
                    const VFloat4 Pzy = y ? Pz + C1 : Pz - C1;
                    for (int x = 0; x < 2; ++x)
                    {
                        const int Index = x + y * 2 + z * 4;

                        const VFloat4 Pt = x ? Pzy + C0 : Pzy - C0;
                        const VFloat4 Corner = Pt / SplatW(Pt);

                        LUMINA_SIMD_ALIGN16 float Tmp[4];
                        Corner.StoreAligned(Tmp);
                        OutCorners[Index] = FVector3(Tmp[0], Tmp[1], Tmp[2]);
                    }
                }
            }
        }
    };

    // 96-byte GPU upload mirror of the shader's `struct FFrustum { float4 Planes[6]; }`. The rich
    // CPU FFrustum (above) carries a SoA tail and must NEVER be embedded in a GPU struct; embed
    // this instead and convert with AsGPU/FromGPU (SceneRenderTypes.h).
    struct FGPUFrustum
    {
        FVector4 Planes[FFrustum::NUM];
    };
    static_assert(sizeof(FGPUFrustum) == 6 * sizeof(FVector4), "FGPUFrustum must match the shader's float4 Planes[6] (96 bytes)");
}
