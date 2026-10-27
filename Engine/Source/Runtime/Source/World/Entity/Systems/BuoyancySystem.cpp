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

    void SBuoyancySystem::OnStartup(const SSystemContext& Context)
    {

        (void)Context.CreateView<SWaterComponent, STransformComponent>();
    }

    void SBuoyancySystem::OnUpdate(const SSystemContext& Context)
    {

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
                const float SurfaceY = Plane->SurfaceY + WaterWaveHeight(*Plane->W, BodyPos.x, BodyPos.z, Time);
                const FVector3 SurfacePos(BodyPos.x, SurfaceY, BodyPos.z);
                const FVector3 SurfaceNormal = WaterWaveNormal(*Plane->W, BodyPos.x, BodyPos.z, Time);

                Context.ApplyBuoyancyImpulse(Entity, SurfacePos, SurfaceNormal,
                    B.Buoyancy, B.LinearDrag, B.AngularDrag, FVector3(0.0f), Dt);
            });
    }
}
