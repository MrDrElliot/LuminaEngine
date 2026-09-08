#pragma once

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "GrassType.generated.h"

namespace Lumina
{
    /**
     * One procedurally scattered grass species, referenced by a terrain material's GrassOutput node.
     * Unlike SFoliageType, nothing here is painted: the terrain layer the node binds it to decides where
     * it grows, and the scatter is regenerated per chunk rather than serialized.
     */
    REFLECT()
    class RUNTIME_API CGrassType : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        /** Mesh instanced for every blade/clump. */
        PROPERTY(Editable, Category = "Grass")
        TObjectPtr<CStaticMesh> Mesh;

        /** Instances per square metre where the bound layer's weight is 1. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0001f, NoDrag, Delta = 0.01f)
        float Density = 2.0f;

        /** Layer weight below which nothing is placed, so a faint blend does not sprout single blades. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0f, ClampMax = 1.0f)
        float MinWeight = 0.25f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMin = 0.85f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMax = 1.25f;

        /** Offset along the surface normal, to sink instances into the ground. */
        PROPERTY(Editable, Category = "Scatter")
        float ZOffset = 0.0f;

        /** 0 = keep upright, 1 = fully align the up-axis to the terrain normal. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0f, ClampMax = 1.0f)
        float AlignToNormal = 0.3f;

        PROPERTY(Editable, Category = "Scatter")
        bool bRandomYaw = true;

        /** Terrain slope above which nothing grows, in degrees. 90 places on any slope. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0f, ClampMax = 90.0f)
        float MaxSlopeDegrees = 45.0f;

        /**
         * Chunks are scattered when they come within this distance of the camera and released when they
         * leave, so this bounds both the draw distance and the resident instance count.
         */
        PROPERTY(Editable, Category = "Rendering", ClampMin = 1.0f, NoDrag, Delta = 256.0f)
        float CullDistance = 8192.0f;

        PROPERTY(Editable, Category = "Rendering")
        bool bCastShadow = false;

        PROPERTY(Editable, Category = "Rendering")
        bool bReceiveShadow = true;

        /** Deterministic per-type seed, so two types on one layer do not land on identical points. */
        PROPERTY(Editable, Category = "Scatter")
        uint32 Seed = 0;
    };
}
