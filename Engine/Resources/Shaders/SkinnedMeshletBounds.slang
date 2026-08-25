#include "Includes/SceneGlobals.slang"

// Per-frame object-space meshlet spheres for skinned meshes, so the cull can reject them per meshlet.

// Derived from bones rather than skinned vertices because this runs before SkinningPass.

// Skinning is a convex combination of per-bone transformed points, so enclosing those encloses the mesh.

// Object space, matching FMeshletHeader::Spheres, so the cull's model transform and MaxScale still apply.

#define LOCAL_SIZE_X 64

struct FSkinnedBoundsArgs
{
    uint NumSlots;
    // The x domain, sized by the longest per-slot meshlet range, so short slots idle their tail.
    uint MaxRange;
    uint BoundsCapacity;
    // Slots RetainedStatic actually holds, since a stale or short buffer yields a wild header slot.
    uint RetainedCapacity;

    uint*              SlotList;
    FSkinnedFrameData* SkinnedFrameData;
    FInstanceStatic*   RetainedStatic;
    FMeshletSphere*    OutBounds;
    // Same index space as OutBounds, so one base addresses both.
    FSkinnedMeshletCone* OutCones;
};
FSkinnedBoundsArgs* Pass() { return GetPassArgs<FSkinnedBoundsArgs>(); }

// 1.0 is the gate every cone reader already checks, so this is how a meshlet opts out of the test.
FSkinnedMeshletCone DisabledCone()
{
    FSkinnedMeshletCone Out;
    Out.Apex       = float3(0.0, 0.0, 0.0);
    Out.Cutoff     = 1.0;
    Out.Axis       = float3(0.0, 0.0, 1.0);
    Out.ApexSpread = 0.0;
    return Out;
}

