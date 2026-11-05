#include "RuntimePCH.h"
#include "WorldDebugInterface.h"

#include "Core/Math/Math.h"
#include "Renderer/Vertex.h"
#include "World/World.h"

namespace Lumina
{
    #if !defined(LE_SHIPPING)
    namespace
    {
        ESolidDrawMode ToSolidMode(EDebugFillMode Mode)
        {
            switch (Mode)
            {
            case EDebugFillMode::Opaque: return ESolidDrawMode::Opaque;
            case EDebugFillMode::XRay: return ESolidDrawMode::XRay;
            default: return ESolidDrawMode::Translucent;
            }
        }

        void AddTriangle(TVector<FSimpleElementVertex>& Vertices, const FVector3& A, const FVector3& B,
            const FVector3& C, uint32 Color)
        {
            Vertices.Emplace(A, Color);
            Vertices.Emplace(B, Color);
            Vertices.Emplace(C, Color);
        }

        void AddQuad(TVector<FSimpleElementVertex>& Vertices, const FVector3& A, const FVector3& B,
            const FVector3& C, const FVector3& D, uint32 Color)
        {
            AddTriangle(Vertices, A, B, C, Color);
            AddTriangle(Vertices, A, C, D, Color);
        }

        FVector3 RingOffset(const FVector3& Right, const FVector3& Up, float Radius, float Angle)
        {
            return Radius * (Math::Cos(Angle) * Right + Math::Sin(Angle) * Up);
        }

        void AddRingBand(TVector<FSimpleElementVertex>& Vertices, const FVector3& FirstCenter, float FirstRadius,
            const FVector3& SecondCenter, float SecondRadius, const FVector3& Right, const FVector3& Up, uint32 Color, int32 Segments)
        {
            for (int32 Index = 0; Index < Segments; ++Index)
            {
                const float FirstAngle = Math::TwoPi<float>() * (float)Index / (float)Segments;
                const float SecondAngle = Math::TwoPi<float>() * (float)(Index + 1) / (float)Segments;
                const FVector3 A = FirstCenter + RingOffset(Right, Up, FirstRadius, FirstAngle);
                const FVector3 B = FirstCenter + RingOffset(Right, Up, FirstRadius, SecondAngle);
                const FVector3 C = SecondCenter + RingOffset(Right, Up, SecondRadius, SecondAngle);
                const FVector3 D = SecondCenter + RingOffset(Right, Up, SecondRadius, FirstAngle);
                AddQuad(Vertices, A, B, C, D, Color);
            }
        }

        void AddDisk(TVector<FSimpleElementVertex>& Vertices, const FVector3& Center, float Radius,
            const FVector3& Right, const FVector3& Up, uint32 Color, int32 Segments)
        {
            for (int32 Index = 0; Index < Segments; ++Index)
            {
                const float FirstAngle = Math::TwoPi<float>() * (float)Index / (float)Segments;
                const float SecondAngle = Math::TwoPi<float>() * (float)(Index + 1) / (float)Segments;
                AddTriangle(Vertices, Center, Center + RingOffset(Right, Up, Radius, FirstAngle),
                    Center + RingOffset(Right, Up, Radius, SecondAngle), Color);
            }
        }

        bool MakeBasis(const FVector3& Direction, FVector3& Axis, FVector3& Right, FVector3& Up)
        {
            const float Length = Math::Length(Direction);
            if (Length <= 0.0f)
            {
                return false;
            }

            Axis = Direction / Length;
            const FVector3 Reference = Math::Abs(Axis.y) < 0.99f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
            Right = Math::Normalize(Math::Cross(Reference, Axis));
            Up = Math::Cross(Axis, Right);
            return true;
        }
    }
    #endif

