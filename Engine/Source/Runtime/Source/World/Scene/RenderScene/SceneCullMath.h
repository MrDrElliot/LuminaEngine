#pragma once

#include "SceneRenderTypes.h"

#include <limits>

namespace Lumina::SceneCull
{
    // Conservative distance bounds of the active slots in [First, End), which the instance cull tests a whole block against.
    inline FInstanceBlockBounds ComputeInstanceBlockBounds(const FInstanceCullEntry* Entries, uint32 First, uint32 End, bool bAlwaysScan)
    {
        constexpr uint32 ActiveBit   = (uint32)EInstanceFlags::Active << 16;
        constexpr uint32 NoCullBit   = (uint32)EInstanceFlags::IgnoreOcclusionCulling << 16;
        constexpr float  Unreachable = -1.0f;

        FVector3 Lo(std::numeric_limits<float>::max());
        FVector3 Hi(-std::numeric_limits<float>::max());
        float Reach      = Unreachable;
        bool  bUnbounded = bAlwaysScan;
        for (uint32 Slot = First; Slot < End; ++Slot)
        {
            const FInstanceCullEntry& Entry = Entries[Slot];
            if ((Entry.DrawIDAndFlags & ActiveBit) == 0u)
            {
                continue;
            }

            // Matches the instance test, which skips the distance cut for these.
            bUnbounded = bUnbounded || (Entry.DrawIDAndFlags & NoCullBit) != 0u || Entry.MaxDrawDistance <= 0.0f;

            const FVector3 Center(Entry.SphereBounds);
            const FVector3 Radius(Entry.SphereBounds.w);
            Lo    = Math::Min(Lo, Center - Radius);
            Hi    = Math::Max(Hi, Center + Radius);
            Reach = Math::Max(Reach, Entry.MaxDrawDistance);
        }

        FInstanceBlockBounds Out;
        Out.bAlwaysScan = bUnbounded ? 1u : 0u;
        Out.Reach       = Reach;
        Out.Min         = Reach < 0.0f ? FVector3(0.0f) : Lo;
        Out.Max         = Reach < 0.0f ? FVector3(0.0f) : Hi;
        return Out;
    }

    // True when an active caster in [First, End) has a distance field, which is all the sun's distance-field cull looks at.
    inline bool BlockHasDistanceFieldCaster(const FInstanceCullEntry* Entries, uint32 First, uint32 End)
    {
        constexpr uint32 Required = ((uint32)EInstanceFlags::Active | (uint32)EInstanceFlags::CastShadow |
                                     (uint32)EInstanceFlags::HasDistanceField) << 16;
        for (uint32 Slot = First; Slot < End; ++Slot)
        {
            if ((Entries[Slot].DrawIDAndFlags & Required) == Required)
            {
                return true;
            }
        }
        return false;
    }

    // The local sphere carried through Transform, its radius grown by the largest axis scale.
    FORCEINLINE FVector4 TransformSphere(const FMatrix4& Transform, const FVector3& LocalCenter, float LocalRadius)
    {
        const FVector3 X(Transform[0]);
        const FVector3 Y(Transform[1]);
        const FVector3 Z(Transform[2]);
        const FVector3 Center = X * LocalCenter.x + Y * LocalCenter.y + Z * LocalCenter.z + FVector3(Transform[3]);
        const float ScaleSq = Math::Max(Math::Max(Math::Dot(X, X), Math::Dot(Y, Y)), Math::Dot(Z, Z));
        return FVector4(Center, LocalRadius * Math::Sqrt(ScaleSq));
    }
}
