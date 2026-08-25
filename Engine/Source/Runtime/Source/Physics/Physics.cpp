#include "RuntimePCH.h"
#include "Physics.h"

#include "API/Box3D/Box3DPhysics.h"

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
}
