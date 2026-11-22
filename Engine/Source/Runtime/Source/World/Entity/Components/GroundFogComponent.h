#pragma once

#include "Core/Math/Math.h"
#include "GroundFogComponent.generated.h"

namespace Lumina
{
    // A shallow layer of radiation fog lying on the ground, which characters and moving bodies push aside as they wade through it.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SGroundFogComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Ground Fog")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 0.5f, Units = "m", ToolTip = "Half size of the fogged area along the entity's X and Z. The layer lies on the entity's height.")
        FVector2 Extent = FVector2(40.0f, 40.0f);

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 0.05f, ClampMax = 20.0f, Delta = 0.05f, Units = "m", ToolTip = "Depth of the layer above the ground.")
        float Thickness = 0.9f;

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 0.1f, Delta = 0.1f, Units = "m", ToolTip = "Distance inside the layer at which it hides 98% of what is behind it. Lower is thicker.")
        float VisibilityDistance = 6.0f;

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 0.01f, ClampMax = 1.0f, Delta = 0.01f, ToolTip = "Fraction of the thickness over which the layer thins out at its top.")
        float TopSoftness = 0.6f;

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 0.0f, Units = "m", ToolTip = "Width over which the layer fades out at the edges of its area.")
        float EdgeFade = 6.0f;

        PROPERTY(Editable, Category = "Ground Fog", ClampMin = 1.0f, Units = "m", ToolTip = "Distance from the camera at which the layer has faded out.")
        float ViewDistance = 250.0f;

        PROPERTY(Editable, Color, Category = "Lighting")
        FVector3 Albedo = FVector3(0.82f, 0.86f, 0.9f);

        PROPERTY(Editable, Category = "Lighting", ClampMin = 0.0f, ToolTip = "Scales all light the layer scatters toward the camera.")
        float ScatteringIntensity = 1.0f;

        PROPERTY(Editable, Category = "Lighting", ClampMin = -0.9f, ClampMax = 0.9f, Delta = 0.01f, ToolTip = "Positive glows toward the sun and lights, negative scatters back toward them.")
        float Anisotropy = 0.35f;

        PROPERTY(Editable, Category = "Wisps", ClampMin = 0.0f, ClampMax = 1.0f, Delta = 0.01f, ToolTip = "How much the layer breaks into wisps and banks rather than lying flat.")
        float WispStrength = 0.65f;

        PROPERTY(Editable, Category = "Wisps", ClampMin = 0.1f, Units = "m", ToolTip = "Size of the largest wisps.")
        float WispScale = 5.0f;

        PROPERTY(Editable, Category = "Wisps", Units = "m/s", ToolTip = "Drift of the layer along world X and Z.")
        FVector2 Wind = FVector2(0.25f, 0.1f);

        PROPERTY(Editable, Category = "Simulation", ToolTip = "Lets skinned meshes and moving bodies part the fog. The layer nearest the camera sets the shared simulation's response.")
        bool bInteractive = true;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f, ClampMax = 4.0f, Delta = 0.01f, EditCondition = "bInteractive", ToolTip = "How hard a body moving through the layer pushes the fog away.")
        float DisplacementStrength = 1.0f;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.1f, ClampMax = 60.0f, Delta = 0.1f, Units = "s", EditCondition = "bInteractive", ToolTip = "Time for a parted path to fill back in.")
        float RefillTime = 3.5f;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.0f, ClampMax = 2.0f, Delta = 0.01f, EditCondition = "bInteractive", ToolTip = "Curl kept in the wake, so it rolls into eddies rather than closing smoothly.")
        float Swirl = 0.5f;

        PROPERTY(Editable, Category = "Simulation", ClampMin = 0.05f, ClampMax = 20.0f, Delta = 0.05f, Units = "s", EditCondition = "bInteractive", ToolTip = "Time the flow a body stirred up takes to die down.")
        float FlowLifetime = 1.6f;
    };
}
