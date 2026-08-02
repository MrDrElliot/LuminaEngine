#pragma once
#include "Renderer/RHI.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    // A single paint/clear request against a render-target texture. Enqueued game-side, drained into
    // the frame snapshot during Extract, executed as a compute dispatch in TexturePaintPass.
    // Asset deletion mid-flight is safe: RHI::Textures::Release is frame-deferred, so the handle and
    // UAV slot outlive this op's frame.
    struct FTexturePaintOp
    {
        enum class EMode : uint8 { Paint, Clear };

        RHI::FTextureH  Target;                              // for clears
        uint32          TargetUAV = RHI::kInvalidHeapSlot;   // mip-0 storage slot, for paints
        FUIntVector2    TargetExtent = FUIntVector2(0);

        EMode           Mode        = EMode::Paint;

        FVector4       Color       = FVector4(1.0f);   // RGBA brush/clear color
        FVector2       CenterUV    = FVector2(0.5f);   // brush center, 0..1
        float           RadiusUV    = 0.05f;             // brush radius in UV units
        float           Strength    = 1.0f;              // 0..1 opacity at the brush center
        float           Hardness    = 1.0f;              // edge falloff exponent; >1 sharper
        int32           BrushIndex  = -1;                // bindless SRV of an optional brush mask; -1 = solid radial
    };
}
