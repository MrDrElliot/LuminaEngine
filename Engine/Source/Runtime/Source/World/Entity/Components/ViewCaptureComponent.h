#pragma once

#include "Assets/AssetTypes/Textures/TextureRenderTarget.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderLayers.h"
#include "World/ECS/Entity.h"
#include "ViewCaptureComponent.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EViewCaptureProjection : uint8
    {
        Perspective,
        Orthographic,
    };

    REFLECT()
    enum class EViewCaptureUpdate : uint8
    {
        // A fresh image every frame, for anything that moves, such as a character preview.
        EveryFrame,

        // Once every UpdateInterval seconds, such as a security monitor or a minimap.
        Interval,

        // Only after RequestCapture, such as an item icon that changes when the player equips something.
        OnDemand,

        // Whenever the capture, a shown entity or a property on either changes.
        OnChange,
    };

    REFLECT()
    enum class EViewCaptureBackground : uint8
    {
        // The world's own sky and fog behind whatever the capture shows.
        Scene,

        // BackgroundColor wherever nothing was drawn.
        Color,

        // Alpha zero wherever nothing was drawn, so the image composites over UI.
        Transparent,
    };

    // Renders what the entity sees into a render target, for UI previews, minimaps, monitors, mirrors and portals.
    REFLECT(Component, Category = "Rendering")
    struct RUNTIME_API SViewCaptureComponent
    {
        GENERATED_BODY()

        // The texture the view lands in, whose width and height set the capture's resolution. Materials and UI sample it like any texture.
        PROPERTY(Editable, Category = "Capture")
        TStrongObjectPtr<CTextureRenderTarget> Target;

        PROPERTY(Editable, Category = "Capture")
        bool bEnabled = true;

        PROPERTY(Editable, Category = "Capture")
        EViewCaptureUpdate UpdateMode = EViewCaptureUpdate::EveryFrame;

        // Seconds between captures in Interval mode.
        PROPERTY(Editable, Category = "Capture", ClampMin = 0.0f, Units = "s")
        float UpdateInterval = 0.1f;

        PROPERTY(Editable, Category = "Camera")
        EViewCaptureProjection Projection = EViewCaptureProjection::Perspective;

        // The vertical field of view of a perspective capture.
        PROPERTY(Editable, Category = "Camera", ClampMin = 1.0f, ClampMax = 170.0f, Units = "deg")
        float FieldOfView = 60.0f;

        // The world width an orthographic capture sees.
        PROPERTY(Editable, Category = "Camera", ClampMin = 0.01f, Units = "m")
        float OrthoWidth = 10.0f;

        PROPERTY(Editable, Category = "Camera", ClampMin = 0.001f, Units = "m")
        float NearPlane = 0.05f;

        PROPERTY(Editable, Category = "Camera", ClampMin = 0.01f, Units = "m")
        float FarPlane = 1000.0f;

        // Render layers the capture draws, which also pick the lights it is lit by, so a preview can keep its own lights out of the world.
        PROPERTY(Editable, Category = "Filter")
        SRenderLayerMask CullMask = SRenderLayerMask(RenderLayers::kAllMask);

        // When not empty, only these entities are drawn, with their children when bIncludeChildren is set.
        PROPERTY(Editable, Category = "Filter")
        TVector<ECS::FEntity> ShowOnly;

        // Never drawn, with their children when bIncludeChildren is set.
        PROPERTY(Editable, Category = "Filter")
        TVector<ECS::FEntity> Hidden;

        PROPERTY(Editable, Category = "Filter")
        bool bIncludeChildren = true;

        PROPERTY(Editable, Category = "Background")
        EViewCaptureBackground Background = EViewCaptureBackground::Scene;

        // Linear color behind the view when Background is Color.
        PROPERTY(Editable, Category = "Background")
        FVector4 BackgroundColor = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

        // Scales the sky's ambient light, zero so only the capture's own lights shine, such as a dark inventory backdrop.
        PROPERTY(Editable, Category = "Lighting", ClampMin = 0.0f)
        float AmbientScale = 1.0f;

        // Fixed exposure in stops, since a capture has no eye adaptation of its own.
        PROPERTY(Editable, Category = "Lighting", ClampMin = -16.0f, ClampMax = 16.0f, Units = "EV")
        float ExposureCompensation = 1.0f;

        PROPERTY(Editable, Category = "Quality")
        bool bAntiAliasing = true;

        // The world's color grading, bloom, depth of field and post-process materials, where off grades with the tone mapper alone.
        PROPERTY(Editable, Category = "Quality")
        bool bPostProcess = false;

        // Clouds, height fog and light shafts, the most expensive part of a capture.
        PROPERTY(Editable, Category = "Quality")
        bool bVolumetrics = false;

        PROPERTY(Editable, Category = "Quality")
        bool bReflections = false;

        PROPERTY(Editable, Category = "Quality")
        bool bTranslucency = true;

        PROPERTY(Editable, Category = "Quality")
        bool bDecals = true;

        // Adds an entity to ShowOnly, the way scripts build the list.
        FUNCTION()
        void AddShowOnly(ECS::FEntity Entity) { ShowOnly.AddUnique(Entity); }

        // Adds an entity to Hidden, the way scripts build the list.
        FUNCTION()
        void AddHidden(ECS::FEntity Entity) { Hidden.AddUnique(Entity); }

        FUNCTION()
        void ClearFilters() { ShowOnly.Reset(); Hidden.Reset(); }

        // Asks for one capture on the next frame, whatever UpdateMode says.
        FUNCTION()
        void RequestCapture() { ++CaptureRequests; }

        // Counts RequestCapture calls, so the renderer reads it without writing the component back.
        uint32 CaptureRequests = 0;
    };
}
