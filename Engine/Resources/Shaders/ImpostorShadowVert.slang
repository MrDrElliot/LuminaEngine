#include "Includes/ImpostorShadow.slang"

// One sun-facing quad per listed point and level, framing the sphere its impostor was baked around.

[shader("vertex")]
FImpostorShadowVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID, uint InstanceIndex : SV_VulkanInstanceID)
{
    FImpostorShadowVertexOutput Out;
    Out.Entry    = 0u;
    Out.Level    = 0u;
    Out.WorldPos = float3(0.0, 0.0, 0.0);
    Out.Position = float4(0.0, 0.0, 0.0, 0.0);

    if (!Pass().List.Contains(InstanceIndex))
    {
        return Out;
    }
    const uint Word  = Pass().List.Get(InstanceIndex);
    const uint Entry = PROXY_ENTRY_POINT_BIT | (Word & ((1u << kImpostorShadowLevelShift) - 1u));
    const FVisProxy P = LoadProxyEntry(Entry);
    if (!P.bValid)
    {
        return Out;
    }
    FMeshletHeader* Header = MeshletHeaderAt(P.Static.MeshletHeaderSlot);
    if (!Header.bHasImpostor)
    {
        return Out;
    }

    const float4x4 Model    = ImpostorShadowModel(P);
    const float3   Center   = mul(Model, float4(Header.ImpostorCenter, 1.0)).xyz;
    const float    HalfSize = Header.ImpostorRadius * MatrixMaxScale(Model) * kImpostorQuadGrowth;

    const float3 ToSun   = normalize(GetSunDirection());
    const float3 Helper  = abs(ToSun.y) > 0.99 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 Right   = normalize(cross(Helper, -ToSun));
    const float3 Up      = cross(-ToSun, Right);

    static const float2 Corners[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    const float2 C = Corners[min(VertexIndex, 3u)];

    Out.Entry    = Entry;
    Out.Level    = Word >> kImpostorShadowLevelShift;
    Out.WorldPos = Center + (Right * C.x + Up * C.y) * HalfSize;
    Out.Position = mul(ImpostorShadowViewProjection(Out.Level), float4(Out.WorldPos, 1.0));
    return Out;
}
