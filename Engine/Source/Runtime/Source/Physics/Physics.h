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
    class FCollisionShape;
    struct FHeightFieldDesc;

    class IPhysicsContext
    {
    public:

        virtual ~IPhysicsContext() = default;

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual TUniquePtr<IPhysicsScene> CreatePhysicsScene(CWorld* World) = 0;

        // Safe from any thread and outlives every scene, so a streamed patch cooks on a worker. Null when the desc is empty.
        virtual TSharedPtr<const FCollisionShape> CookHeightField(const FHeightFieldDesc& Desc) const = 0;
    };
    
    enum class EPhysicsAPI : uint8
    {
        Box3D,
    };
    
    RUNTIME_API void Initialize(EPhysicsAPI API = EPhysicsAPI::Box3D);
    RUNTIME_API void Shutdown();

    RUNTIME_API IPhysicsContext* GetPhysicsContext();

    // Process-wide and read as shapes are built, so it has to be set before any body exists.
    RUNTIME_API void SetLengthUnitsPerMeter(float LengthUnits);
    RUNTIME_API float GetLengthUnitsPerMeter();
}
