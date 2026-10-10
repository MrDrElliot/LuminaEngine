#include "Includes/ParticleLighting.slang"

// Draws each particle as a strip from where it is back through RibbonSegments of recorded positions.

// Which end of the segment and which side of the strip each of a quad's six vertices takes.
static const uint2 kRibbonCorners[6] =
{
    uint2(0u, 0u), uint2(1u, 0u), uint2(1u, 1u),
    uint2(0u, 0u), uint2(1u, 1u), uint2(0u, 1u)
};

// Point zero is the particle itself, and point Segments its oldest recorded position.
float3 RibbonPoint(uint Slot, float3 Head, uint Point)
{
    if (Point == 0u)
    {
        return Head;
    }

    const uint Segments = Pass().RibbonSegments;
    const uint Sample   = (Pass().RibbonHead + Segments - ((Point - 1u) % Segments)) % Segments;
    return Pass().RibbonHistory.Get(Slot * Segments + Sample).xyz;
}

[shader("vertex")]
FParticleVertexOutput VertexMain(uint VertexIndex : SV_VulkanVertexID)
{
    const uint Segments = Pass().RibbonSegments;
    const uint Slot     = ParticleSlotForVertex(VertexIndex);
    FGPUParticle P      = Pass().Particles.Get(Slot);
    if (Segments == 0u || !IsParticleAlive(P))
    {
        return CulledParticleVertex();
    }

    const float3 Head    = ParticleDrawPosition(P);
    const uint   Local   = VertexIndex % (Segments * 6u);
    const uint2  Corner  = kRibbonCorners[Local % 6u];
    const uint   Point   = Local / 6u + Corner.x;

    const float3 Position = RibbonPoint(Slot, Head, Point);
    const float3 Newer    = RibbonPoint(Slot, Head, Point == 0u ? 0u : Point - 1u);
    const float3 Older    = RibbonPoint(Slot, Head, min(Point + 1u, Segments));

    float3 Tangent = Newer - Older;
    if (dot(Tangent, Tangent) < 1e-10)
    {
        Tangent = dot(P.Velocity, P.Velocity) > 1e-10 ? P.Velocity : float3(0.0, 1.0, 0.0);
    }
    Tangent = normalize(Tangent);

    const FParticleViewBasis View = ParticleViewBasis(Position);
    const float3 ToEye = View.Eye - Position;
    float3 Side = cross(Tangent, ToEye);
    Side = dot(Side, Side) > 1e-10 ? normalize(Side) : View.Right;

    const float Along = (float)Point / (float)Segments;
    const float Width = P.Size * lerp(1.0, Pass().RibbonTailWidth, Along);
    const float SideSign = Corner.y != 0u ? 1.0 : -1.0;

    FParticleVertexOutput Out;
    Out.UV           = float2(Along, (float)Corner.y);
    Out.Color        = P.Color * float4(Pass().Tint.rgb * Pass().Tint.w, 1.0 - Along);
    Out.TextureIndex = Pass().TextureIndex;
    Out.Position     = ParticleToClip(Position + Side * (Width * 0.5 * SideSign));
    Out.Normal       = normalize(ToEye);
    LightParticleVertex(Out, P.Position, P.Size);
    return Out;
}
