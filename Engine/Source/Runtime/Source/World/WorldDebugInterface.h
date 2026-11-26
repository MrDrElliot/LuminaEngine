#pragma once

#include "Containers/Span.h"
#include "Core/Templates/Optional.h"
#include "DebugDrawTypes.h"
#include "Renderer/PrimitiveDrawInterface.h"

namespace Lumina
{
    class CWorld;
    struct FSimpleElementVertex;

    // The debug drawing a world hands out as World.Debug, queueing lines, text and solids for its renderer.
    struct RUNTIME_API FWorldDebugInterface : IPrimitiveDrawInterface
    {
        CWorld* World = nullptr;

        using IPrimitiveDrawInterface::DrawBox;
        using IPrimitiveDrawInterface::DrawSphere;
        using IPrimitiveDrawInterface::DrawCapsule;
        using IPrimitiveDrawInterface::DrawCone;
        using IPrimitiveDrawInterface::DrawArrow;

        void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) override;
        void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f) override;
        void DrawText(FStringView Text, TOptional<FVector4> Color);
        // Centered on where Location lands on screen, over the scene; shrinks with distance, Scale multiplies its size.
        void DrawWorldText(FStringView Text, FVector3 Location, TOptional<FVector4> Color, float Scale = 1.0f);

        //~ Wireframe shapes, where an unset option takes the interface default.

        void DrawLine(FVector3 Start, FVector3 End, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawSphere(FVector3 Center, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);
        void DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color, TOptional<float> Thickness, TOptional<bool> bDepthTest, TOptional<float> Duration);

        //~ Solid shapes.

        void DrawSolidTriangles(TVector<FSimpleElementVertex>&& Vertices, ESolidDrawMode Mode = ESolidDrawMode::Translucent, float Duration = -1.0f);
        void DrawSolidTriangles(TSpan<const SDebugDrawVertex> Vertices, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidTriangle(FVector3 A, FVector3 B, FVector3 C, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidQuad(FVector3 A, FVector3 B, FVector3 C, FVector3 D, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidSphere(FVector3 Center, float Radius, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle, float SweepAngle, FVector4 Color, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
        void DrawSolidArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color, float HeadSize = 0.2f, EDebugFillMode Mode = EDebugFillMode::Translucent, float Duration = -1.0f);
    };
}
