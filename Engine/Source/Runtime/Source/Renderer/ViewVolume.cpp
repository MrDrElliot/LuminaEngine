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

    // NDC is Vulkan here, so +y already points down the screen and depth is reversed (1 near, 0 far).
    bool FViewVolume::WorldToScreen(const FVector3& WorldLocation, const FVector2& ViewportSize,
        FVector2& OutScreen, float& OutViewDepth) const
    {
        // Tested in view space because clip.w is a constant 1 under ortho and could not answer it.
        const FVector4 ViewPos = ViewMatrix * FVector4(WorldLocation, 1.0f);
        OutViewDepth = ViewPos.z;

        const FVector4 Clip = ViewProjectionMatrix * FVector4(WorldLocation, 1.0f);
        const float W = IsOrthographic() ? 1.0f : Clip.w;
        if (Math::Abs(W) < 1e-6f)
        {
            OutScreen = FVector2(0.0f);
            return false;
        }

        const FVector2 Ndc(Clip.x / W, Clip.y / W);
        OutScreen = FVector2((Ndc.x * 0.5f + 0.5f) * ViewportSize.x,
                             (Ndc.y * 0.5f + 0.5f) * ViewportSize.y);
        return OutViewDepth > Near;
    }

    void FViewVolume::ScreenToWorldRay(const FVector2& ScreenPosition, const FVector2& ViewportSize,
        FVector3& OutOrigin, FVector3& OutDirection) const
    {
        const float SafeWidth  = Math::Max(ViewportSize.x, 1.0f);
        const float SafeHeight = Math::Max(ViewportSize.y, 1.0f);
        const float NdcX = (ScreenPosition.x / SafeWidth)  * 2.0f - 1.0f;
        const float NdcY = (ScreenPosition.y / SafeHeight) * 2.0f - 1.0f;

        const FMatrix4 InverseViewProjection = Math::Inverse(ViewProjectionMatrix);

        // Reverse-Z, so 1 unprojects to the near plane and 0 to the far one.
        const FVector4 NearH = InverseViewProjection * FVector4(NdcX, NdcY, 1.0f, 1.0f);
        const FVector4 FarH  = InverseViewProjection * FVector4(NdcX, NdcY, 0.0f, 1.0f);

        const FVector3 NearWorld = FVector3(NearH.x, NearH.y, NearH.z) / NearH.w;
        const FVector3 FarWorld  = FVector3(FarH.x, FarH.y, FarH.z) / FarH.w;

        OutOrigin = NearWorld;
        OutDirection = Math::Normalize(FarWorld - NearWorld);
    }

    FVector3 FViewVolume::DeprojectScreenToWorld(const FVector2& ScreenPosition, const FVector2& ViewportSize,
        float WorldDistance) const
    {
        FVector3 Origin;
        FVector3 Direction;
        ScreenToWorldRay(ScreenPosition, ViewportSize, Origin, Direction);
        return Origin + Direction * WorldDistance;
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
