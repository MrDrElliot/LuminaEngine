#include "RuntimePCH.h"
#include "Physics.h"

#include "API/Box3D/Box3DPhysics.h"
#include <box3d/constants.h>

namespace Lumina::Physics
{
    static inline TUniquePtr<IPhysicsContext> GPhysicsContext;

    
    void Initialize(EPhysicsAPI API)
    {
        if (API == EPhysicsAPI::Box3D)
        {
            GPhysicsContext = MakeUnique<FBox3DPhysicsContext>();
        }

        GPhysicsContext->Initialize();
    }

    void Shutdown()
    {
        GPhysicsContext->Shutdown();
        GPhysicsContext.reset();
    }

    IPhysicsContext* GetPhysicsContext()
    {
        return GPhysicsContext.get();
    }

    void SetLengthUnitsPerMeter(float LengthUnits)
    {
        b3SetLengthUnitsPerMeter(LengthUnits);
    }

    float GetLengthUnitsPerMeter()
    {
        return b3GetLengthUnitsPerMeter();
    }
}