    void FWorldDebugInterface::DrawSolidTriangles(TSpan<const SDebugDrawVertex> Input, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Input.IsEmpty() || Input.Num() % 3 != 0)
        {
            return;
        }

        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Input.Num());
        for (const SDebugDrawVertex& Vertex : Input)
        {
            Vertices.Emplace(Vertex.Position, PackColor(Vertex.Color));
        }
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidTriangle(FVector3 A, FVector3 B, FVector3 C, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(3);
        AddTriangle(Vertices, A, B, C, PackColor(Color));
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidQuad(FVector3 A, FVector3 B, FVector3 C, FVector3 D, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(6);
        AddQuad(Vertices, A, B, C, D, PackColor(Color));
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidBox(FVector3 Center, FVector3 HalfExtents, FQuat Rotation, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        const FVector3 Local[8] =
        {
            { -HalfExtents.x, -HalfExtents.y, -HalfExtents.z },
            {  HalfExtents.x, -HalfExtents.y, -HalfExtents.z },
            {  HalfExtents.x,  HalfExtents.y, -HalfExtents.z },
            { -HalfExtents.x,  HalfExtents.y, -HalfExtents.z },
            { -HalfExtents.x, -HalfExtents.y,  HalfExtents.z },
            {  HalfExtents.x, -HalfExtents.y,  HalfExtents.z },
            {  HalfExtents.x,  HalfExtents.y,  HalfExtents.z },
            { -HalfExtents.x,  HalfExtents.y,  HalfExtents.z },
        };
        FVector3 Corners[8];
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Corners[Index] = Center + Math::Rotate(Rotation, Local[Index]);
        }

        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(36);
        const uint32 Packed = PackColor(Color);
        AddQuad(Vertices, Corners[0], Corners[3], Corners[2], Corners[1], Packed);
        AddQuad(Vertices, Corners[4], Corners[5], Corners[6], Corners[7], Packed);
        AddQuad(Vertices, Corners[0], Corners[1], Corners[5], Corners[4], Packed);
        AddQuad(Vertices, Corners[3], Corners[7], Corners[6], Corners[2], Packed);
        AddQuad(Vertices, Corners[0], Corners[4], Corners[7], Corners[3], Packed);
        AddQuad(Vertices, Corners[1], Corners[2], Corners[6], Corners[5], Packed);
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidSphere(FVector3 Center, float Radius, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Radius <= 0.0f)
        {
            return;
        }

        constexpr int32 Segments = 16;
        constexpr int32 Rings = 8;
        const FVector3 Axis(0.0f, 1.0f, 0.0f);
        const FVector3 Right(1.0f, 0.0f, 0.0f);
        const FVector3 Up(0.0f, 0.0f, 1.0f);
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * Rings * 6);
        const uint32 Packed = PackColor(Color);
        for (int32 Ring = 0; Ring < Rings; ++Ring)
        {
            const float First = -Math::HalfPi<float>() + Math::Pi<float>() * (float)Ring / (float)Rings;
            const float Second = -Math::HalfPi<float>() + Math::Pi<float>() * (float)(Ring + 1) / (float)Rings;
            AddRingBand(Vertices, Center + Axis * (Radius * Math::Sin(First)), Radius * Math::Cos(First),
                Center + Axis * (Radius * Math::Sin(Second)), Radius * Math::Cos(Second), Right, Up, Packed, Segments);
        }
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidCylinder(FVector3 Start, FVector3 End, float Radius, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Radius <= 0.0f)
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(End - Start, Axis, Right, Up))
        {
            return;
        }

        constexpr int32 Segments = 16;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * 12);
        const uint32 Packed = PackColor(Color);
        AddRingBand(Vertices, Start, Radius, End, Radius, Right, Up, Packed, Segments);
        AddDisk(Vertices, Start, Radius, Right, Up, Packed, Segments);
        AddDisk(Vertices, End, Radius, Right, Up, Packed, Segments);
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidCone(FVector3 Apex, FVector3 Direction, float AngleRadians, float Length, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Length <= 0.0f || AngleRadians <= 0.0f || AngleRadians >= Math::HalfPi<float>())
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(Direction, Axis, Right, Up))
        {
            return;
        }

        constexpr int32 Segments = 16;
        const FVector3 Base = Apex + Axis * Length;
        const float Radius = Math::Tan(AngleRadians) * Length;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * 9);
        const uint32 Packed = PackColor(Color);
        AddRingBand(Vertices, Apex, 0.0f, Base, Radius, Right, Up, Packed, Segments);
        AddDisk(Vertices, Base, Radius, Right, Up, Packed, Segments);
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidCapsule(FVector3 Start, FVector3 End, float Radius, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Radius <= 0.0f)
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(End - Start, Axis, Right, Up))
        {
            DrawSolidSphere(Start, Radius, Color, Mode, Duration);
            return;
        }

        constexpr int32 Segments = 16;
        constexpr int32 Rings = 4;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * (Rings * 2 + 1) * 6);
        const uint32 Packed = PackColor(Color);
        for (int32 Ring = 0; Ring < Rings; ++Ring)
        {
            const float First = -Math::HalfPi<float>() + Math::HalfPi<float>() * (float)Ring / (float)Rings;
            const float Second = -Math::HalfPi<float>() + Math::HalfPi<float>() * (float)(Ring + 1) / (float)Rings;
            AddRingBand(Vertices, Start + Axis * (Radius * Math::Sin(First)), Radius * Math::Cos(First),
                Start + Axis * (Radius * Math::Sin(Second)), Radius * Math::Cos(Second), Right, Up, Packed, Segments);
        }
        AddRingBand(Vertices, Start, Radius, End, Radius, Right, Up, Packed, Segments);
        for (int32 Ring = 0; Ring < Rings; ++Ring)
        {
            const float First = Math::HalfPi<float>() * (float)Ring / (float)Rings;
            const float Second = Math::HalfPi<float>() * (float)(Ring + 1) / (float)Rings;
            AddRingBand(Vertices, End + Axis * (Radius * Math::Sin(First)), Radius * Math::Cos(First),
                End + Axis * (Radius * Math::Sin(Second)), Radius * Math::Cos(Second), Right, Up, Packed, Segments);
        }
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidCircle(FVector3 Center, FVector3 Normal, float Radius, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Radius <= 0.0f)
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(Normal, Axis, Right, Up))
        {
            return;
        }

        constexpr int32 Segments = 32;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * 3);
        AddDisk(Vertices, Center, Radius, Right, Up, PackColor(Color), Segments);
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidArc(FVector3 Center, FVector3 Normal, float Radius, float StartAngle,
        float SweepAngle, FVector4 Color, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Radius <= 0.0f || SweepAngle == 0.0f)
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(Normal, Axis, Right, Up))
        {
            return;
        }

        constexpr int32 Segments = 32;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * 3);
        const uint32 Packed = PackColor(Color);
        for (int32 Index = 0; Index < Segments; ++Index)
        {
            const float First = StartAngle + SweepAngle * (float)Index / (float)Segments;
            const float Second = StartAngle + SweepAngle * (float)(Index + 1) / (float)Segments;
            AddTriangle(Vertices, Center, Center + RingOffset(Right, Up, Radius, First),
                Center + RingOffset(Right, Up, Radius, Second), Packed);
        }
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }

    void FWorldDebugInterface::DrawSolidArrow(FVector3 Start, FVector3 Direction, float Length, FVector4 Color,
        float HeadSize, EDebugFillMode Mode, float Duration)
    {
        #if !defined(LE_SHIPPING)
        if (Length <= 0.0f || HeadSize <= 0.0f)
        {
            return;
        }

        FVector3 Axis, Right, Up;
        if (!MakeBasis(Direction, Axis, Right, Up))
        {
            return;
        }

        constexpr int32 Segments = 12;
        const float HeadLength = Math::Min(Length * 0.3f, HeadSize * 2.0f);
        const FVector3 Tip = Start + Axis * Length;
        const FVector3 HeadBase = Tip - Axis * HeadLength;
        const float ShaftRadius = HeadSize * 0.25f;
        TVector<FSimpleElementVertex> Vertices;
        Vertices.Reserve(Segments * 18);
        const uint32 Packed = PackColor(Color);
        AddRingBand(Vertices, Start, ShaftRadius, HeadBase, ShaftRadius, Right, Up, Packed, Segments);
        AddDisk(Vertices, Start, ShaftRadius, Right, Up, Packed, Segments);
        AddRingBand(Vertices, HeadBase, HeadSize, Tip, 0.0f, Right, Up, Packed, Segments);
        AddDisk(Vertices, HeadBase, HeadSize, Right, Up, Packed, Segments);
        DrawSolidTriangles(std::move(Vertices), ToSolidMode(Mode), Duration);
        #endif
    }
}
