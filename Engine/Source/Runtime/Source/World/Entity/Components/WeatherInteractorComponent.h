#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "WeatherInteractorComponent.generated.h"

namespace Lumina
{
    struct FSkeletonResource;

    // A sphere that touches water, puddles and snow, riding a bone or socket or the entity itself.
    REFLECT()
    struct RUNTIME_API FWeatherContactPoint
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Contact", ToolTip = "Bone or socket the sphere follows. None keeps it on the entity.")
        FName Bone;

        PROPERTY(Editable, Category = "Contact", ToolTip = "Offset from the bone or entity, in its own space.")
        FVector3 Offset = FVector3(0.0f);

        PROPERTY(Editable, Category = "Contact", ClampMin = 0.01f, ClampMax = 10.0f, Units = "m")
        float Radius = 0.12f;
    };

    // One contact sphere resolved against the current mesh, kept between frames to measure how it moved.
    struct FWeatherContactRuntime
    {
        int32    BoneIndex    = Constants::kIndexNone;
        FVector3 Offset       = FVector3(0.0f);
        float    Radius       = 0.0f;
        FVector3 PrevPosition = FVector3(0.0f);
        float    PrevSurfaceY = 0.0f;
        float    SprayDistance = 0.0f;
        bool     bHasPrev     = false;
    };

    // Lets an entity disturb the weather it walks through, and the weather soak it in return.
    REFLECT(Component, Category = "Environment")
    struct RUNTIME_API SWeatherInteractorComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Interaction")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Interaction", ToolTip = "Spheres that touch water, puddles and snow. Empty finds the feet, legs and hips of a skeleton, or wraps a static mesh's bounds.")
        TVector<FWeatherContactPoint> Contacts;

        PROPERTY(Editable, Category = "Interaction", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Scales the ripples and wakes this raises in water and puddles.")
        float RippleStrength = 1.0f;

        PROPERTY(Editable, Category = "Interaction", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Scales the splashes this throws up landing in water or stepping in puddles.")
        float SplashScale = 1.0f;

        PROPERTY(Editable, Category = "Interaction", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Scales the foam churned up behind this in open water.")
        float FoamStrength = 1.0f;

        PROPERTY(Editable, Category = "Interaction", ClampMin = 0.0f, ClampMax = 2.0f, ToolTip = "How deep a print this presses into snow and mud.")
        float TrailStrength = 1.0f;

        PROPERTY(Editable, Category = "Wetness", ToolTip = "Soaks in rain and keeps a water line after wading, drying off over time.")
        bool bGetsWet = true;

        PROPERTY(Editable, Category = "Wetness", ClampMin = 0.0f, ClampMax = 2.0f, EditCondition = "bGetsWet", ToolTip = "Wetness gained per second standing in full rain.")
        float SoakRate = 0.12f;

        PROPERTY(Editable, Category = "Wetness", ClampMin = 0.0f, ClampMax = 2.0f, EditCondition = "bGetsWet", ToolTip = "Wetness lost per second out of the rain.")
        float DryingRate = 0.015f;

        // Transient state the interaction system keeps, never serialized.
        TVector<FWeatherContactRuntime> Runtime;
        const FSkeletonResource*        ResolvedSkeleton = nullptr;
        uint32                          ResolvedContactCount = 0;
        bool                            bResolved        = false;
        float                           Soak             = 0.0f;
        float                           WaterlineHeight  = 0.0f;
        float                           WaterlineWetness = 0.0f;
        float                           RainExposure     = 1.0f;
        uint32                          ExposureCountdown = 0;
    };
}
