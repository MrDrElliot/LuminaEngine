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

        virtual ~IPhysicsContext() = default;

        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual TUniquePtr<IPhysicsScene> CreatePhysicsScene(CWorld* World) = 0;
    };
    
    enum class EPhysicsAPI : uint8
    {
        Box3D,
    };
    
    void Initialize(EPhysicsAPI API = EPhysicsAPI::Box3D);
    void Shutdown();

    IPhysicsContext* GetPhysicsContext();

    // Box3D derives its tolerances (linear slop, speculative distance, AABB margins) from this, and at 1 they
    // suit meter-scale bodies: a 5 mm slop swallows a 1.75 mm coin. Below 1 tightens them for small objects,
    // so 0.1 gives a 0.5 mm slop. Process-wide, and read when shapes are built, so set it before bodies exist.
    RUNTIME_API void SetLengthUnitsPerMeter(float LengthUnits);
    RUNTIME_API float GetLengthUnitsPerMeter();
}
