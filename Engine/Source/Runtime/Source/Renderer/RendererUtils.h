#pragma once

#include "RHITexture.h"
#include "Containers/Span.h"
#include "Containers/FunctionRef.h"
#include "Platform/GenericPlatform.h"
#include "Shared/SharedConstants.h"

namespace Lumina::RenderUtils
{
    // Decodes raw image bytes straight into the global texture heap; ImTextureID = ResourceID().
    RUNTIME_API RHI::FManagedTexture CreateImageFromPixels(TSpan<const uint8> PixelData, bool bFlipVertically = true, FUIntVector2 Size = {}, const char* DebugName = nullptr);

    inline uint32 CalculateMipCount(uint32 Width, uint32 Height)
    {
        uint32 Levels = 1;
        while (Width > 1 || Height > 1)
        {
            Width = Math::Max(Width >> 1, 1u);
            Height = Math::Max(Height >> 1, 1u);
            ++Levels;
        }
        return Levels;
    }

    constexpr uint32 GetGroupCount(uint32 ThreadCount, uint32 LocalSize)
    {
        return (ThreadCount + LocalSize - 1) / LocalSize;
    }

    // The shader must undo this with the same MAX_DISPATCH_AXIS, or Y rows repeat and the tail is lost.
    inline FUIntVector2 FoldGroupCount(uint32 GroupCount)
    {
        return FUIntVector2(Math::Min(GroupCount, (uint32)MAX_DISPATCH_AXIS),
                            GetGroupCount(GroupCount, (uint32)MAX_DISPATCH_AXIS));
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
}
