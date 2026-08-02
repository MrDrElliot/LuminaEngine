#pragma once
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FRHIImage;
    class FViewVolume;

    class RUNTIME_API IPrimitiveDrawInterface
    {
    public:
        
        IPrimitiveDrawInterface() = default;
        virtual ~IPrimitiveDrawInterface() = default;
        IPrimitiveDrawInterface(const IPrimitiveDrawInterface&) = default;
        IPrimitiveDrawInterface& operator=(const IPrimitiveDrawInterface&) = default;
        IPrimitiveDrawInterface(IPrimitiveDrawInterface&&) = default;
        IPrimitiveDrawInterface& operator=(IPrimitiveDrawInterface&&) = default;
        
        // ResourceID = global-heap slot of the billboard texture (e.g. CTexture::GetResourceID()).
        virtual void DrawBillboard(int32 ResourceID, const FVector3& Location, float Scale) = 0;
        
        
        virtual void DrawLine(const FVector3& Start, const FVector3& End, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f) = 0;
        void DrawBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawBoxCorners(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, const FVector4& Color, float CornerFraction = 0.25f, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawSphere(const FVector3& Center, float Radius, const FVector4& Color, uint8 Segments = 16, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawCapsule(const FVector3& Start, const FVector3& End, float Radius, const FVector4& Color, uint8 Segments = 16, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawCone(const FVector3& Apex, const FVector3& Direction, float AngleRadians, float Length, const FVector4& Color, uint8 Segments = 16, uint8 Stacks = 4, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawFrustum(const FMatrix4& Matrix, float zNear, float zFar, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        void DrawArrow(const FVector3& Start, const FVector3& Direction, float Length, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f, float HeadSize = 0.2f);
        void DrawViewVolume(const FViewVolume& ViewVolume, const FVector4& Color, float Thickness = 1.0f, bool bDepthTest = true, float Duration = 0.0f);
        
    };
}
