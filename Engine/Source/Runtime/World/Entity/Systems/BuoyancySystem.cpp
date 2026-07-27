#include "pch.h"
#include "BuoyancySystem.h"
#include "SystemContext.h"
#include "SystemResources.h"
#include "World/Entity/Components/BuoyancyComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    FSystemAccess SBuoyancySystem::Access = FSystemAccess{}
        .Read<SWaterComponent, STransformComponent, SBuoyancyComponent, SRigidBodyComponent>()
        .Write<SystemResource::PhysicsQuery>();

    void SBuoyancySystem::Startup(const FSystemContext& Context) noexcept
    {
        (void)Context.CreateView<SWaterComponent, STransformComponent>();
    }

    static float WaterGerstnerHeight(const SWaterComponent& W, float WorldX, float WorldZ, float Time)
    {
        float WindLen = Math::Sqrt(W.WindDirection.x * W.WindDirection.x + W.WindDirection.y * W.WindDirection.y);
        FVector2 Wind = (WindLen > 1e-4f)
            ? FVector2(W.WindDirection.x / WindLen, W.WindDirection.y / WindLen)
            : FVector2(1.0f, 0.0f);

        float Amplitude  = W.WaveAmplitude;
        float Scale      = Math::Max(W.WaveScale, 0.05f);
        int   Count      = Math::Clamp(W.WaveCount, 1, 8);
        float Wavelength = Math::Max(8.0f * Scale, 0.5f);
        float SpeedScale = 1.0f;

        float Y = 0.0f;
        for (int i = 0; i < Count; ++i)
        {
            constexpr float AngleStep = 0.45f;
            float Angle = AngleStep * ((float)i - 0.5f * (float)(Count - 1));
            float ca = Math::Cos(Angle);
            float sa = Math::Sin(Angle);
            float Dx = Wind.x * ca - Wind.y * sa;
            float Dz = Wind.x * sa + Wind.y * ca;

            float k     = 2.0f * LE_PI_F / Wavelength;
            float Speed = Math::Sqrt(9.81f / Math::Max(k, 1e-3f))
                        * (0.5f + 0.5f * Math::Clamp(W.WindSpeed * 0.1f, 0.0f, 1.0f)) * SpeedScale;
            float Phi   = k * (Dx * WorldX + Dz * WorldZ) + Time * Speed;
            Y += Amplitude * Math::Sin(Phi);

            Wavelength *= 0.62f;
            Amplitude  *= 0.62f;
            SpeedScale *= 1.16f;
        }
        return Y;
    }

    // Outward water-surface normal at (WorldX, WorldZ), from the Gerstner height gradient (central
    // differences). Lets a floating body tilt with the wave slope rather than only bobbing vertically.
    static FVector3 WaterGerstnerNormal(const SWaterComponent& W, float WorldX, float WorldZ, float Time)
    {
        constexpr float Eps = 0.5f;
        const float HxL = WaterGerstnerHeight(W, WorldX - Eps, WorldZ, Time);
        const float HxR = WaterGerstnerHeight(W, WorldX + Eps, WorldZ, Time);
        const float HzL = WaterGerstnerHeight(W, WorldX, WorldZ - Eps, Time);
        const float HzR = WaterGerstnerHeight(W, WorldX, WorldZ + Eps, Time);
        const float dHdx = (HxR - HxL) / (2.0f * Eps);
        const float dHdz = (HzR - HzL) / (2.0f * Eps);
        return Math::Normalize(FVector3(-dHdx, 1.0f, -dHdz));
    }

    void SBuoyancySystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        const float Time = (float)Context.GetTime();
        const float Dt   = (float)Context.GetDeltaTime();
        if (Dt <= 0.0f)
        {
            return;
        }

        // Snapshot the buoyant water planes (center + half extent + mean surface Y + component for the waves).
        struct FWaterPlane { const SWaterComponent* W; float Cx; float Cz; float HX; float HZ; float SurfaceY; };
        FWaterPlane Planes[16];
        int NumPlanes = 0;
        Context.CreateView<SWaterComponent, STransformComponent>().each(
            [&](const SWaterComponent& W, const STransformComponent& T)
            {
                if (!W.bBuoyancy || NumPlanes >= 16)
                {
                    return;
                }
                FVector3 C = T.GetWorldLocation();
                Planes[NumPlanes++] = { &W, C.x, C.z, W.Extent.x * 0.5f, W.Extent.y * 0.5f, C.y };
            });

        if (NumPlanes == 0)
        {
            return;
        }

        Context.CreateView<SBuoyancyComponent, SRigidBodyComponent>().each(
            [&](entt::entity Entity, const SBuoyancyComponent& B, const SRigidBodyComponent& RB)
            {
                if (RB.BodyID == 0xFFFFFFFFu)
                {
                    return;
                }

                const FVector3 BodyPos = Context.GetBodyPosition(Entity);

                // Pick the highest water plane this body is over in XZ.
                const FWaterPlane* Plane = nullptr;
                for (int p = 0; p < NumPlanes; ++p)
                {
                    const FWaterPlane& Pl = Planes[p];
                    if (Math::Abs(BodyPos.x - Pl.Cx) <= Pl.HX && Math::Abs(BodyPos.z - Pl.Cz) <= Pl.HZ)
                    {
                        if (!Plane || Pl.SurfaceY > Plane->SurfaceY)
                        {
                            Plane = &Pl;
                        }
                    }
                }
                if (!Plane)
                {
                    return;
                }

                // Sample the wave surface under the body; Jolt derives the submerged volume from the body's
                // real collider against this surface plane (so float depth + self-righting are shape-accurate).
                const float SurfaceY = Plane->SurfaceY + WaterGerstnerHeight(*Plane->W, BodyPos.x, BodyPos.z, Time);
                const FVector3 SurfacePos(BodyPos.x, SurfaceY, BodyPos.z);
                const FVector3 SurfaceNormal = WaterGerstnerNormal(*Plane->W, BodyPos.x, BodyPos.z, Time);

                Context.ApplyBuoyancyImpulse(Entity, SurfacePos, SurfaceNormal,
                    B.Buoyancy, B.LinearDrag, B.AngularDrag, FVector3(0.0f), Dt);
            });
    }
}
