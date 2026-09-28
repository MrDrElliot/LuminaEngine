#include "Includes/ParticleLighting.slang"

// Draws the emitter's mesh at every particle, one vertex per corner of every finest-LOD meshlet triangle.

float3 RotateAboutAxis(float3 V, float3 Axis, float Angle)
{
    const float S = sin(Angle);
    const float C = cos(Angle);
    return V * C + cross(Axis, V) * S + Axis * dot(Axis, V) * (1.0 - C);
}

// Turns the mesh's up axis onto Dir, for casings and shards that should point where they travel.
float3 AlignUpTo(float3 V, float3 Dir)
{
    const float CosAngle = dot(float3(0.0, 1.0, 0.0), Dir);
    if (CosAngle > 0.9999)
    {
        return V;
    }
    if (CosAngle < -0.9999)
    {
        return float3(V.x, -V.y, -V.z);
    }
    return RotateAboutAxis(V, normalize(cross(float3(0.0, 1.0, 0.0), Dir)), acos(CosAngle));
}

[shader("vertex")]
FParticleVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    FGPUParticle P = SpanGet(&Pass().Particles, ParticleSlotForVertex(VertexIndex));
    const uint HeaderSlot = Pass().MeshletHeaderSlot;
    if (!IsParticleAlive(P) || HeaderSlot == 0u || HeaderSlot >= MeshletHeaders().Count)
    {
        return CulledParticleVertex();
    }
    P.Position = ParticleDrawPosition(P);

    FMeshletHeader* Header = SpanAt(MeshletHeaders(), HeaderSlot);
    const uint VertsPerMeshlet = MESHLET_MAX_TRIANGLES * 3u;
    const uint Local           = VertexIndex % max(Pass().VertsPerParticle, 1u);
    const uint MeshletIndex    = Local / VertsPerMeshlet;
    const uint Triangle        = (Local % VertsPerMeshlet) / 3u;
    const uint Corner          = Local % 3u;
    if (MeshletIndex >= Header.MeshletCount)
    {
        return CulledParticleVertex();
    }

    const FMeshlet M = Header.Meshlets[MeshletIndex];
    if (M.LODIndex != 0u || Triangle >= M.TriangleCount)
    {
        return CulledParticleVertex();
    }

    const uint VertexLocal = (Header.Triangles[M.TriangleOffset + Triangle] >> (Corner * 8u)) & 0xFFu;
    if (VertexLocal >= M.VertexCount)
    {
        return CulledParticleVertex();
    }

    const FVertexData Vertex = LoadStaticVertex(Header, M, VertexLocal);
    float3 Offset = Vertex.Position * P.Size;
    float3 Normal = Vertex.Normal;

    const float Speed = length(P.Velocity);
    if (Pass().bAlignMeshToVelocity != 0u && Speed > 1e-4)
    {
        Offset = AlignUpTo(Offset, P.Velocity / Speed);
        Normal = AlignUpTo(Normal, P.Velocity / Speed);
    }
    else
    {
        // A per-particle axis, so a pile of debris tumbles in different directions from one rotation value.
        uint Seed = asuint(P.Seed) ^ 0x9E3779B9u;
        const float3 Axis = RandOnUnitSphere(Seed);
        Offset = RotateAboutAxis(Offset, Axis, P.Rotation);
        Normal = RotateAboutAxis(Normal, Axis, P.Rotation);
    }

    FParticleVertexOutput Out;
    Out.UV           = Vertex.UV;
    Out.Color        = P.Color * Vertex.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = ParticleToClip(P.Position + Offset);
    Out.Normal       = Normal;
    LightParticleVertex(Out, P.Position, P.Size);
    return Out;
}
