#include "RuntimePCH.h"

#include "CameraLibrary.h"

#include "Physics/PhysicsLibrary.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Systems/SystemSingletons.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        const FResolvedSceneView* ViewOf(CWorld* World)
        {
            return World != nullptr ? World->GetResolvedView() : nullptr;
        }
    }

    FVector3 CCameraLibrary::GetViewPosition(CWorld* World)
    {
        const FResolvedSceneView* View = ViewOf(World);
        return View != nullptr ? View->ViewVolume.GetViewPosition() : FVector3(0.0f);
    }

    FVector3 CCameraLibrary::GetViewForward(CWorld* World)
    {
        const FResolvedSceneView* View = ViewOf(World);
        return View != nullptr ? View->ViewVolume.GetForwardVector() : FVector3(0.0f, 0.0f, 1.0f);
    }

    FVector3 CCameraLibrary::GetViewRight(CWorld* World)
    {
        const FResolvedSceneView* View = ViewOf(World);
        return View != nullptr ? View->ViewVolume.GetRightVector() : FVector3(1.0f, 0.0f, 0.0f);
    }

    FVector3 CCameraLibrary::GetViewUp(CWorld* World)
    {
        const FResolvedSceneView* View = ViewOf(World);
        return View != nullptr ? View->ViewVolume.GetUpVector() : FVector3(0.0f, 1.0f, 0.0f);
    }

    float CCameraLibrary::GetFieldOfView(CWorld* World)
    {
        const FResolvedSceneView* View = ViewOf(World);
        return View != nullptr ? View->ViewVolume.GetFOV() : 0.0f;
    }

    ECS::FEntity CCameraLibrary::GetActiveCameraEntity(CWorld* World)
    {
        return World != nullptr ? World->GetActiveCameraEntity() : ECS::NullEntity;
    }

    void CCameraLibrary::SetActiveCamera(CWorld* World, ECS::FEntity Entity, float BlendTime,
        ECameraBlendFunction BlendFunction)
    {
        if (World == nullptr)
        {
            return;
        }

        if (BlendTime > 0.0f)
        {
            World->SetActiveCamera(Entity, BlendTime, BlendFunction);
        }
        else
        {
            World->SetActiveCamera(Entity);
        }
    }

    uint32 CCameraLibrary::PlayShake(CWorld* World, FCameraShakeParams Params)
    {
        return World != nullptr
            ? SCameraSystem::PlayCameraShake(ECS::GetWorldRegistry(*World), Params)
            : 0;
    }

    uint32 CCameraLibrary::PlayImpactShake(CWorld* World, float Intensity, float Duration)
    {
        FCameraShakeParams Params;
        Params.LocationAmplitude = FVector3(Intensity * 0.04f, Intensity * 0.04f, Intensity * 0.02f);
        Params.RotationAmplitude = FVector3(Intensity * 1.5f, Intensity * 1.5f, Intensity * 0.6f);
        Params.Frequency = 18.0f;
        Params.Duration = Duration;
        Params.BlendInTime = 0.03f;
        Params.BlendOutTime = Math::Min(Duration, 0.25f);
        return PlayShake(World, Params);
    }

    void CCameraLibrary::StopShake(CWorld* World, uint32 ShakeHandle)
    {
        if (World != nullptr)
        {
            SCameraSystem::StopCameraShake(ECS::GetWorldRegistry(*World), ShakeHandle);
        }
    }

    void CCameraLibrary::StopAllShakes(CWorld* World)
    {
        if (World != nullptr)
        {
            SCameraSystem::StopAllCameraShakes(ECS::GetWorldRegistry(*World));
        }
    }

    FVector2 CCameraLibrary::GetViewportSize(CWorld* World)
    {
        IRenderScene* Scene = World != nullptr ? World->GetRenderer() : nullptr;
        if (Scene == nullptr)
        {
            return FVector2(0.0f);
        }
        const FUIntVector2 Extent = Scene->GetRenderExtent();
        return FVector2((float)Extent.x, (float)Extent.y);
    }

    FScreenProjection CCameraLibrary::WorldToScreen(CWorld* World, FVector3 WorldLocation)
    {
        FScreenProjection Result;
        const FResolvedSceneView* View = ViewOf(World);
        const FVector2 Viewport = GetViewportSize(World);
        if (View == nullptr || Viewport.x <= 0.0f || Viewport.y <= 0.0f)
        {
            return Result;
        }

        Result.bOnScreen = View->ViewVolume.WorldToScreen(WorldLocation, Viewport, Result.Position, Result.Depth);
        return Result;
    }

    FWorldRay CCameraLibrary::ScreenToWorldRay(CWorld* World, FVector2 ScreenPosition)
    {
        FWorldRay Result;
        const FResolvedSceneView* View = ViewOf(World);
        const FVector2 Viewport = GetViewportSize(World);
        if (View == nullptr || Viewport.x <= 0.0f || Viewport.y <= 0.0f)
        {
            return Result;
        }

        View->ViewVolume.ScreenToWorldRay(ScreenPosition, Viewport, Result.Origin, Result.Direction);
        Result.bValid = true;
        return Result;
    }

    FWorldRay CCameraLibrary::ViewportCenterRay(CWorld* World)
    {
        const FVector2 Viewport = GetViewportSize(World);
        return ScreenToWorldRay(World, FVector2(Viewport.x * 0.5f, Viewport.y * 0.5f));
    }

    FVector3 CCameraLibrary::DeprojectScreenToWorld(CWorld* World, FVector2 ScreenPosition, float WorldDistance)
    {
        const FWorldRay Ray = ScreenToWorldRay(World, ScreenPosition);
        return Ray.bValid ? (Ray.Origin + Ray.Direction * WorldDistance) : FVector3(0.0f);
    }

    namespace
    {
        // A hit this close short of the point is the point's own surface, not something in front of it.
        constexpr float kOcclusionSlack = 0.35f;

        bool IsUnoccluded(CWorld* World, const FVector3& Eye, const FVector3& Point, float Distance, ECS::FEntity IgnoreA, ECS::FEntity IgnoreB)
        {
            const SRayResult Hit = CPhysicsLibrary::Raycast(World, Eye, Point, IgnoreA);
            return !Hit.bHit || ECS::FEntity(Hit.Entity) == IgnoreB || Hit.Distance >= Distance - kOcclusionSlack;
        }
    }

    bool CCameraLibrary::IsPointVisible(CWorld* World, FVector3 Point, ECS::FEntity IgnoreA, ECS::FEntity IgnoreB, float MaxDistance)
    {
        if (World == nullptr)
        {
            return false;
        }

        const FVector3 Eye = GetViewPosition(World);
        const float Distance = Math::Length(Point - Eye);
        if (Distance > MaxDistance)
        {
            return false;
        }

        const FScreenProjection Projected = WorldToScreen(World, Point);
        const FVector2 Viewport = GetViewportSize(World);
        if (!Projected.bOnScreen || Projected.Depth <= 0.0f)
        {
            return false;
        }
        if (Projected.Position.x < 0.0f || Projected.Position.y < 0.0f || Projected.Position.x > Viewport.x || Projected.Position.y > Viewport.y)
        {
            return false;
        }

        return IsUnoccluded(World, Eye, Point, Distance, IgnoreA, IgnoreB);
    }

    bool CCameraLibrary::IsPointVisibleFrom(CWorld* World, FVector3 Eye, FVector3 Forward, float FovDegrees, float Aspect,
        FVector3 Point, ECS::FEntity IgnoreA, ECS::FEntity IgnoreB, float MaxDistance)
    {
        if (World == nullptr)
        {
            return false;
        }

        const FVector3 ToPoint = Point - Eye;
        const float Distance = Math::Length(ToPoint);
        if (Distance > MaxDistance || Distance < 1e-3f || Math::Length(Forward) < 1e-3f)
        {
            return false;
        }

        // Projected onto the view's own axes, which is the frustum test without a viewport.
        const FVector3 ViewForward = Math::Normalize(Forward);
        FVector3 Right = Math::Cross(FVector3(0.0f, 1.0f, 0.0f), ViewForward);
        Right = Math::Length(Right) > 1e-3f ? Math::Normalize(Right) : FVector3(1.0f, 0.0f, 0.0f);
        const FVector3 Up = Math::Cross(ViewForward, Right);
        const float Depth = Math::Dot(ToPoint, ViewForward);
        if (Depth <= 0.0f)
        {
            return false;
        }
        const float TanHalfV = Math::Tan(Math::Radians(FovDegrees * 0.5f));
        const float TanHalfH = TanHalfV * Aspect;
        if (Math::Abs(Math::Dot(ToPoint, Right)) > Depth * TanHalfH || Math::Abs(Math::Dot(ToPoint, Up)) > Depth * TanHalfV)
        {
            return false;
        }

        return IsUnoccluded(World, Eye, Point, Distance, IgnoreA, IgnoreB);
    }
}
