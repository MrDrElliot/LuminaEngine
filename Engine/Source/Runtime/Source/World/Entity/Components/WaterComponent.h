#pragma once

#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "WaterComponent.generated.h"

namespace Lumina
{
    class CTexture;

    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SWaterComponent
    {
        GENERATED_BODY()

        /** Plane size in the local XZ plane (before the entity transform scale). */
        PROPERTY(Editable, Category = "Water|Surface", Units = "m")
        FVector2 Extent = FVector2(50.0f, 50.0f);

        /** Tessellation: verts per side of the procedural grid. Higher = smoother waves, costlier. */
        PROPERTY(Editable, Category = "Water|Surface", ClampMin = 2, ClampMax = 512)
        int32 GridResolution = 128;

        /** Master surface opacity (soft-blended at the shoreline regardless). */
        PROPERTY(Editable, Category = "Water|Surface", ClampMin = 0.0f, ClampMax = 1.0f)
        float Opacity = 1.0f;
        

        /** Wind direction in the XZ plane; waves travel along it. */
        PROPERTY(Editable, Category = "Water|Waves")
        FVector2 WindDirection = FVector2(1.0f, 0.0f);

        /** Wind strength: scales wave speed. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f)
        float WindSpeed = 4.0f;

        /** Peak vertical displacement of the dominant wave. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, Delta = 0.01f, Units = "m")
        float WaveAmplitude = 0.4f;

        /** Gerstner steepness (0 = rolling swell, 1 = sharp peaks). High values can pinch the surface. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, ClampMax = 1.0f)
        float Choppiness = 0.6f;

        /** Wavelength multiplier for the synthesized wave set. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.05f)
        float WaveScale = 0.1f;

        /** Number of summed Gerstner waves (fanned around the wind direction). */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 1, ClampMax = 8)
        int32 WaveCount = 4;

        /** Optional tangent-space detail normal (BC5) for high-frequency ripples. */
        PROPERTY(Editable, Category = "Water|Waves")
        TObjectPtr<CTexture> DetailNormalMap;

        /** Strength of the detail normal perturbation. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, ClampMax = 1.0f)
        float DetailStrength = 0.3f;

        /** Detail normal tiling across the surface. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.01f)
        float DetailTiling = 8.0f;

        /** Detail normal scroll speed (world units / s along the wind). */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f)
        float DetailScrollSpeed = 0.05f;
        

        /** Tint of shallow water (where the bed is close to the surface). */
        PROPERTY(Editable, Color, Category = "Water|Color")
        FVector3 ShallowColor = FVector3(0.10f, 0.55f, 0.65f);

        /** Tint approached as the water column deepens. */
        PROPERTY(Editable, Color, Category = "Water|Color")
        FVector3 DeepColor = FVector3(0.02f, 0.12f, 0.28f);

        /** Water-column depth over which the color fades shallow -> deep. */
        PROPERTY(Editable, Category = "Water|Color", ClampMin = 0.01f, Units = "m")
        float DepthFadeDistance = 6.0f;

        /** Beer-Lambert absorption strength applied to the refracted scene. */
        PROPERTY(Editable, Category = "Water|Color", ClampMin = 0.0f)
        float AbsorptionScale = 1.0f;
        
        
        /** Screen-space refraction offset scale (driven by the surface normal). */
        PROPERTY(Editable, Category = "Water|Refraction", ClampMin = 0.0f, ClampMax = 0.5f, Delta = 0.001f)
        float RefractionStrength = 0.04f;

        /** Overall reflection intensity (SSR + sky fallback). */
        PROPERTY(Editable, Category = "Water|Reflection", ClampMin = 0.0f, ClampMax = 1.0f)
        float ReflectionStrength = 1.0f;

        /** Surface roughness: blurs the reflection (sky prefilter mip) and softens the sun glint. */
        PROPERTY(Editable, Category = "Water|Reflection", ClampMin = 0.0f, ClampMax = 1.0f)
        float Roughness = 0.04f;

        /** Schlick Fresnel exponent (higher = reflection only at grazing angles). */
        PROPERTY(Editable, Category = "Water|Reflection", ClampMin = 1.0f, ClampMax = 8.0f)
        float FresnelPower = 5.0f;

        /** Max world-space distance the SSR ray marches before falling back to the sky. */
        PROPERTY(Editable, Category = "Water|Reflection", ClampMin = 1.0f, Units = "m")
        float SSRMaxDistance = 50.0f;

        /** SSR ray-march step count; higher resolves more but costs more. */
        PROPERTY(Editable, Category = "Water|Reflection", ClampMin = 8, ClampMax = 128)
        int32 SSRStepCount = 32;


        /** Strength of the sun glint on the wave normals (cascade-shadowed). */
        PROPERTY(Editable, Category = "Water|Specular", ClampMin = 0.0f)
        float SpecularIntensity = 1.0f;


        /** Foam color (shoreline + wave crests). */
        PROPERTY(Editable, Color, Category = "Water|Foam")
        FVector3 FoamColor = FVector3(1.0f, 1.0f, 1.0f);

        /** Overall foam strength multiplier. */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.0f)
        float FoamIntensity = 0.0f;

        /** Water-column depth over which shoreline foam appears (foam where water meets geometry). */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.0f, Units = "m")
        float ShorelineFoamWidth = 1.5f;

        /** Wave-crest height fraction that foams (1 = foam on all crests, 0 = only the tallest). */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.0f, ClampMax = 1.0f)
        float CrestFoamAmount = 0.3f;

        /** Optional foam texture, scrolled with the wind. */
        PROPERTY(Editable, Category = "Water|Foam")
        TObjectPtr<CTexture> FoamTexture;

        /** Foam texture tiling across the surface. */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.01f)
        float FoamTiling = 4.0f;
        
        
        /** Fog color the scene fades toward as the underwater view distance grows. */
        PROPERTY(Editable, Color, Category = "Water|Underwater")
        FVector3 UnderwaterFogColor = FVector3(0.02f, 0.12f, 0.20f);

        /** Underwater fog density (absorption per meter of submerged view ray). */
        PROPERTY(Editable, Category = "Water|Underwater", ClampMin = 0.0f, Delta = 0.001f)
        float UnderwaterFogDensity = 0.15f;

        /** Screen distortion amount while submerged. */
        PROPERTY(Editable, Category = "Water|Underwater", ClampMin = 0.0f, ClampMax = 0.2f, Delta = 0.001f)
        float UnderwaterDistortion = 0.015f;

        /** Overall color cast applied to the submerged view. */
        PROPERTY(Editable, Color, Category = "Water|Underwater")
        FVector3 UnderwaterTint = FVector3(0.6f, 0.85f, 1.0f);


        /** When enabled, entities with a Buoyancy component float on this water surface (matching the waves). */
        PROPERTY(Editable, Category = "Water|Buoyancy")
        bool bBuoyancy = false;
    };
}
