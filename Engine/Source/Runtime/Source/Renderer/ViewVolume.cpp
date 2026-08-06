#include "RuntimePCH.h"
#include "ViewVolume.h"


namespace Lumina
{

    FVector3 FViewVolume::UpAxis        = FVector3(0.0f,  1.0f,  0.0f);
    FVector3 FViewVolume::DownAxis      = FVector3(0.0f, -1.0f,  0.0f);
    FVector3 FViewVolume::RightAxis     = FVector3(1.0f,  0.0f,  0.0f);
    FVector3 FViewVolume::LeftAxis      = FVector3(-1.0f, 0.0f,  0.0f);
    FVector3 FViewVolume::ForwardAxis   = FVector3(0.0f,  0.0f,  1.0f);
    FVector3 FViewVolume::BackwardAxis  = FVector3(0.0f,  0.0f, -1.0f);
    
    FViewVolume::FViewVolume(float fov, float aspect, float InNear, float InFar)
        : ViewPosition(FVector3(1.0))
        // Seeded so SetView's normalize/cross chain never sees uninitialized memory.
        , ForwardVector(ForwardAxis)
        , UpVector(UpAxis)
        , RightVector(RightAxis)
        , Near(InNear)
        , Far(InFar)
        , FOV(fov)
        , AspectRatio(aspect)
    {
        SetPerspective(fov, aspect);
        SetView(FVector3(0.0), ForwardAxis, UpAxis);
    }

    // Y-flip bakes Vulkan +Y-down NDC into the matrix; reverse-Z via swapped Far/Near.
    static FMatrix4 BuildVulkanReverseZPerspective(float FovDegrees, float Aspect, float Near, float Far)
    {
        FMatrix4 P = Math::Perspective(Math::Radians(FovDegrees), Aspect, Far, Near);
        P[1][1] *= -1.0f;
        return P;
    }

    static FMatrix4 BuildVulkanReverseZOrtho(float Width, float Aspect, float Near, float Far)
    {
        const float HalfWidth  = Math::Max(Width, 0.001f) * 0.5f;
        const float HalfHeight = HalfWidth / Math::Max(Aspect, 0.001f);
        FMatrix4 P = Math::Ortho(-HalfWidth, HalfWidth, -HalfHeight, HalfHeight, Far, Near);
        P[1][1] *= -1.0f;
        return P;
    }

    void FViewVolume::RebuildProjection()
    {
        ProjectionMatrix = (ProjectionMode == EViewProjectionMode::Orthographic)
            ? BuildVulkanReverseZOrtho(OrthoWidth, AspectRatio, Near, Far)
            : BuildVulkanReverseZPerspective(FOV, AspectRatio, Near, Far);
    }

    FViewVolume& FViewVolume::SetNear(float InNear)
    {
        Near = InNear;
        RebuildProjection();
        UpdateMatrices();

        return *this;
    }

    FViewVolume& FViewVolume::SetFar(float InFar)
    {
        Far = InFar;
        RebuildProjection();
        UpdateMatrices();

        return *this;
    }


    FViewVolume& FViewVolume::SetViewPosition(const FVector3& Position)
    {
        ViewPosition = Position;
        UpdateMatrices();

        return *this;
    }

    FViewVolume& FViewVolume::SetView(const FVector3& Position, const FVector3& ViewDirection, const FVector3& UpDirection)
    {
        ViewPosition    = Position;
        UpVector        = Math::Normalize(UpDirection);
        ForwardVector   = Math::Normalize(ViewDirection);
        RightVector     = Math::Normalize(Math::Cross(UpVector, ForwardVector));
        UpVector        = Math::Normalize(Math::Cross(ForwardVector, RightVector));

        UpdateMatrices();

        return *this;
    }


    FViewVolume& FViewVolume::SetPerspective(float fov, float aspect)
    {
        ProjectionMode = EViewProjectionMode::Perspective;
        FOV = fov;
        AspectRatio = aspect;

        RebuildProjection();
        UpdateMatrices();

        return *this;
    }

    FViewVolume& FViewVolume::SetOrthographic(float InWidth, float InAspect)
    {
        ProjectionMode = EViewProjectionMode::Orthographic;
        OrthoWidth = Math::Max(InWidth, 0.001f);
        AspectRatio = InAspect;

        RebuildProjection();
        UpdateMatrices();

        return *this;
    }

    FViewVolume& FViewVolume::SetAspectRatio(float InAspect)
    {
        AspectRatio = InAspect;

        RebuildProjection();
        UpdateMatrices();

        return *this;
    }

    // Authored FOV is kept even while orthographic, so toggling back restores the user's framing.
    FViewVolume& FViewVolume::SetFOV(float InFOV)
    {
        FOV = InFOV;
        RebuildProjection();
        UpdateMatrices();

        return *this;
    }

    FViewVolume& FViewVolume::Rotate(float Angle, FVector3 Axis)
    {
        float Radians   = Math::Radians(Angle);
        FMatrix4 R     = Math::Rotate(FMatrix4(1), Radians, Axis);
        
        ForwardVector = Math::Normalize(R * FVector4(ForwardVector, 0));
        UpVector      = Math::Normalize(R * FVector4(UpVector, 0));
        
        RightVector   = Math::Normalize(Math::Cross(UpVector, ForwardVector));

        UpdateMatrices();
        return *this;
    }

    FMatrix4 FViewVolume::ToReverseDepthViewProjectionMatrix() const
    {
        // Standard-Z projection (not reverse-Z) for shadow face VPs; keeps Vulkan Y-flip.
        FMatrix4 P = Math::Perspective(Math::Radians(FOV), AspectRatio, Near, Far);
        P[1][1] *= -1.0f;
        return P * ViewMatrix;
    }

    FFrustum FViewVolume::GetFrustum() const
    {
        return FFrustum::FromViewProjection(ViewProjectionMatrix);
    }
    
    void FViewVolume::UpdateMatrices()
    {
        ViewMatrix = Math::LookAt(ViewPosition, ViewPosition + ForwardVector, UpVector);
        ViewProjectionMatrix = ProjectionMatrix * ViewMatrix;
    }
}
