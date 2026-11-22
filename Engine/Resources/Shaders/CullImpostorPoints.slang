#include "Includes/AppendBuffer.slang"
#include "Includes/Culling.slang"
#include "Includes/Impostor.slang"
#include "Includes/VirtualShadow.slang"

// The far plants held as bare points, listed for the camera's proxy pass or, with IMPOSTOR_SHADOW_CULL, for the static sun shadow pages that draw this frame.

struct FCullImpostorPointsArgs
{
    GPUSpan<FImpostorPoint>       Points;
    GPUSpan<FImpostorType>        Types;
    // One per IMPOSTOR_POINT_GROUP_SIZE points, the span each workgroup walks.
    GPUSpan<FInstanceBlockBounds> Blocks;
    GPUSpan<uint>                 OutList;
    // The word counting OutList, which may run past its capacity.
    GPUSpan<uint>                 OutCount;
    uint                          NumViews;
    uint                          FirstLevel;
    uint                          LastLevel;
    uint                          _Pad0;
};
FCullImpostorPointsArgs* Pass() { return GetPassArgs<FCullImpostorPointsArgs>(); }

// Bits of a shadow list word below its level.
static const uint kShadowLevelShift = 27u;

struct FPointSphere
{
    float3 Center;
    float  Radius;
    bool   bVisible;
};

// Where the point's impostor stands after distance culling and thinning, with the same widening the proxy pass draws it at.
FPointSphere PlacePoint(FImpostorPoint Point, FImpostorType Type)
{
    FPointSphere Out;
    Out.bVisible = false;
    const float4x4 Model = ImpostorPointModel(Point);
    Out.Center = mul(Model, float4(Type.Proxy.LocalCenter, 1.0)).xyz;
    Out.Radius = Type.Proxy.SplatHalfSize * Point.Scale * kImpostorQuadGrowth;

    const float3 ToCam  = Out.Center - GetCullCameraPosition();
    const float  DistSq = dot(ToCam, ToCam);
    if (Type.MaxDrawDistance > 0.0)
    {
        const float CutOff = Type.MaxDrawDistance + Out.Radius;
        if (DistSq > CutOff * CutOff)
        {
            return Out;
        }
    }

    // Thinned on the pivot hash exactly as a retained slot is, so the forest keeps the same survivors near and far.
    const float Keep = ThinningKeep(DistSq, Type.Proxy.ThinningStartDistance);
    if (Keep < 1.0)
    {
        const float3 Scale = ThinningScale(Keep, Point.Position);
        if (Scale.y <= 0.0)
        {
            return Out;
        }
        Out.Center = Point.Position + (Out.Center - Point.Position) * Scale;
        Out.Radius *= Scale.x;
    }
    Out.bVisible = true;
    return Out;
}

bool IsBlockInReach(FInstanceBlockBounds Block, float3 CamPos)
{
    if (Block.Reach < 0.0)
    {
        return false;
    }
    const float3 ToBox = clamp(CamPos, Block.Min, Block.Max) - CamPos;
    return dot(ToBox, ToBox) <= Block.Reach * Block.Reach;
}

[shader("compute")]
[numthreads(IMPOSTOR_POINT_GROUP_SIZE, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID, uint3 GroupID : SV_GroupID)
{
    // Undoes the Y fold the dispatch applies past MAX_DISPATCH_AXIS groups.
    const uint BlockIdx = GroupID.x + GroupID.y * MAX_DISPATCH_AXIS;
    const uint Index    = GlobalID.x + GroupID.y * MAX_DISPATCH_AXIS * IMPOSTOR_POINT_GROUP_SIZE;
    if (!Pass().Points.Contains(Index) || !Pass().Blocks.Contains(BlockIdx))
    {
        return;
    }

    const FInstanceBlockBounds Block = Pass().Blocks.GetAligned(BlockIdx);
    if (!IsBlockInReach(Block, GetCullCameraPosition()))
    {
        return;
    }

    const FImpostorPoint Point = Pass().Points.GetAligned(Index);
    if (!Pass().Types.Contains(Point.Type))
    {
        return;
    }
    const FImpostorType Type = Pass().Types.GetAligned(Point.Type);
    if ((Type.Flags & IMPOSTOR_TYPE_FLAG_ACTIVE) == 0u)
    {
        return;
    }

#ifdef IMPOSTOR_SHADOW_CULL
    if ((Type.Flags & IMPOSTOR_TYPE_FLAG_CAST_SHADOW) == 0u)
    {
        return;
    }
    const FPointSphere Sphere = PlacePoint(Point, Type);
    if (!Sphere.bVisible)
    {
        return;
    }

    // A point is drawn only where its static page is fresh or was cleared, so a settled forest lists almost nothing.
    const float3 BlockCenter = (Block.Min + Block.Max) * 0.5;
    const float  BlockRadius = length(Block.Max - BlockCenter);
    for (uint Level = Pass().FirstLevel; Level <= Pass().LastLevel; ++Level)
    {
        if (IsVirtualShadowPageCulled(Level, VSM_MASK_STATIC, BlockCenter, BlockRadius)
            || IsVirtualShadowPageCulled(Level, VSM_MASK_STATIC, Sphere.Center, Sphere.Radius))
        {
            continue;
        }
        const uint OutSlot = AppendReserve(Pass().OutCount.At(0u), Pass().OutList.Count, 0u);
        if (OutSlot != kAppendFull)
        {
            Pass().OutList.Set(OutSlot, (Level << kShadowLevelShift) | Index);
        }
    }
#else
    const uint NumViews = min(Pass().NumViews, (uint)MAX_CULL_VIEWS);
    if (NumViews == 0u)
    {
        return;
    }
    const FCullView Camera = CullViews().GetAligned(0u);
    if ((Camera.LayerMask & RENDER_LAYER_DEFAULT_MASK) == 0u || Camera.HasFlag(CULL_VIEW_FLAG_CAST_SHADOW_ONLY))
    {
        return;
    }

    const FPointSphere Sphere = PlacePoint(Point, Type);
    if (!Sphere.bVisible || !FrustumContainsSphere(Camera.FrustumPlanes, Sphere.Center, Sphere.Radius))
    {
        return;
    }

    const uint OutSlot = AppendReserve(Pass().OutCount.At(0u), Pass().OutList.Count, 0u);
    if (OutSlot != kAppendFull)
    {
        Pass().OutList.Set(OutSlot, PROXY_ENTRY_POINT_BIT | Index);
    }
#endif
}
