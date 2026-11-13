#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Vector.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/RenderLayers.h"
#include "Renderer/ViewVolume.h"
#include "PostProcessSettings.h"
#include "CameraComponent.generated.h"


namespace Lumina
{
    struct FPropertyChangedEvent;

    class CMaterialInterface;

    // Interpolation curve for blending the active camera (CCameraSystem::SetActiveCamera);
    // EvaluateCameraBlend maps normalized [0..1] time onto [0..1].
    REFLECT()
    enum class ECameraBlendFunction : uint8
    {
        Linear      = 0, // Constant velocity.
        EaseIn      = 1, // Quadratic; starts slow.
        EaseOut     = 2, // Quadratic; ends slow.
        EaseInOut   = 3, // Smoothstep; slow at both ends. The cinematic default.
    };

    /** Maps normalized blend time [0..1] through Function to an eased [0..1] alpha. */
    RUNTIME_API float EvaluateCameraBlend(ECameraBlendFunction Function, float Alpha);

    // Yaw about world up, then pitch about the camera's right, stopping a degree short of vertical.
    RUNTIME_API FQuat AddMouseLook(const FQuat& Rotation, FVector2 YawPitchDegrees);
    RUNTIME_API FViewVolume AddMouseLook(const FViewVolume& View, FVector2 YawPitchDegrees);

    // Result of CCameraSubsystem::WorldToScreen. Blittable so C# receives it by value.
    REFLECT()
    struct RUNTIME_API SScreenProjection
    {
        GENERATED_BODY()

        /** Pixel coordinates, origin top-left, +Y down. Meaningless unless bOnScreen. */
        PROPERTY(Hidden)
        FVector2 Position;

        /** View-space forward distance in world units, positive in front. Valid even when off screen. */
        PROPERTY(Hidden)
        float Depth = 0.0f;

        /** False when the point is at or behind the near plane, where a projected point mirrors. */
        PROPERTY(Hidden)
        bool bOnScreen = false;
    };

    // Result of CCameraSubsystem::ScreenToWorldRay. Blittable so C# receives it by value.
    REFLECT()
    struct RUNTIME_API SWorldRay
    {
        GENERATED_BODY()

        /** On the near plane, not at the camera position. */
        PROPERTY(Hidden)
        FVector3 Origin;

        /** Normalized. */
        PROPERTY(Hidden)
        FVector3 Direction;

        /** False when the world has no rendered view to deproject through. */
        PROPERTY(Hidden)
        bool bValid = false;
    };

    REFLECT(Component, Category = "Camera")
    struct RUNTIME_API SCameraComponent
    {
        GENERATED_BODY()

        SCameraComponent(float fov = 90.0f, float aspect = 16.0f / 9.0f)
            :ViewVolume(fov, aspect)
        {}

        void SetView(const FVector3& Position, const FVector3& ViewDirection, const FVector3& UpDirection)
        {
            ViewVolume.SetView(Position, ViewDirection, UpDirection);
        }

        // Bake the resolved (possibly blended) view into the view volume without writing authored FOV, so
        // direct matrix consumers (editor gizmo, CPU picking) match the rendered view. Called by CCameraSystem.
        void SetResolvedView(const FVector3& Position, const FVector3& ViewDirection, const FVector3& UpDirection, float InFOV)
        {
            ViewVolume.SetFOV(InFOV);
            ViewVolume.SetView(Position, ViewDirection, UpDirection);
        }

        FUNCTION()
        void SetFOV(float NewFOV)
        {
            FOV = NewFOV;
            ViewVolume.SetFOV(NewFOV);
        }
        
        void SetAspectRatio(float NewAspect)
        {
            ViewVolume.SetAspectRatio(NewAspect);
        }

        /** Width is the world-space span across the viewport. */
        void SetOrthographic(float Width)
        {
            ViewVolume.SetOrthographic(Width, ViewVolume.GetAspectRatio());
        }

        void SetPerspectiveProjection()
        {
            ViewVolume.SetPerspective(FOV, ViewVolume.GetAspectRatio());
        }

