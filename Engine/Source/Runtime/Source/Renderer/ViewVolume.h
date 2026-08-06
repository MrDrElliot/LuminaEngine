#pragma once

#include "Core/DisableAllWarnings.h"
#include "Core/Math/Frustum.h"
#include "Core/Object/ObjectMacros.h"
PRAGMA_DISABLE_ALL_WARNINGS
#include "Core/Math/Math.h"
PRAGMA_ENABLE_ALL_WARNINGS

namespace Lumina
{
    enum class EViewProjectionMode : uint8
    {
        Perspective,
        Orthographic,
    };

    class RUNTIME_API FViewVolume
    {
    public:

        static constexpr float DefaultFarPlane = 4000.0f;
        static constexpr float DefaultNearPlane = 0.01f;

        FViewVolume(float fov = 90.0f, float aspect = 16.0f / 9.0f, float InNear = DefaultNearPlane, float InFar = DefaultFarPlane);

        FViewVolume& SetNear(float InNear);
        FViewVolume& SetFar(float InFar);
        FViewVolume& SetViewPosition(const FVector3& Position);
        FViewVolume& SetView(const FVector3& Position, const FVector3& ViewDirection, const FVector3& UpDirection);
        FViewVolume& SetPerspective(float InFov, float InAspect);

        /** InWidth is the world-space width the viewport spans; height follows from the aspect. */
        FViewVolume& SetOrthographic(float InWidth, float InAspect);

        FViewVolume& SetAspectRatio(float InAspect);
        FViewVolume& SetFOV(float InFOV);
        FViewVolume& Rotate(float Angle, FVector3 Axis);

        FORCEINLINE EViewProjectionMode GetProjectionMode() const { return ProjectionMode; }
        FORCEINLINE bool IsOrthographic() const { return ProjectionMode == EViewProjectionMode::Orthographic; }
        FORCEINLINE float GetOrthoWidth() const { return OrthoWidth; }
        
        FORCEINLINE const FVector3& GetViewPosition() const { return ViewPosition; }

        FORCEINLINE const FMatrix4& GetViewMatrix() const { return ViewMatrix; }
        FORCEINLINE FMatrix4 GetInverseViewMatrix() const { return Math::Inverse(ViewMatrix); }
        FORCEINLINE const FMatrix4& GetViewProjectionMatrix() const { return ViewProjectionMatrix; }
        FORCEINLINE const FMatrix4& GetProjectionMatrix() const { return ProjectionMatrix; }
        FORCEINLINE FMatrix4 GetInverseProjectionMatrix() const { return Math::Inverse(ProjectionMatrix); }
        FORCEINLINE const FVector3& GetForwardVector() const { return ForwardVector; }
        FORCEINLINE const FVector3& GetRightVector() const { return RightVector; }
        FORCEINLINE const FVector3& GetUpVector() const { return UpVector; }
        FMatrix4 ToReverseDepthViewProjectionMatrix() const;

        FORCEINLINE float GetNear() const { return Near; }
        FORCEINLINE float GetFar() const { return Far; }
        FFrustum GetFrustum() const;
        FORCEINLINE float GetFOV() const { return FOV; }
        FORCEINLINE float GetAspectRatio() const { return AspectRatio; }

        static FVector3 UpAxis;
        static FVector3 DownAxis;
        static FVector3 RightAxis;
        static FVector3 LeftAxis;
        static FVector3 ForwardAxis;
        static FVector3 BackwardAxis;
        
    private:

        void UpdateMatrices();

        // Single owner of ProjectionMatrix; every setter routes here so none of them can silently
        // drop an orthographic volume back to perspective.
        void RebuildProjection();

        EViewProjectionMode ProjectionMode = EViewProjectionMode::Perspective;

        FVector3           ViewPosition;
        FVector3           ForwardVector;
        FVector3           UpVector;
        FVector3           RightVector;

        FMatrix4           ProjectionMatrix;
        FMatrix4           ViewMatrix;
        FMatrix4           ViewProjectionMatrix;

        float               Near;
        float               Far;
        
        float               FOV = 90.0f;
        float               AspectRatio = 16.0f/9.0f;
        float               OrthoWidth = 10.0f;
    };
}
