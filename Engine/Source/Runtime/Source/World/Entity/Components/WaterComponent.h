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
        FVector2 Extent = FVector2(512.0f, 512.0f);

        /** Tessellation: verts per side of the procedural grid. Keep cell size well under WaveLength. */
        PROPERTY(Editable, Category = "Water|Surface", ClampMin = 2, ClampMax = 512)
        int32 GridResolution = 384;

        // Width the outermost ring of cells stretches out to, so a sea meets the horizon; 0 keeps the body its own size.
        PROPERTY(Editable, Category = "Water|Surface", ClampMin = 0.0f, Units = "m")
        float HorizonExtent = 0.0f;

        /** Master surface opacity (soft-blended at the shoreline regardless). */
        PROPERTY(Editable, Category = "Water|Surface", ClampMin = 0.0f, ClampMax = 1.0f)
        float Opacity = 1.0f;
        

        /** Wind direction in the XZ plane; waves travel along it. */
        PROPERTY(Editable, Category = "Water|Waves")
        FVector2 WindDirection = FVector2(1.0f, 0.0f);

        /** Wind strength: scales wave speed. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f)
        float WindSpeed = 1.0f;

        /** Crest-to-trough half height of the dominant wave. Clamped against WaveLength at the Stokes breaking limit. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, Delta = 0.01f, Units = "m")
        float WaveAmplitude = 0.012f;

        /** Gerstner steepness (0 = rolling swell, 1 = sharp peaks). Also drives how readily crests whitecap. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, ClampMax = 1.0f)
        float Choppiness = 0.1f;

        // Wavelength of the dominant wave, each successive wave 0.86x shorter.
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.5f, Units = "m")
        float WaveLength = 3.5f;

        // Swell octaves the surface geometry carries, doubled into the series; finer ripples are shaded on top regardless.
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 1, ClampMax = 8)
        int32 WaveCount = 6;

        /** Optional tangent-space detail normal (BC5) for high-frequency ripples. */
        PROPERTY(Editable, Category = "Water|Waves")
        TStrongObjectPtr<CTexture> DetailNormalMap;

        /** Strength of the detail normal perturbation. */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f, ClampMax = 1.0f)
        float DetailStrength = 0.3f;

        // World size of one detail normal tile, faded out with distance so it never reads as a repeating pattern.
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.01f, Units = "m")
        float DetailTileSize = 6.0f;

        /** Detail normal scroll speed (world units / s along the wind). */
        PROPERTY(Editable, Category = "Water|Waves", ClampMin = 0.0f)
        float DetailScrollSpeed = 0.05f;
        

        /** Tint of shallow water (where the bed is close to the surface). */
        PROPERTY(Editable, Color, Category = "Water|Color")
        FVector3 ShallowColor = FVector3(0.05f, 0.09f, 0.07f);

        /** Tint approached as the water column deepens. */
        PROPERTY(Editable, Color, Category = "Water|Color")
        FVector3 DeepColor = FVector3(0.008f, 0.025f, 0.03f);

        /** Water-column depth over which the color fades shallow -> deep. */
        PROPERTY(Editable, Category = "Water|Color", ClampMin = 0.01f, Units = "m")
        float DepthFadeDistance = 3.0f;

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
        float Roughness = 0.02f;

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
        float FoamIntensity = 0.4f;

        /** Water-column depth over which shoreline foam appears (foam where water meets geometry). */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.0f, Units = "m")
        float ShorelineFoamWidth = 0.35f;

        /** How readily a compressing crest whitecaps (1 = any compression foams, 0 = only fully folded). */
        PROPERTY(Editable, Category = "Water|Foam", ClampMin = 0.0f, ClampMax = 1.0f)
        float CrestFoamAmount = 0.0f;

        /** Optional foam texture, scrolled with the wind. */
        PROPERTY(Editable, Category = "Water|Foam")
        TStrongObjectPtr<CTexture> FoamTexture;

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
