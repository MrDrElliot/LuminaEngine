#include "RuntimePCH.h"
#include "PrimitiveDrawInterface.h"

#include "Core/Math/Math.h"

#include "ViewVolume.h"
#include "Containers/Array.h"

namespace Lumina
{
    void IPrimitiveDrawInterface::DrawBox(const FVector3& Center, const FVector3& HalfExtents,
        const FQuat& Rotation, const FVector4& Color, float Thickness, bool bDepthTest, float Duration)
    {
        FVector3 LocalCorners[8] =
        {
            {-HalfExtents.x, -HalfExtents.y, -HalfExtents.z},
            { HalfExtents.x, -HalfExtents.y, -HalfExtents.z},
            { HalfExtents.x,  HalfExtents.y, -HalfExtents.z},
            {-HalfExtents.x,  HalfExtents.y, -HalfExtents.z},

            {-HalfExtents.x, -HalfExtents.y,  HalfExtents.z},
            { HalfExtents.x, -HalfExtents.y,  HalfExtents.z},
            { HalfExtents.x,  HalfExtents.y,  HalfExtents.z},
            {-HalfExtents.x,  HalfExtents.y,  HalfExtents.z},
        };

        FVector3 corners[8];
        for (int i = 0; i < 8; ++i)
        {
            corners[i] = Center + Math::Rotate(Rotation, LocalCorners[i]);
        }

        DrawLine(corners[0], corners[1], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[1], corners[2], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[2], corners[3], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[3], corners[0], Color, Thickness, bDepthTest, Duration);

        DrawLine(corners[4], corners[5], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[5], corners[6], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[6], corners[7], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[7], corners[4], Color, Thickness, bDepthTest, Duration);

        DrawLine(corners[0], corners[4], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[1], corners[5], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[2], corners[6], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[3], corners[7], Color, Thickness, bDepthTest, Duration);
    }

    void IPrimitiveDrawInterface::DrawBoxCorners(const FVector3& Center, const FVector3& HalfExtents,
        const FQuat& Rotation, const FVector4& Color, float CornerFraction, float Thickness, bool bDepthTest, float Duration)
    {
        const float t = Math::Clamp(CornerFraction, 0.0f, 0.5f);

        FVector3 LocalCorners[8] =
        {
            {-HalfExtents.x, -HalfExtents.y, -HalfExtents.z},
            { HalfExtents.x, -HalfExtents.y, -HalfExtents.z},
            { HalfExtents.x,  HalfExtents.y, -HalfExtents.z},
            {-HalfExtents.x,  HalfExtents.y, -HalfExtents.z},

            {-HalfExtents.x, -HalfExtents.y,  HalfExtents.z},
            { HalfExtents.x, -HalfExtents.y,  HalfExtents.z},
            { HalfExtents.x,  HalfExtents.y,  HalfExtents.z},
            {-HalfExtents.x,  HalfExtents.y,  HalfExtents.z},
        };

        FVector3 Corners[8];
        for (int i = 0; i < 8; ++i)
        {
            Corners[i] = Center + Math::Rotate(Rotation, LocalCorners[i]);
        }

        // The three neighbor corners along each axis for each of the 8 corners.
        static constexpr int Neighbors[8][3] =
        {
            {1, 3, 4},
            {0, 2, 5},
            {3, 1, 6},
            {2, 0, 7},
            {5, 7, 0},
            {4, 6, 1},
            {7, 5, 2},
            {6, 4, 3},
        };

        for (int i = 0; i < 8; ++i)
        {
            for (int n = 0; n < 3; ++n)
            {
                const FVector3& Far = Corners[Neighbors[i][n]];
                const FVector3 Stub = Corners[i] + (Far - Corners[i]) * t;
                DrawLine(Corners[i], Stub, Color, Thickness, bDepthTest, Duration);
            }
        }
    }

