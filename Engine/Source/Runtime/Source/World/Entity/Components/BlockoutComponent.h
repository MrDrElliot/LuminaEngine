#pragma once

#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "BlockoutComponent.generated.h"

namespace Lumina
{
    /** Primitive a blockout shape is generated from. */
    REFLECT()
    enum class EBlockoutShape : uint8
    {
        Box      = 0,
        Plane    = 1,
        Ramp     = 2,
        Stairs   = 3,
        Cylinder = 4,
        Cone     = 5,
        Sphere   = 6,
        Capsule  = 7,
        Torus    = 8,
        Arch     = 9,
        Pipe     = 10,
    };

    /** Where the entity origin sits inside the generated shape. */
    REFLECT()
    enum class EBlockoutPivot : uint8
    {
        /** Bottom face, so the shape rests on whatever it was placed against. */
        Base   = 0,

        /** Middle of the bounding box. */
        Center = 1,
    };

    /** Authored parameters SBlockoutSystem meshes onto the SDynamicMeshComponent beside it. */
    REFLECT(Component, Category = "Modeling")
    struct RUNTIME_API SBlockoutComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Blockout")
        EBlockoutShape Shape = EBlockoutShape::Box;

        /** Bounding size in meters before entity scale. Radial shapes read X and Z as diameters. */
        PROPERTY(Editable, Category = "Blockout", Units = "m", ClampMin = 0.01f)
        FVector3 Size = FVector3(2.0f);

        PROPERTY(Editable, Category = "Blockout")
        EBlockoutPivot Pivot = EBlockoutPivot::Base;

        /** Material for the whole shape. Null draws with the engine default. */
        PROPERTY(Editable, Category = "Blockout")
        TStrongObjectPtr<CMaterialInterface> Material;

        /** Sides around the axis for Cylinder, Cone, Sphere, Capsule, Torus, Arch and Pipe. */
        PROPERTY(Editable, Category = "Blockout|Tessellation", ClampMin = 3, ClampMax = 128)
        int32 RadialSegments = 16;

        /** Rings from pole to pole for Sphere, Capsule and Torus. Flat-sided shapes ignore it. */
        PROPERTY(Editable, Category = "Blockout|Tessellation", ClampMin = 2, ClampMax = 128)
        int32 RingSegments = 8;

        /** Step count for Stairs, and segments along the sweep for Arch. */
        PROPERTY(Editable, Category = "Blockout|Tessellation", ClampMin = 1, ClampMax = 128)
        int32 Steps = 8;

        /** Wall thickness for Pipe, band thickness for Arch, tube radius for Torus. */
        PROPERTY(Editable, Category = "Blockout", Units = "m", ClampMin = 0.001f)
        float Thickness = 0.25f;

        /** Meters of surface per UV tile, so differently sized shapes keep one texel density. */
        PROPERTY(Editable, Category = "Blockout", Units = "m", ClampMin = 0.001f)
        float UVScale = 1.0f;

        /** Parameter hash the current mesh was built from. Transient, never serialized. */
        uint32 BuiltHash = 0;

        /** False until the first build, and after every load because it is never serialized. */
        bool bBuilt = false;

        /** Material the sibling mesh's slot 0 override last pointed at. Transient, never serialized. */
        const void* BuiltMaterial = nullptr;
    };
}
