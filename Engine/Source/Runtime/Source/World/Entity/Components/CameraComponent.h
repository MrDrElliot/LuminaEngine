#pragma once
#include "Containers/Array.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Renderer/ViewVolume.h"
#include "PostProcessSettings.h"
#include "CameraComponent.generated.h"


namespace Lumina
{
    class CMaterialInterface;

    // Interpolation curve for blending the active camera (SCameraSystem::SetActiveCamera);
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
        // direct matrix consumers (editor gizmo, CPU picking) match the rendered view. Called by SCameraSystem.
        void SetResolvedView(const FVector3& Position, const FVector3& ViewDirection, const FVector3& UpDirection, float InFOV)
        {
            ViewVolume.SetFOV(InFOV);
            ViewVolume.SetView(Position, ViewDirection, UpDirection);
        }

        FUNCTION(Script)
        void SetFOV(float NewFOV)
        {
            FOV = NewFOV;
            ViewVolume.SetFOV(NewFOV);
        }
        
        void SetAspectRatio(float NewAspect)
        {
            ViewVolume.SetPerspective(ViewVolume.GetFOV(), NewAspect);
        }

        void SetPosition(const FVector3& NewPosition)
        {
            ViewVolume.SetViewPosition(NewPosition);
        }

        FUNCTION(Script)
        float GetFOV() const { return ViewVolume.GetFOV(); }
        float GetAspectRatio() const { return ViewVolume.GetAspectRatio(); }
        const FMatrix4& GetViewMatrix() const { return ViewVolume.GetViewMatrix(); }
        const FMatrix4& GetProjectionMatrix() const { return ViewVolume.GetProjectionMatrix(); }
        const FMatrix4& GetViewProjectionMatrix() const { return ViewVolume.GetViewProjectionMatrix(); }
        const FViewVolume& GetViewVolume() const { return ViewVolume; }
        
        FUNCTION(Script)
        FVector3 GetPosition() const { return ViewVolume.GetViewPosition(); }
        
        FUNCTION(Script)
        FVector3 GetForwardVector() const { return ViewVolume.GetForwardVector(); }
        
        FUNCTION(Script)
        FVector3 GetRightVector() const { return ViewVolume.GetRightVector(); }

        /** Vertical field of view in degrees. */
        PROPERTY(Editable, Category = "Camera", Units = "deg")
        float FOV = 90.0f;

        /** When true, this camera activates automatically when the entity is spawned. */
        PROPERTY(Editable, Category = "Camera")
        bool bAutoActivate = false;

        // Per-camera color grading + tone mapping; the render scene reads it from the active camera and
        // applies it in the composite pass. Defaults to an identity grade with AGX.
        PROPERTY(Editable, Category = "Camera", DefaultCollapsed)
        SPostProcessSettings PostProcess;

        // Post-process materials run in order after tone mapping; each is a fullscreen pass whose Emissive
        // replaces scene color (later entries read earlier via SceneColor). Must be MaterialType = PostProcess.
        PROPERTY(Editable, Category = "Camera|Post Process")
        TVector<TObjectPtr<CMaterialInterface>> PostProcessMaterials;

    private:

        FViewVolume ViewVolume;
    };

    struct RUNTIME_API FSwitchActiveCameraEvent
    {
        entt::entity NewActiveEntity;
    };
}