    void IPrimitiveDrawInterface::DrawSphere(const FVector3& Center, float Radius, const FVector4& Color,
        uint8 Segments, float Thickness, bool bDepthTest, float Duration)
    {
        for (uint8 lat = 1; lat < Segments; ++lat)
        {
            float latAngle = Math::Pi<float>() * lat / Segments;
            float y = Radius * cos(latAngle);
            float ringRadius = Radius * sin(latAngle);

            for (int lon = 0; lon < Segments; ++lon)
            {
                float lonAngle1 = Math::TwoPi<float>() * lon / Segments;
                float lonAngle2 = Math::TwoPi<float>() * (lon + 1) / Segments;

                FVector3 p1 = Center + FVector3(ringRadius * cos(lonAngle1), y, ringRadius * sin(lonAngle1));
                FVector3 p2 = Center + FVector3(ringRadius * cos(lonAngle2), y, ringRadius * sin(lonAngle2));

                DrawLine(p1, p2, Color, Thickness, bDepthTest, Duration);
            }
        }

        for (uint8 lon = 0; lon < Segments; ++lon)
        {
            float lonAngle = Math::TwoPi<float>() * lon / Segments;

            for (int lat = 0; lat < Segments; ++lat)
            {
                float latAngle1 = Math::Pi<float>() * lat / Segments;
                float latAngle2 = Math::Pi<float>() * (lat + 1) / Segments;

                FVector3 p1 = Center + FVector3(Radius * sin(latAngle1) * cos(lonAngle),
                                                  Radius * cos(latAngle1),
                                                  Radius * sin(latAngle1) * sin(lonAngle));

                FVector3 p2 = Center + FVector3(Radius * sin(latAngle2) * cos(lonAngle),
                                                  Radius * cos(latAngle2),
                                                  Radius * sin(latAngle2) * sin(lonAngle));

                DrawLine(p1, p2, Color, Thickness, bDepthTest, Duration);
            }
        }
    }

    void IPrimitiveDrawInterface::DrawCapsule(const FVector3& Start, const FVector3& End, float Radius, const FVector4& Color, uint8 Segments, float Thickness, bool bDepthTest, float Duration)
    {
        FVector3 axis = End - Start;
        float height = Math::Length(axis);
        FVector3 direction = height > 0.0f ? axis / height : FVector3(0, 1, 0);
        
        FVector3 up = Math::Abs(direction.y) < 0.999f ? FVector3(0, 1, 0) : FVector3(1, 0, 0);
        FVector3 right = Math::Normalize(Math::Cross(up, direction));
        FVector3 forward = Math::Cross(direction, right);
        
        for (uint8 i = 0; i <= Segments; ++i)
        {
            float t = static_cast<float>(i) / Segments;
            FVector3 center = Math::Mix(Start, End, t);
            
            for (uint8 lon = 0; lon < Segments; ++lon)
            {
                float angle1 = Math::TwoPi<float>() * lon / Segments;
                float angle2 = Math::TwoPi<float>() * (lon + 1) / Segments;
                
                FVector3 p1 = center + Radius * (cos(angle1) * right + sin(angle1) * forward);
                FVector3 p2 = center + Radius * (cos(angle2) * right + sin(angle2) * forward);
                
                DrawLine(p1, p2, Color, Thickness, bDepthTest, Duration);
            }
        }
        
        for (uint8 lon = 0; lon < Segments; ++lon)
        {
            float angle = Math::TwoPi<float>() * lon / Segments;
            FVector3 offset = Radius * (cos(angle) * right + sin(angle) * forward);
            
            DrawLine(Start + offset, End + offset, Color, Thickness, bDepthTest, Duration);
        }
        
        for (uint8 lat = 1; lat <= Segments / 2; ++lat)
        {
            float latAngle = Math::HalfPi<float>() * lat / (Segments / 2);
            float y = Radius * sin(latAngle);
            float ringRadius = Radius * cos(latAngle);
            
            for (uint8 lon = 0; lon < Segments; ++lon)
            {
                float lonAngle1 = Math::TwoPi<float>() * lon / Segments;
                float lonAngle2 = Math::TwoPi<float>() * (lon + 1) / Segments;
                
                FVector3 offset1 = ringRadius * (cos(lonAngle1) * right + sin(lonAngle1) * forward) + y * direction;
                FVector3 offset2 = ringRadius * (cos(lonAngle2) * right + sin(lonAngle2) * forward) + y * direction;
                
                DrawLine(End + offset1, End + offset2, Color, Thickness, bDepthTest, Duration);
            }
        }
        
        for (uint8 lat = 1; lat <= Segments / 2; ++lat)
        {
            float latAngle = Math::HalfPi<float>() * lat / (Segments / 2);
            float y = Radius * sin(latAngle);
            float ringRadius = Radius * cos(latAngle);
            
            for (uint8 lon = 0; lon < Segments; ++lon)
            {
                float lonAngle1 = Math::TwoPi<float>() * lon / Segments;
                float lonAngle2 = Math::TwoPi<float>() * (lon + 1) / Segments;
                
                FVector3 offset1 = ringRadius * (cos(lonAngle1) * right + sin(lonAngle1) * forward) - y * direction;
                FVector3 offset2 = ringRadius * (cos(lonAngle2) * right + sin(lonAngle2) * forward) - y * direction;
                
                DrawLine(Start + offset1, Start + offset2, Color, Thickness, bDepthTest, Duration);
            }
        }
        
        for (uint8 lon = 0; lon < Segments; ++lon)
        {
            float lonAngle = Math::TwoPi<float>() * lon / Segments;
            FVector3 radialDir = cos(lonAngle) * right + sin(lonAngle) * forward;
            
            for (uint8 lat = 0; lat < Segments / 2; ++lat)
            {
                float latAngle1 = Math::HalfPi<float>() * lat / (Segments / 2);
                float latAngle2 = Math::HalfPi<float>() * (lat + 1) / (Segments / 2);
                
                FVector3 p1 = End + Radius * (cos(latAngle1) * radialDir + sin(latAngle1) * direction);
                FVector3 p2 = End + Radius * (cos(latAngle2) * radialDir + sin(latAngle2) * direction);
                
                DrawLine(p1, p2, Color, Thickness, Duration);
            }
            
            for (uint8 lat = 0; lat < Segments / 2; ++lat)
            {
                float latAngle1 = Math::HalfPi<float>() * lat / (Segments / 2);
                float latAngle2 = Math::HalfPi<float>() * (lat + 1) / (Segments / 2);
                
                FVector3 p1 = Start + Radius * (cos(latAngle1) * radialDir - sin(latAngle1) * direction);
                FVector3 p2 = Start + Radius * (cos(latAngle2) * radialDir - sin(latAngle2) * direction);
                
                DrawLine(p1, p2, Color, Thickness, bDepthTest, Duration);
            }
        }
    }

