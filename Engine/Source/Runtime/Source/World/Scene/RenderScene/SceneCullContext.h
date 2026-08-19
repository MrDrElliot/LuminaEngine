#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Frustum.h"
#include "Core/Math/Math.h"

namespace Lumina
{
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
