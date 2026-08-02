#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "DecalComponent.generated.h"

namespace Lumina
{
    class CMaterialInterface;

    // Projects a Decal-domain material onto opaque surfaces inside its box (DBuffer decals). The box is
    // the entity transform scaled by Size; the material projects along the box's local -Z.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SDecalComponent
    {
        GENERATED_BODY()

        /** Material to project. Must have its domain set to Decal. */
        PROPERTY(Editable, Category = "Decal")
        TObjectPtr<CMaterialInterface> DecalMaterial;

        /** Box dimensions in local units (before the entity transform scale). The decal projects along -Z. */
        PROPERTY(Editable, Category = "Decal", Units = "m")
        FVector3 Size = FVector3(1.0f, 1.0f, 1.0f);

        /** Master opacity of the projection. */
        PROPERTY(Editable, Category = "Decal", ClampMin = 0.0f, ClampMax = 1.0f)
        float Opacity = 1.0f;

        /** Surfaces angled more than this from the projection axis fade out (prevents wall smearing). */
        PROPERTY(Editable, Category = "Decal", ClampMin = 0.0f, ClampMax = 90.0f, Units = "deg")
        float FadeAngle = 75.0f;

        /** Higher values draw later (on top of) lower ones when decals overlap. */
        PROPERTY(Editable, Category = "Decal")
        int32 SortOrder = 0;
    };
}