    void IPrimitiveDrawInterface::DrawCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians,
                                           float Length, const FVector4& Color, uint8 Segments, uint8 Stacks, float Thickness, bool bDepthTest, float Duration)
    {
        FVector3 dir = Math::Normalize(Direction);
        
        FVector3 up = Math::Abs(dir.y) < 0.99f ? FVector3(0,1,0) : FVector3(1,0,0);
        FVector3 right = Math::Normalize(Math::Cross(dir, up));
        FVector3 coneUp = Math::Normalize(Math::Cross(right, dir));
        
        for (uint8 stack = 1; stack <= Stacks; ++stack)
        {
            float t = (float)stack / Stacks;
            float ringLength = t * Length;
            float ringRadius = ringLength * tan(AngleRadians);
        
            TVector<FVector3> circlePoints(Segments);
            for (int i = 0; i < Segments; ++i)
            {
                float theta = Math::TwoPi<float>() * i / Segments;
                circlePoints[i] = Apex + dir * ringLength + ringRadius * (cos(theta) * right + sin(theta) * coneUp);
            }
        
            for (uint8 i = 0; i < Segments; ++i)
            {
                DrawLine(circlePoints[i], circlePoints[(i + 1) % Segments], Color, Thickness, bDepthTest, Duration);
            }
        
            for (uint8 i = 0; i < Segments; ++i)
            {
                DrawLine(Apex, circlePoints[i], Color, Thickness, bDepthTest, Duration);
            }
        
            if (stack > 1)
            {
                float tPrev = (float)(stack - 1) / Stacks;
                float prevLength = tPrev * Length;
                float prevRadius = prevLength * tan(AngleRadians);
        
                for (int i = 0; i < Segments; ++i)
                {
                    FVector3 prevPoint = Apex + dir * prevLength + prevRadius * (cos(2*Math::Pi<float>() * i / Segments) * right +
                                                                               sin(2*Math::Pi<float>() * i / Segments) * coneUp);
                    DrawLine(prevPoint, circlePoints[i], Color, Thickness, bDepthTest, Duration);
                }
            }
        }
    }

    void IPrimitiveDrawInterface::DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness, bool bDepthTest, float Duration)
    {
        auto UnprojectCorner = [&](float x, float y, float z) -> FVector3
        {
            FVector4 ndc(x, y, z, 1.0f);
            FVector4 world = Math::Inverse(Matrix) * ndc;
            return FVector3(world) / world.w;
        };

        FVector3 corners[8];

        corners[0] = UnprojectCorner(-1, -1, zNear);
        corners[1] = UnprojectCorner( 1, -1, zNear);
        corners[2] = UnprojectCorner( 1,  1, zNear);
        corners[3] = UnprojectCorner(-1,  1, zNear);

        corners[4] = UnprojectCorner(-1, -1, zFar);
        corners[5] = UnprojectCorner( 1, -1, zFar);
        corners[6] = UnprojectCorner( 1,  1, zFar);
        corners[7] = UnprojectCorner(-1,  1, zFar);


        DrawLine(corners[0], corners[1], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[1], corners[2], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[2], corners[3], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[3], corners[0], Color, Thickness, bDepthTest, Duration);

        DrawLine(corners[4], corners[5], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[5], corners[6], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[6], corners[7], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[7], corners[4], Color, Thickness, bDepthTest, Duration);

        DrawLine(corners[0], corners[4], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[1], corners[5], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[2], corners[6], Color, Thickness, bDepthTest, Duration);
        DrawLine(corners[3], corners[7], Color, Thickness, bDepthTest, Duration);
    }

    void IPrimitiveDrawInterface::DrawArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness, bool bDepthTest, float Duration, float HeadSize)
    {
        FVector3 End = Start + Math::Normalize(Direction) * Length;

        DrawLine(Start, End, Color, Thickness, bDepthTest, Duration);

        FVector3 Up(0, 1, 0);
        if (Math::Abs(Math::Dot(Math::Normalize(Direction), Up)) > 0.99f)
        {
            Up = FVector3(1, 0, 0);
        }
        FVector3 Right = Math::Normalize(Math::Cross(Direction, Up));
        Up = Math::Normalize(Math::Cross(Right, Direction));

        FVector3 Tip = End;
        FVector3 BaseCenter = End - Math::Normalize(Direction) * HeadSize;

        FVector3 Corner1 = BaseCenter + (Up + Right) * HeadSize * 0.5f;
        FVector3 Corner2 = BaseCenter + (Up - Right) * HeadSize * 0.5f;
        FVector3 Corner3 = BaseCenter + (-Up - Right) * HeadSize * 0.5f;
        FVector3 Corner4 = BaseCenter + (-Up + Right) * HeadSize * 0.5f;

        DrawLine(Tip, Corner1, Color, Thickness, bDepthTest, Duration);
        DrawLine(Tip, Corner2, Color, Thickness, bDepthTest, Duration);
        DrawLine(Tip, Corner3, Color, Thickness, bDepthTest, Duration);
        DrawLine(Tip, Corner4, Color, Thickness, bDepthTest, Duration);

        DrawLine(Corner1, Corner2, Color, Thickness, bDepthTest, Duration);
        DrawLine(Corner2, Corner3, Color, Thickness, bDepthTest, Duration);
        DrawLine(Corner3, Corner4, Color, Thickness, bDepthTest, Duration);
        DrawLine(Corner4, Corner1, Color, Thickness, bDepthTest, Duration);
    }

    void IPrimitiveDrawInterface::DrawViewVolume(const FViewVolume& ViewVolume, const FVector4& Color, float Thickness, bool bDepthTest, float Duration)
    {
        DrawFrustum(ViewVolume.GetViewProjectionMatrix(), ViewVolume.GetNear(), ViewVolume.GetFar(), Color, Thickness, bDepthTest, Duration);
    }
}

