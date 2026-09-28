#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "TerrainLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // Ground queries against every terrain in a world, so script can place things without its own heightfield.
    REFLECT()
    class RUNTIME_API CTerrainLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        static bool IsOnTerrain(CWorld* World, float X, float Z);

        // The terrain surface height under (X, Z), or Fallback when no terrain covers the point.
        FUNCTION()
        static float GetTerrainHeight(CWorld* World, float X, float Z, float Fallback = 0.0f);

        // The terrain surface normal under (X, Z), or world up when no terrain covers the point.
        FUNCTION()
        static FVector3 GetTerrainNormal(CWorld* World, float X, float Z);
    };
}
