#pragma once

#include <algorithm>
#include "RHITexture.h"
#include "Containers/Array.h"
#include "Core/Functional/FunctionRef.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::RenderUtils
{
    // Decodes raw image bytes straight into the global texture heap; ImTextureID = ResourceID().
    RUNTIME_API RHI::FManagedTexture CreateImageFromPixels(TSpan<uint8> PixelData, bool bFlipVertically = true, FUIntVector2 Size = {});

    inline uint32 CalculateMipCount(uint32 Width, uint32 Height)
    {
        uint32 Levels = 1;
        while (Width > 1 || Height > 1)
        {
            Width = std::max(Width >> 1, 1u);
            Height = std::max(Height >> 1, 1u);
            ++Levels;
        }
        return Levels;
    }
    
    inline FUIntVector2 SplitAddress(uint64 Address) 
    {
        return FUIntVector2(static_cast<uint32>(Address & 0xFFFFFFFFull), static_cast<uint32>(Address >> 32));
    }

    inline uint32 GetMipDim(uint32 BaseWidth, uint32 Level)
    {
        return std::max(1u, BaseWidth >> Level);
    }

    inline uint32 GetGroupCount(uint32 ThreadCount, uint32 LocalSize)
    {
        return (ThreadCount + LocalSize - 1) / LocalSize;
    };

    constexpr uint32 CreateViewMask(TSpan<uint32> Layers)
    {
        uint32 Mask = 0;
        for(uint32_t layer : Layers)
        {
            Mask |= (1u << layer);
        }
        return Mask;
    }

    template<uint32... Layers>
    requires ((Layers < 32) && ...)
    constexpr uint32 CreateViewMask()
    {
        return ((1u << Layers) | ...);
    }


    inline FQuat GetCameraRotation(float yaw, float pitch)
    {
        float yawRad = Math::Radians(yaw);
        float pitchRad = Math::Radians(pitch);

        FQuat pitchQuat = Math::AngleAxis(pitchRad, FVector3(1, 0, 0));
        FQuat yawQuat = Math::AngleAxis(yawRad, FVector3(0, 1, 0));

        return yawQuat * pitchQuat;
    }

    inline FVector3 GetForwardVector(float yaw, float pitch)
    {
        float yawRad = Math::Radians(yaw);
        float pitchRad = Math::Radians(pitch);
        
        return FVector3(
            Math::Cos(pitchRad) * Math::Sin(yawRad),
            -Math::Sin(pitchRad),
            Math::Cos(pitchRad) * Math::Cos(yawRad)
        );
    }

    inline FVector3 GetRightVector(float yaw)
    {
        float yawRad = Math::Radians(yaw);
        return FVector3(Math::Cos(yawRad), 0.0f, -Math::Sin(yawRad));
    }

    inline FVector3 GetUpVector(float yaw, float pitch)
    {
        return Math::Cross(GetRightVector(yaw), GetForwardVector(yaw, pitch));
    }

    inline FVector3 GetCameraPosition(const FVector3& characterPos, float eyeHeight)
    {
        return characterPos + FVector3(0, eyeHeight, 0);
    }
}
