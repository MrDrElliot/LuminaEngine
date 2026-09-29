#include "RuntimePCH.h"
#include "Physics/PhysicsScene.h"
#include "BuoyancySystem.h"
#include "World/ECS/Registry.h"
#include "SystemContext.h"
#include "SystemResources.h"
#include "World/Entity/Components/BuoyancyComponent.h"
#include "World/Entity/Components/WaterComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    void SBuoyancySystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        Reads<SWaterComponent, STransformComponent, SBuoyancyComponent, SRigidBodyComponent>();
        Writes<SystemResource::PhysicsQuery>();
    }

    void SBuoyancySystem::OnStartup()
    {
        const FSystemContext& Context = GetContext();

        (void)Context.CreateView<SWaterComponent, STransformComponent>();
    }

    static float WaterGerstnerHeight(const SWaterComponent& W, float WorldX, float WorldZ, float Time)
    {
        float WindLen = Math::Sqrt(W.WindDirection.x * W.WindDirection.x + W.WindDirection.y * W.WindDirection.y);
        FVector2 Wind = (WindLen > 1e-4f)
            ? FVector2(W.WindDirection.x / WindLen, W.WindDirection.y / WindLen)
            : FVector2(1.0f, 0.0f);

        // The geometry waves of GetWaterWave in Includes/Water.slang, or floating bodies drift off the drawn surface.
        constexpr float MaxSteepness = 0.42f;
        constexpr float LengthRatio    = 0.86f;
        constexpr float AmplitudePower = 1.25f;
        const int       Count          = Math::Clamp(W.WaveCount, 1, 8) * 3;
        const float     WindFactor   = 0.5f + 0.5f * Math::Clamp(W.WindSpeed * 0.1f, 0.0f, 1.0f);

        float Y = 0.0f;
        for (int i = 0; i < Count; ++i)
        {
            const float Ratio  = Math::Pow(LengthRatio, (float)i);
            const float Spread = 1.0f + 1.6f * Math::Clamp((float)i / 15.0f, 0.0f, 1.0f);
            const float Hashed = (float)i * 0.6180339887f + 0.13f;
            const float Angle  = (Hashed - Math::Floor(Hashed) - 0.5f) * Spread;
            const float ca = Math::Cos(Angle);
            const float sa = Math::Sin(Angle);
            const float Dx = Wind.x * ca - Wind.y * sa;
            const float Dz = Wind.x * sa + Wind.y * ca;

            const float k       = 2.0f * LE_PI_F / (Math::Max(W.WaveLength, 0.5f) * Ratio);
            const float Omega   = Math::Sqrt(9.81f * k) * WindFactor;
            const float PhaseH  = (float)i * 0.7548776662f + 0.31f;
            const float Phase   = (PhaseH - Math::Floor(PhaseH)) * 2.0f * LE_PI_F;
            const float Phi     = k * (Dx * WorldX + Dz * WorldZ) - Omega * Time + Phase;
            Y += Math::Min(W.WaveAmplitude * Math::Pow(Ratio, AmplitudePower), MaxSteepness / k) * Math::Sin(Phi);
        }
        return Y;
    }

    // Outward water-surface normal from the Gerstner gradient, so a body tilts with the slope.
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

    void SBuoyancySystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();

        const float Time = (float)Context.GetTime();
        const float Dt   = (float)Context.GetDeltaTime();
        if (Dt <= 0.0f || Context.GetPhysicsScene() == nullptr)
        {
            return;
        }

        // Snapshot the buoyant water planes (center + half extent + mean surface Y + component for the waves).
        struct FWaterPlane { const SWaterComponent* W; float Cx; float Cz; float HalfX; float HalfZ; float SurfaceY; };
        FWaterPlane Planes[16];
        int NumPlanes = 0;
        Context.CreateView<SWaterComponent, STransformComponent>().ForEach(
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

        Context.CreateView<SBuoyancyComponent, SRigidBodyComponent>().ForEach(
            [&](ECS::FEntity Entity, const SBuoyancyComponent& B, const SRigidBodyComponent& RB)
            {
                if (Context.GetPhysicsScene()->GetBodyStatus(Entity) != Physics::EPhysicsBodyStatus::Ready)
                {
                    return;
                }

                const FVector3 BodyPos = Context.GetBodyPosition(Entity);

                // Pick the highest water plane this body is over in XZ.
                const FWaterPlane* Plane = nullptr;
                for (int p = 0; p < NumPlanes; ++p)
                {
                    const FWaterPlane& Pl = Planes[p];
                    if (Math::Abs(BodyPos.x - Pl.Cx) <= Pl.HalfX && Math::Abs(BodyPos.z - Pl.Cz) <= Pl.HalfZ)
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

                // Submerged volume comes from the body bounds against the fluid plane.
                const float SurfaceY = Plane->SurfaceY + WaterGerstnerHeight(*Plane->W, BodyPos.x, BodyPos.z, Time);
                const FVector3 SurfacePos(BodyPos.x, SurfaceY, BodyPos.z);
                const FVector3 SurfaceNormal = WaterGerstnerNormal(*Plane->W, BodyPos.x, BodyPos.z, Time);

                Context.ApplyBuoyancyImpulse(Entity, SurfacePos, SurfaceNormal,
                    B.Buoyancy, B.LinearDrag, B.AngularDrag, FVector3(0.0f), Dt);
            });
    }
}