        bool IsOrthographic() const { return ViewVolume.IsOrthographic(); }
        float GetOrthoWidth() const { return ViewVolume.GetOrthoWidth(); }

        void SetPosition(const FVector3& NewPosition)
        {
            ViewVolume.SetViewPosition(NewPosition);
        }

        FUNCTION()
        float GetFOV() const { return ViewVolume.GetFOV(); }
        float GetAspectRatio() const { return ViewVolume.GetAspectRatio(); }
        const FMatrix4& GetViewMatrix() const { return ViewVolume.GetViewMatrix(); }
        const FMatrix4& GetProjectionMatrix() const { return ViewVolume.GetProjectionMatrix(); }
        const FMatrix4& GetViewProjectionMatrix() const { return ViewVolume.GetViewProjectionMatrix(); }
        const FViewVolume& GetViewVolume() const { return ViewVolume; }
        
        FUNCTION()
        FVector3 GetPosition() const { return ViewVolume.GetViewPosition(); }
        
        FUNCTION()
        FVector3 GetForwardVector() const { return ViewVolume.GetForwardVector(); }
        
        FUNCTION()
        FVector3 GetRightVector() const { return ViewVolume.GetRightVector(); }

        /** Vertical field of view in degrees. */
        PROPERTY(Editable, Category = "Camera", Units = "deg")
        float FOV = 90.0f;

        /** Nearest distance drawn. Raising it buys depth precision everywhere further out. */
        PROPERTY(Editable, Category = "Camera", ClampMin = 0.001f)
        float NearPlane = FViewVolume::DefaultNearPlane;

        /** Furthest distance drawn. */
        PROPERTY(Editable, Category = "Camera", ClampMin = 1.0f)
        float FarPlane = FViewVolume::DefaultFarPlane;
        
        // Render layers this camera draws.
        PROPERTY(Editable, Category = "Camera|Render Layers")
        SRenderLayerMask CullMask = SRenderLayerMask(RenderLayers::kAllMask);

        // Draws the Foreground layer over the world at this vertical field of view, for first-person hands and weapons. Zero draws it in place.
        PROPERTY(Editable, Category = "Camera|Render Layers", Units = "deg", ClampMin = 0.0f, ClampMax = 170.0f)
        float ForegroundFOV = 0.0f;

        // How far toward the camera Foreground geometry is pulled so walls never cut into it, small enough to clear the world yet leave it past NearPlane.
        PROPERTY(Editable, Category = "Camera|Render Layers", ClampMin = 0.01f, ClampMax = 1.0f)
        float ForegroundDepthScale = 0.1f;

        FViewLayerSettings GetViewLayers() const
        {
            return FViewLayerSettings{ CullMask.Bits, ForegroundFOV, ForegroundDepthScale };
        }

        void ApplyCameraProperties()
        {
            ViewVolume.SetFOV(FOV).SetNear(NearPlane).SetFar(FarPlane);
        }

        #if USING(WITH_EDITOR)
        void PostEditChange(const FPropertyChangedEvent& Event) { ApplyCameraProperties(); }
        #endif

        /** When true, this camera activates automatically when the entity is spawned. */
        PROPERTY(Editable, Category = "Camera")
        bool bAutoActivate = false;

        // Turns the rendered view by mouse motion that arrives after the update, so set it to the look code's degrees per count. Zero is off.
        PROPERTY(Editable, Category = "Camera|Input", ClampMin = 0.0f)
        float MouseLookDegreesPerCount = 0.0f;

        // Per-camera color grading + tone mapping; the render scene reads it from the active camera and
        // applies it in the composite pass. Defaults to an identity grade with AGX.
        PROPERTY(Editable, Category = "Camera", DefaultCollapsed)
        SPostProcessSettings PostProcess;

        // Post-process materials run in order after tone mapping; each is a fullscreen pass whose Emissive
        // replaces scene color (later entries read earlier via SceneColor). Must be MaterialType = PostProcess.
        PROPERTY(Editable, Category = "Camera|Post Process")
        TVector<TStrongObjectPtr<CMaterialInterface>> PostProcessMaterials;

    private:

        FViewVolume ViewVolume;
    };
}
