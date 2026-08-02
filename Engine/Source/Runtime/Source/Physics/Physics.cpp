#include "RuntimePCH.h"
#include "Physics.h"

#include "API/Jolt/JoltPhysics.h"

namespace Lumina::Physics
{
    static inline TUniquePtr<IPhysicsContext> GPhysicsContext;

    
    void Initialize(EPhysicsAPI API)
    {
        if (API == EPhysicsAPI::Jolt)
        {
            GPhysicsContext = MakeUnique<FJoltPhysicsContext>();
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
