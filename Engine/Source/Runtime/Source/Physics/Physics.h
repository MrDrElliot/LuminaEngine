#pragma once
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    class CWorld;
}

namespace Lumina::Physics
{
    static constexpr float GEarthGravity = -9.81f;
    
    class IPhysicsScene;

    class IPhysicsContext
    {
    public:

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual TUniquePtr<IPhysicsScene> CreatePhysicsScene(CWorld* World) = 0;
    };
    
    enum class EPhysicsAPI : uint8
    {
        Jolt,
    };
    
    void Initialize(EPhysicsAPI API = EPhysicsAPI::Jolt);
    void Shutdown();

    IPhysicsContext* GetPhysicsContext();
}
