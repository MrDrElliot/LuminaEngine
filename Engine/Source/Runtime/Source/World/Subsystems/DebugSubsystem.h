#pragma once

#include "Containers/String.h"
#include "Containers/Span.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Quat/Quat.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/DebugDrawTypes.h"
#include "World/Subsystems/WorldSubsystem.h"
#include "DebugSubsystem.generated.h"

namespace Lumina
{
    // Transient debug geometry, where a Duration at or below zero draws for one frame, and the world's debug windows.
    REFLECT()
    class RUNTIME_API CDebugSubsystem : public CWorldSubsystem
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        void DrawLine(FVector3 Start, FVector3 End, FVector4 Color,
            float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawSphere(FVector3 Center, float Radius, FVector4 Color,
            float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
            FVector4 Color, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawBoxCorners(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
            FVector4 Color, float CornerFraction = 0.25f, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
            float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length,
            FVector4 Color, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color,
            float HeadSize = 0.2f, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color,
            uint8 Segments = 32, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle,
            float SweepAngle, FVector4 Color, uint8 Segments = 16, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
            uint8 Segments = 16, float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawAxes(FVector3 Origin, FQuat Rotation, float Length,
            float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawPoint(FVector3 Position, float Size, FVector4 Color,
            float Thickness = 1.0f, float Duration = -1.0f, bool bDepthTest = true);

        FUNCTION()
        void DrawSolidTriangles(const SDebugDrawVertex* Vertices, int32 Count,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidTriangle(FVector3 A, FVector3 B, FVector3 C, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidQuad(FVector3 A, FVector3 B, FVector3 C, FVector3 D, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidSphere(FVector3 Center, float Radius, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length,
            FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color,
            EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle,
            float SweepAngle, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        FUNCTION()
        void DrawSolidArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color,
            float HeadSize = 0.2f, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);

        /** One screen-space line, stacked from the top left of the world viewport for this frame only. */
        FUNCTION()
        void DrawText(const FString& Text, FVector4 Color);

        // Once a frame while the developer overlay shows, between ImGui's NewFrame and Render, so handlers build debug windows.
        PROPERTY(Hidden)
        FScriptDelegate OnDebugGui;

        #if USING(WITH_DEV_UI)
        // Recovers ImGui's stacks after each handler, so one that throws or forgets End leaves the rest intact.
        void BroadcastDebugGui();
        #endif
    };
}
