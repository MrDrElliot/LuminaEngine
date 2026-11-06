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
    namespace BuoyancySystemInternal
    {
        // A buoyant water plane: center, half extent, mean surface height, and the component for the waves.
        struct FWaterPlane
        {
            const SWaterComponent* W;
            float Cx;
            float Cz;
            float HalfX;
            float HalfZ;
            float SurfaceY;
        };
    }

    void CBuoyancySystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
    }

    void CBuoyancySystem::OnUpdate(const SSystemContext& Context)
    {
        using BuoyancySystemInternal::FWaterPlane;

        const float Time = (float)Context.GetTime();
        const float Dt   = Context.GetDeltaTime();
        Physics::IPhysicsQueries* Queries = Context.GetPhysics();
        if (Dt <= 0.0f || Queries == nullptr)
        {
            return;
        }

        FWaterPlane Planes[16];
        int NumPlanes = 0;
        Waters.ForEach([&](const SWaterComponent& W, const STransformComponent& T)
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

        Floaters.ParallelForEach([&](ECS::FEntity Entity, const SBuoyancyComponent& B, const SRigidBodyComponent&)
        {
            if (Queries->GetBodyStatus(Entity) != Physics::EPhysicsBodyStatus::Ready)
            {
                return;
            }

            const FVector3 BodyPos = Queries->GetBodyPosition(Entity);

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

            Queries->ApplyBuoyancyImpulse(Entity, SurfacePos, SurfaceNormal, B.Buoyancy, B.LinearDrag, B.AngularDrag, FVector3(0.0f), Dt);
        });
    }
}
