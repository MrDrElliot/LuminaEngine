#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "DecalLibrary.generated.h"

namespace Lumina
{
    class CWorld;
    class CMaterialInterface;

    // Script access to projected decals, for marks left by gameplay such as scorches and bullet holes.
    REFLECT()
    class RUNTIME_API CDecalLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Size.z is the depth the box reaches either side of the surface, and a positive Lifetime fades then destroys it.
        FUNCTION()
        static ECS::FEntity SpawnDecal(CWorld* World, CMaterialInterface* Material, FVector3 Location, FVector3 Normal, FVector3 Size,
                                       float RotationDegrees = 0.0f, float Lifetime = 0.0f, float FadeOutDuration = 1.0f,
                                       int32 AtlasCell = 0, int32 AtlasColumns = 1, int32 AtlasRows = 1);
    };
}
