#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Frustum.h"
#include "Core/Math/SIMD/SIMD.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    // Sphere-parallel: one plane broadcast across eight spheres, the transpose of the per-object test.
    FORCEINLINE uint32 FrustumInsideMask8(const FFrustum& F, SIMD::VFloat8 CX, SIMD::VFloat8 CY,
                                          SIMD::VFloat8 CZ, SIMD::VFloat8 R)
    {
        using namespace SIMD;

        const VFloat8 NegR = -R;
        VFloat8 Outside = VFloat8::Zero();

        for (int p = 0; p < FFrustum::NUM; ++p)
        {
            const VFloat8 Dist = MulAdd(VFloat8::Broadcast(F.Planes[p].x), CX,
                                 MulAdd(VFloat8::Broadcast(F.Planes[p].y), CY,
                                 MulAdd(VFloat8::Broadcast(F.Planes[p].z), CZ,
                                        VFloat8::Broadcast(F.Planes[p].w))));
            Outside = Or(Outside, CmpLt(Dist, NegR));
        }

        return (~(uint32)MoveMask(Outside)) & 0xFFu;
    }

    struct FSceneCullContext
    {
        /** World-space sphere for a shadow-casting light's influence region. */
        struct FLightSphere
        {
            FVector3   Center;
            float       Radius;
        };

        /** Main camera frustum. Anything inside this passes unconditionally. */
        FFrustum Frustum;

        FFrustum SunShadowFrustum;

        FVector3 SunDirection = FVector3(0.0f);

        // One entry per shadow-casting point/spot light, sized by the renderer before dispatch.
        TVector<FLightSphere> ShadowLights;

        TVector<FFrustum> CaptureFrusta;

        bool bEnabled  = true;
        bool bHasSun   = false;

        void Reset()
        {
            ShadowLights.clear();
            CaptureFrusta.clear();
            bEnabled = true;
            bHasSun  = false;
        }

        // Eight at once against the two frusta everything pays for; OutMaybe still needs the scalar path.
        FORCEINLINE void ShouldKeepBatch8(
            const float* RESTRICT CenterX,
            const float* RESTRICT CenterY,
            const float* RESTRICT CenterZ,
            const float* RESTRICT Radius,
            const float* RESTRICT MaxDrawDistance,
            uint32                CasterMask,
            const FVector3&       CameraPosition,
            uint32&               OutKeep,
            uint32&               OutMaybe) const
        {
            using namespace SIMD;

            const VFloat8 CX = VFloat8::Load(CenterX);
            const VFloat8 CY = VFloat8::Load(CenterY);
            const VFloat8 CZ = VFloat8::Load(CenterZ);
            const VFloat8 R  = VFloat8::Load(Radius);
            const VFloat8 MD = VFloat8::Load(MaxDrawDistance);

            // A zero draw distance means "never distance-culled", so only positive limits can reject.
            const VFloat8 DX = CX - VFloat8::Broadcast(CameraPosition.x);
            const VFloat8 DY = CY - VFloat8::Broadcast(CameraPosition.y);
            const VFloat8 DZ = CZ - VFloat8::Broadcast(CameraPosition.z);
            const VFloat8 DistSq = MulAdd(DX, DX, MulAdd(DY, DY, DZ * DZ));
            const VFloat8 CutOff = MD + R;
            const VFloat8 Limited = CmpGt(MD, VFloat8::Zero());
            const uint32  RejectMask = (uint32)MoveMask(And(Limited, CmpGt(DistSq, CutOff * CutOff)));

            const uint32 CameraMask = FrustumInsideMask8(Frustum, CX, CY, CZ, R);

            uint32 Keep = CameraMask;
            if (bHasSun)
            {
                Keep |= FrustumInsideMask8(SunShadowFrustum, CX, CY, CZ, R) & CasterMask;
            }

            OutKeep = Keep & ~RejectMask;

            // Anything still rejected has a second chance only if one of the rare lists is non-empty.
            const bool bHasExtras = !CaptureFrusta.empty() || !ShadowLights.empty();
            OutMaybe = bHasExtras ? (~OutKeep & ~RejectMask & 0xFFu) : 0u;
        }

        FORCEINLINE bool ShouldKeep(
            const FVector3& Center,
            float            Radius,
            bool             bCastsShadow,
            float            MaxDrawDistance,
            const FVector3& CameraPosition) const
        {
            if (!bEnabled)
            {
                return true;
            }

            if (MaxDrawDistance > 0.0f)
            {
                const FVector3 ToCamera = Center - CameraPosition;
                const float     DistSq   = Math::Dot(ToCamera, ToCamera);
                const float     CutOff   = MaxDrawDistance + Radius;
                if (DistSq > CutOff * CutOff)
                {
                    return false;
                }
            }

            if (Frustum.IntersectsSphere(Center, Radius))
            {
                return true;
            }

            // Visible to a preview/capture camera: keep regardless of shadow casting.
            for (const FFrustum& CaptureFrustum : CaptureFrusta)
            {
                if (CaptureFrustum.IntersectsSphere(Center, Radius))
                {
                    return true;
                }
            }

            if (!bCastsShadow)
            {
                return false;
            }

            if (bHasSun && SunShadowFrustum.IntersectsSphere(Center, Radius))
            {
                return true;
            }

            for (const FLightSphere& Light : ShadowLights)
            {
                const FVector3 Delta  = Center - Light.Center;
                const float     DistSq = Math::Dot(Delta, Delta);
                const float     Sum    = Radius + Light.Radius;
                if (DistSq <= Sum * Sum)
                {
                    return true;
                }
            }

            return false;
        }

        FORCEINLINE bool IsCameraVisible(
            const FVector3& Center,
            float            Radius,
            float            MaxDrawDistance,
            const FVector3& CameraPosition) const
        {
            if (!bEnabled)
            {
                return true;
            }

            if (MaxDrawDistance > 0.0f)
            {
                const FVector3 ToCamera = Center - CameraPosition;
                const float     DistSq   = Math::Dot(ToCamera, ToCamera);
                const float     CutOff   = MaxDrawDistance + Radius;
                if (DistSq > CutOff * CutOff)
                {
                    return false;
                }
            }

            if (Frustum.IntersectsSphere(Center, Radius))
            {
                return true;
            }

            for (const FFrustum& CaptureFrustum : CaptureFrusta)
            {
                if (CaptureFrustum.IntersectsSphere(Center, Radius))
                {
                    return true;
                }
            }

            return false;
        }
    };
}
