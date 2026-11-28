#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/Entity/Components/VirtualTextureComponent.h"

namespace Lumina
{
    class CRuntimeVirtualTexture;

    // One brush swept from From to To across a virtual texture's world plane.
    struct FVirtualTextureStamp
    {
        TWeakObjectPtr<CRuntimeVirtualTexture> Texture;
        FVector2 From     = FVector2(0.0f);
        FVector2 To       = FVector2(0.0f);
        FVector4 Value    = FVector4(1.0f);
        float    Radius   = 0.5f;
        float    Hardness = 0.5f;
        float    Opacity  = 1.0f;
        EVirtualTextureBlend Blend = EVirtualTextureBlend::Max;
    };

    // Matches FVirtualTextureStampGPU in VirtualTextureWrite.slang.
    struct FVirtualTextureStampGPU
    {
        FVector2 From     = FVector2(0.0f);
        FVector2 To       = FVector2(0.0f);
        FVector4 Value    = FVector4(0.0f);
        float    Radius   = 0.0f;
        float    Hardness = 0.0f;
        float    Opacity  = 0.0f;
        uint32   Blend    = 0u;
    };
    static_assert(sizeof(FVirtualTextureStampGPU) == 48 && offsetof(FVirtualTextureStampGPU, Value) == 16, "FVirtualTextureStampGPU must match VirtualTextureWrite.slang.");

    namespace VirtualTextureMath
    {
        // The window's first texel along each axis, in world texels, snapped so the content never shifts under it.
        inline FIntVector2 WindowOrigin(const FVector2& CenterXZ, float TexelSize, int32 Resolution)
        {
            return FIntVector2((int32)Math::Floor(CenterXZ.x / TexelSize) - Resolution / 2, (int32)Math::Floor(CenterXZ.y / TexelSize) - Resolution / 2);
        }

        // The world texel rects, as min x, min y, width and height, that a window moving from Old to New uncovers. The corner a diagonal move uncovers is listed twice.
        inline void ExposedRects(const FIntVector2& Old, const FIntVector2& New, int32 Resolution, TVector<FIntVector4>& Out)
        {
            const FIntVector2 Delta = New - Old;
            if (Math::Abs(Delta.x) >= Resolution || Math::Abs(Delta.y) >= Resolution)
            {
                Out.Add(FIntVector4(New.x, New.y, Resolution, Resolution));
                return;
            }
            if (Delta.x != 0)
            {
                const int32 MinX = Delta.x > 0 ? Old.x + Resolution : New.x;
                Out.Add(FIntVector4(MinX, New.y, Math::Abs(Delta.x), Resolution));
            }
            if (Delta.y != 0)
            {
                const int32 MinY = Delta.y > 0 ? Old.y + Resolution : New.y;
                Out.Add(FIntVector4(New.x, MinY, Resolution, Math::Abs(Delta.y)));
            }
        }

        // Where a world texel lives in the wrapped image.
        inline int32 Wrap(int32 WorldTexel, int32 Resolution)
        {
            const int32 R = WorldTexel % Resolution;
            return R < 0 ? R + Resolution : R;
        }
    }
}