[shader("compute")]
[numthreads(LOCAL_SIZE_X, 1, 1)]
void ComputeMain(uint3 GlobalID : SV_DispatchThreadID)
{
    const uint SlotListIdx = GlobalID.y;
    if (SlotListIdx >= Pass().NumSlots || Pass().OutBounds == nullptr)
    {
        return;
    }

    const uint SlotIdx = Pass().SlotList[SlotListIdx];
    if (SlotIdx >= Pass().RetainedCapacity || Pass().RetainedStatic == nullptr
        || Pass().SkinnedFrameData == nullptr)
    {
        return;
    }

    FSkinnedFrameData Skin = Pass().SkinnedFrameData[SlotIdx];

    if (Skin.SkinnedBoundsBase == kNoSkinnedBounds)
    {
        return;
    }

    const uint SurfaceEnd = Skin.SurfaceMeshletOffset + Skin.SurfaceMeshletCount;
    const uint ShadowEnd  = Skin.ShadowMeshletOffset + Skin.ShadowMeshletCount;

    // Skinned never re-selects a LOD per view, so the union of these two is every reachable meshlet.
    const uint RangeBegin = min(Skin.SurfaceMeshletOffset, Skin.ShadowMeshletOffset);
    const uint RangeEnd   = max(SurfaceEnd, ShadowEnd);

    const uint MeshletIdx = RangeBegin + GlobalID.x;
    if (MeshletIdx >= RangeEnd)
    {
        return;
    }

    // Reached, not copied, since a local copy would put a raw pointer in an undecorated OpVariable.
    FInstanceStatic* Static = Pass().RetainedStatic + SlotIdx;

    // Slot 0 is the null header, so this is readable for any slot value the slab can name.
    FMeshletHeader* Header = Static.MeshletHeader;
    if (MeshletHeaders() == nullptr || Bones() == nullptr)
    {
        return;
    }

    // Bounded by the header's own count, because only the header knows how long its arrays are.
    if (MeshletIdx >= Header.MeshletCount || Header.Spheres == nullptr)
    {
        return;
    }

    // SkinnedBoundsBase folds in RangeBegin, so this lands inside the slot's own slice.
    const uint OutIdx = Skin.SkinnedBoundsBase + MeshletIdx;
    if (OutIdx >= Pass().BoundsCapacity)
    {
        return;
    }

    const FMeshletSphere Bind = Header.Spheres[MeshletIdx];
    const FMeshletBonePalette Palette = GetBonePalette(Header, MeshletIdx);

    // When this gate fails the vertices stay in bind pose, so the bind sphere is exact rather than a guess.
    if (!CanSkinMeshlet(Palette, Skin.BoneOffset))
    {
        Pass().OutBounds[OutIdx] = Bind;

        // Bind pose is exact here, so the authored cone carries over with no widening.
        if (Pass().OutCones != nullptr)
        {
            FSkinnedMeshletCone Out = DisabledCone();
            if (Header.Cones != nullptr)
            {
                const FMeshletCone C = Header.Cones[MeshletIdx];
                Out.Apex   = C.Apex;
                Out.Cutoff = C.Cutoff;
                Out.Axis   = C.Axis;
            }
            Pass().OutCones[OutIdx] = Out;
        }
        return;
    }

    const float4 BindCenter = float4(Bind.Center, 1.0);

    const bool bWantCone = Pass().OutCones != nullptr;
    const bool bHasCone  = bWantCone && Header.Cones != nullptr;

    const FMeshletCone BindCone = bHasCone ? Header.Cones[MeshletIdx] : (FMeshletCone)0;
    const float4 BindApex       = float4(BindCone.Apex, 1.0);

    float3 Lo = float3( 3.4e38,  3.4e38,  3.4e38);
    float3 Hi = float3(-3.4e38, -3.4e38, -3.4e38);
    float  MaxScaledRadius = 0.0;

    float3 AxisSum  = float3(0.0, 0.0, 0.0);
    float3 ApexSum  = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < Palette.Count; ++i)
    {
        const FSkinRows Rows = UnpackBoneRows(Bones()[ResolvePaletteBone(Header, Palette, i, Skin.BoneOffset)]);

        const float3 Posed = float3(dot(Rows.R0, BindCenter), dot(Rows.R1, BindCenter), dot(Rows.R2, BindCenter));

        Lo = min(Lo, Posed);
        Hi = max(Hi, Posed);

        // Per bone, because a scaled bone scales the bind radius with it.
        const float SqScaleX = dot(Rows.R0.xyz, Rows.R0.xyz);
        const float SqScaleY = dot(Rows.R1.xyz, Rows.R1.xyz);
        const float SqScaleZ = dot(Rows.R2.xyz, Rows.R2.xyz);

        MaxScaledRadius = max(MaxScaledRadius, sqrt(max(SqScaleX, max(SqScaleY, SqScaleZ))) * Bind.Radius);

        if (bHasCone)
        {
            AxisSum += normalize(float3(dot(Rows.R0.xyz, BindCone.Axis),
                                        dot(Rows.R1.xyz, BindCone.Axis),
                                        dot(Rows.R2.xyz, BindCone.Axis)));

            ApexSum += float3(dot(Rows.R0, BindApex), dot(Rows.R1, BindApex), dot(Rows.R2, BindApex));
        }
    }

    const float3 Center = (Lo + Hi) * 0.5;

    FMeshletSphere Out;
    Out.Center = Center;

    // Half the diagonal covers every transformed center, and the scaled bind radius covers the vertices.
    Out.Radius = length(Hi - Center) + MaxScaledRadius;

    Pass().OutBounds[OutIdx] = Out;

    if (!bWantCone)
    {
        return;
    }

    // An authored cutoff of 1.0 already means no cone, so nothing here can make it reject.
    if (!bHasCone || BindCone.Cutoff >= 1.0 || dot(AxisSum, AxisSum) <= 1e-12)
    {
        Pass().OutCones[OutIdx] = DisabledCone();
        return;
    }

    const float3 MeanAxis = normalize(AxisSum);
    const float3 MeanApex = ApexSum / (float)Palette.Count;

    // Second pass over the palette, since the mean is only known once the first one finished.
    float CosSpread  = 1.0;
    float ApexSpread = 0.0;

    for (uint j = 0; j < Palette.Count; ++j)
    {
        const FSkinRows Rows = UnpackBoneRows(Bones()[ResolvePaletteBone(Header, Palette, j, Skin.BoneOffset)]);

        const float3 Axis = normalize(float3(dot(Rows.R0.xyz, BindCone.Axis),
                                             dot(Rows.R1.xyz, BindCone.Axis),
                                             dot(Rows.R2.xyz, BindCone.Axis)));

        const float3 Apex = float3(dot(Rows.R0, BindApex), dot(Rows.R1, BindApex), dot(Rows.R2, BindApex));

        CosSpread  = min(CosSpread, dot(MeanAxis, Axis));
        ApexSpread = max(ApexSpread, length(Apex - MeanApex));
    }

    const float Widened = WidenCutoff(BindCone.Cutoff, CosSpread);

    FSkinnedMeshletCone Cone = DisabledCone();

    // Past this the reject cone has been widened away to nothing, so the test is pure cost.
    if (Widened < 0.9999)
    {
        Cone.Apex       = MeanApex;
        Cone.Cutoff     = Widened;
        Cone.Axis       = MeanAxis;
        Cone.ApexSpread = ApexSpread;
    }

    Pass().OutCones[OutIdx] = Cone;
}
