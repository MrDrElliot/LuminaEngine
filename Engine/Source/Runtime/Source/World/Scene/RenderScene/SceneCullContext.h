#pragma once

#include "Containers/Array.h"
#include "Core/Math/Frustum.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    // Coarse per-frame rejection test built before the parallel mesh gather. Holds every volume an instance
    // could contribute to: camera frustum, sun-swept shadow frustum, and one sphere per shadow-casting light.
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

        // Camera frustum extruded by ShadowSweepDistance along SunDirection; encloses every sun-shadow caster.
        // Prebuilt here (vs after AllocateShadowTiles) so it's available during the parallel gather. Valid when bHasSun.
        FFrustum SunShadowFrustum;

        FVector3 SunDirection = FVector3(0.0f);

        // One entry per shadow-casting point/spot light, sized by the renderer before dispatch.
        TVector<FLightSphere> ShadowLights;

        // One frustum per active scene-capture view; an instance inside any capture frustum must still be
        // uploaded or the GPU per-view cull has nothing to draw for that view.
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

        // True if the sphere must be in this frame's instance upload; conservative (rejects only when outside
        // every sampling view). bCastsShadow=false skips shadow tests; MaxDrawDistance (0=off) adds a distance cut.
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

        // True only when the sphere is in the main camera view (frustum + draw distance), ignoring shadow
        // sweeps -- a shadow-only caster isn't "rendered", so anim systems can stop ticking its pose.
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

            // A mesh visible only in a preview/capture view is still "rendered", so its
            // pose must keep ticking or the preview shows a frozen animation.
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
