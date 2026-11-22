#include "RuntimePCH.h"
#include "World/Subsystems/DebugSubsystem.h"
#include "Core/Math/Math.h"
#include "Renderer/Vertex.h"
#include "World/World.h"

#if USING(WITH_DEV_UI)
#include <imgui.h>
#include <imgui_internal.h>
#endif

namespace Lumina
{
    void CDebugSubsystem::DrawLine(FVector3 Start, FVector3 End, FVector4 Color,
        float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawLine(Start, End, Color, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSphere(FVector3 Center, float Radius, FVector4 Color,
        float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSphere(Center, Radius, Color, 16, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
        FVector4 Color, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawBox(Center, HalfExtents, Rotation, Color, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawBoxCorners(FVector3 Center, FVector3 HalfExtents, FQuat Rotation,
        FVector4 Color, float CornerFraction, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawBoxCorners(Center, HalfExtents, Rotation, Color, CornerFraction, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
        float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawCapsule(Start, End, Radius, Color, 16, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length,
        FVector4 Color, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawCone(Apex, Direction, AngleRadians, Length, Color, 16, 4, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color,
        float HeadSize, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawArrow(Start, Direction, Length, Color, Thickness, bDepthTest, Duration, HeadSize);
        }
        #endif
    }

    void CDebugSubsystem::DrawCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color,
        uint8 Segments, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawCircle(Center, Normal, Radius, Color, Segments, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle,
        float SweepAngle, FVector4 Color, uint8 Segments, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawArc(Center, Normal, Radius, StartAngle, SweepAngle, Color, Segments, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
        uint8 Segments, float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawCylinder(Start, End, Radius, Color, Segments, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawAxes(FVector3 Origin, FQuat Rotation, float Length,
        float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawAxes(Origin, Rotation, Length, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawPoint(FVector3 Position, float Size, FVector4 Color,
        float Thickness, float Duration, bool bDepthTest)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawPoint(Position, Size, Color, Thickness, bDepthTest, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidTriangles(const SDebugDrawVertex* Vertices, int32 Count,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr && Vertices != nullptr && Count > 0 && Count % 3 == 0)
        {
            World->GetDebugInterface()->DrawSolidTriangles(TSpan<const SDebugDrawVertex>(Vertices, (size_t)Count), Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidTriangle(FVector3 A, FVector3 B, FVector3 C, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidTriangle(A, B, C, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidQuad(FVector3 A, FVector3 B, FVector3 C, FVector3 D, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidQuad(A, B, C, D, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidBox(Center, HalfExtents, Rotation, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidSphere(FVector3 Center, float Radius, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidSphere(Center, Radius, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidCylinder(Start, End, Radius, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length,
        FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidCone(Apex, Direction, AngleRadians, Length, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidCapsule(Start, End, Radius, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color,
        EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidCircle(Center, Normal, Radius, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle,
        float SweepAngle, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidArc(Center, Normal, Radius, StartAngle, SweepAngle, Color, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawSolidArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color,
        float HeadSize, EDebugFillMode Mode, float Duration)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawSolidArrow(Start, Direction, Length, Color, HeadSize, Mode, Duration);
        }
        #endif
    }

    void CDebugSubsystem::DrawText(const FString& Text, FVector4 Color)
    {
        CWorld* World = GetWorld();
        #if !defined(LE_SHIPPING)
        if (World != nullptr)
        {
            World->GetDebugInterface()->DrawText(Text, Color);
        }
        #endif
    }

    #if USING(WITH_DEV_UI)
    void CDebugSubsystem::BroadcastDebugGui()
    {
        if (ImGui::GetCurrentContext() == nullptr || !OnDebugGui.IsBound())
        {
            return;
        }

        // A handler's unbalanced Begin is recovered rather than asserted on, then the host's own setting comes back.
        ImGuiIO& IO = ImGui::GetIO();
        const bool bAssertOnRecovery = IO.ConfigErrorRecoveryEnableAssert;
        IO.ConfigErrorRecoveryEnableAssert = false;

        OnDebugGui.BroadcastGuarded([](FScriptDelegateBase::FThunk Thunk, void* Context, const void* Payload)
        {
            ImGuiErrorRecoveryState Recovery;
            ImGui::ErrorRecoveryStoreState(&Recovery);
            Thunk(Context, Payload);
            ImGui::ErrorRecoveryTryToRecoverState(&Recovery);
        });

        IO.ConfigErrorRecoveryEnableAssert = bAssertOnRecovery;
    }
    #endif
}
