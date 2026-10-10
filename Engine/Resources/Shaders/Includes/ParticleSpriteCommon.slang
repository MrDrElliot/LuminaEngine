#pragma once
#include "ParticleSimCommon.slang"
#include "Triplanar.slang"

// Must match EParticleFacingMode in ParticleSystem.h.
static const uint PARTICLE_FACING_CAMERA   = 0u;
static const uint PARTICLE_FACING_WORLD_XZ = 1u;
static const uint PARTICLE_FACING_VELOCITY = 2u;
static const uint PARTICLE_FACING_VERTICAL = 3u;
static const uint PARTICLE_FACING_SURFACE  = 4u;

// Must match EParticleRenderMode.
static const uint PARTICLE_RENDER_SPRITE = 0u;
static const uint PARTICLE_RENDER_MESH   = 1u;
static const uint PARTICLE_RENDER_RIBBON = 2u;

// Must match EParticleFlipbookMode.
static const uint PARTICLE_FLIPBOOK_OVER_LIFE = 0u;
static const uint PARTICLE_FLIPBOOK_LOOP      = 1u;

// Must match EParticleBlendMode in ParticleSystem.h.
static const uint PARTICLE_BLEND_ALPHA          = 0u;
static const uint PARTICLE_BLEND_ADDITIVE       = 1u;
static const uint PARTICLE_BLEND_PREMULTIPLIED  = 2u;
static const uint PARTICLE_BLEND_MULTIPLY       = 3u;

// Must match PARTICLE_RENDER_FLAG_LIT in SceneRenderTypes.h.
static const uint PARTICLE_RENDER_LIT = 1u << 8u;

// Mirrors FParticlePushConstants in DefaultSceneRenderer.cpp; every particle pipeline pushes this block.
struct FParticlePushConstants
{
    GPUSpan<FGPUParticle> Particles;
    float4 Tint;                   // rgb tint, w intensity multiplier
    uint   TextureIndex;           // bindless 2D SRV (0 = default sprite)
    uint   FacingMode;             // EParticleFacingMode
    float  VelocityStretch;        // extra length along velocity, in seconds of travel
    uint   SubUVColumns;           // sprite-sheet subdivisions; 1x1 = whole texture
    uint   SubUVRows;
    uint   AttrFloats;             // floats per particle in the attribute buffer
    GPUSpan<float> Attributes;     // declared-attribute buffer; empty when the emitter has none
    int    AttrSlotSizeScaleX;     // -1 when the stack did not declare it
    int    AttrSlotSizeScaleY;
    int    AttrSlotPrevPosX;       // written by the Trail module; -1 without one
    int    AttrSlotPrevPosY;
    int    AttrSlotPrevPosZ;
    uint   MaterialIndex;          // Materials() slot, read only by the Particle material domain
    uint   bSorted;                // 0 draws unsorted at full capacity, and SortedIndices is not read
    GPUSpan<uint> SortedIndices;   // live particles back-to-front
    float  SoftFadeDistance;       // meters of depth fade against the scene; 0 disables it
    uint   RenderFlags;            // EParticleBlendMode in the low byte, PARTICLE_RENDER_LIT above it
    GPUSpan<float4> RibbonHistory; // RibbonSegments past positions per particle
    uint   RenderMode;             // EParticleRenderMode
    uint   VertsPerParticle;
    uint   MeshletHeaderSlot;      // the Mesh render mode's mesh
    uint   bAlignMeshToVelocity;
    uint   RibbonSegments;
    uint   RibbonHead;             // newest history sample
    float  RibbonTailWidth;
    float  ExtrapolateTime;        // seconds a fixed-rate emitter is behind the frame
    uint   FlipbookMode;
    float  FlipbookFPS;
    uint   bRandomStartFrame;
    uint   bShadowPass;            // drawing into the sun cascade ShadowCascade instead of the camera
    int    ShadowDataIndex;
    uint   ShadowCascade;
    float  CameraFadeDistance;     // meters from the camera inside which a sprite fades away; 0 disables it
    float  Pad0;
};
FParticlePushConstants* Pass() { return GetPassArgs<FParticlePushConstants>(); }

// The draw covers VertsPerParticle vertices per particle, and a sorted draw only the live ones in order.
uint ParticleSlotForVertex(uint VertexIndex)
{
    const uint Ordinal = VertexIndex / max(Pass().VertsPerParticle, 1u);
    return (Pass().bSorted != 0u) ? Pass().SortedIndices.Get(Ordinal) : Ordinal;
}

bool IsParticleAlive(FGPUParticle P)
{
    return (P.Lifetime > 0.0) && (P.Age < P.Lifetime) && (P.Size > 0.0);
}

// Where the particle would be now, when a fixed-rate emitter last stepped ExtrapolateTime ago.
float3 ParticleDrawPosition(FGPUParticle P)
{
    return P.Position + P.Velocity * Pass().ExtrapolateTime;
}

// The frame sprites face, which is the camera's except in the shadow pass, where it is the sun's.
struct FParticleViewBasis
{
    float3 Right;
    float3 Up;
    float3 Forward;
    float3 Eye;
};

FParticleViewBasis ParticleViewBasis(float3 Around)
{
    FParticleViewBasis Basis;
    if (Pass().bShadowPass != 0u)
    {
        const float3 ToSun = normalize(GetSunDirection());
        const float3 Helper = abs(ToSun.y) > 0.99 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
        Basis.Forward = -ToSun;
        Basis.Right   = normalize(cross(Helper, Basis.Forward));
        Basis.Up      = cross(Basis.Forward, Basis.Right);
        Basis.Eye     = Around + ToSun * 1000.0;
        return Basis;
    }

    const float4x4 ViewMatrix = GetCameraView();
    Basis.Right   =  float3(ViewMatrix[0][0], ViewMatrix[0][1], ViewMatrix[0][2]);
    Basis.Up      = -float3(ViewMatrix[1][0], ViewMatrix[1][1], ViewMatrix[1][2]);
    Basis.Forward =  float3(ViewMatrix[2][0], ViewMatrix[2][1], ViewMatrix[2][2]);
    Basis.Eye     = GetCameraPosition();
    return Basis;
}

float4 ParticleToClip(float3 WorldPos)
{
    if (Pass().bShadowPass != 0u)
    {
        return mul(GetShadowViewProjection(Pass().ShadowDataIndex, Pass().ShadowCascade), float4(WorldPos, 1.0));
    }
    return WorldToClip(WorldPos);
}

float ParticleSeedRandom(FGPUParticle P)
{
    return frac(P.Seed * 0.61803398875 + 0.5);
}

float2 ParticleFlipbookUV(FGPUParticle P, float2 UV)
{
    const uint FrameCount = max(Pass().SubUVColumns * Pass().SubUVRows, 1u);
    if (FrameCount <= 1u)
    {
        return UV;
    }

    const uint StartFrame = Pass().bRandomStartFrame != 0u ? (uint)(ParticleSeedRandom(P) * (float)FrameCount) : 0u;
    uint Frame;
    if (Pass().FlipbookMode == PARTICLE_FLIPBOOK_LOOP)
    {
        Frame = (uint)(P.Age * Pass().FlipbookFPS);
    }
    else
    {
        // Clamped off the last frame, since at LifeRatio 1 the floor lands one cell past the sheet.
        Frame = min((uint)(saturate(P.Age / max(P.Lifetime, 1e-4)) * (float)FrameCount), FrameCount - 1u);
    }
    Frame = (Frame + StartFrame) % FrameCount;

    const float2 CellSize = float2(1.0 / (float)Pass().SubUVColumns, 1.0 / (float)Pass().SubUVRows);
    const float2 CellIdx  = float2((float)(Frame % Pass().SubUVColumns), (float)(Frame / Pass().SubUVColumns));
    return (CellIdx + UV) * CellSize;
}

uint ParticleBlendMode() { return Pass().RenderFlags & 0xFFu; }
bool IsParticleLit()     { return (Pass().RenderFlags & PARTICLE_RENDER_LIT) != 0u; }

// 1 in open air, falling to 0 where the sprite meets the opaque surface behind it or comes right up to the lens.
float ParticleSoftFade(float4 SvPosition)
{
    const float SpriteDepth = LinearizeSceneDepth(SvPosition.z);
    const float CameraFade  = Pass().CameraFadeDistance;
    float       Fade        = CameraFade > 0.0 ? smoothstep(CameraFade * 0.3, CameraFade, SpriteDepth) : 1.0;

    const float      FadeDistance = Pass().SoftFadeDistance;
    const FTexture2D SceneDepth = Scene().SceneDepth;
    if (FadeDistance <= 0.0 || !SceneDepth.IsValid())
    {
        return Fade;
    }

    const float2 UV       = SvPosition.xy / float2(GetScreenSize());
    const float  SceneRaw = SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
    return Fade * saturate((LinearizeSceneDepth(SceneRaw) - SpriteDepth) / FadeDistance);
}

// FParticleCompiler resolves Slot, which is how this non-generated shader reads a layout it cannot know.
float ReadParticleAttr(uint ParticleIndex, int Slot, float Fallback)
{
    if (Slot < 0)
    {
        return Fallback;
    }
    return Pass().Attributes.Get(ParticleIndex * Pass().AttrFloats + (uint)Slot);
}

static const float2 kParticleQuadPositions[6] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),

    float2(-1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0,  1.0)
};

static const float2 kParticleQuadUVs[6] =
{
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0),

    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(0.0, 1.0)
};

struct FParticleSprite
{
    bool   bAlive;
    uint   ParticleIndex;
    float3 WorldPosition;
    float2 UV;
    // Already turned by the particle's rotation, so a tangent frame reads what placed the vertex.
    float3 AxisRight;
    float3 AxisUp;
    float3 Normal;         // quad face, turned toward the camera
    float2 Extent;         // half-size along AxisRight / AxisUp
};

// A dead slot returns bAlive false with the frame left at defaults; callers emit a clip-culled degenerate.
FParticleSprite ExpandParticleSprite(uint VertexIndex, out FGPUParticle OutParticle)
{
    const uint QuadIndex = VertexIndex % 6;

    const uint ParticleIndex = ParticleSlotForVertex(VertexIndex);

    FGPUParticle P = Pass().Particles.Get(ParticleIndex);
    P.Position = ParticleDrawPosition(P);
    OutParticle = P;

    FParticleSprite Sprite;
    Sprite.ParticleIndex = ParticleIndex;
    Sprite.bAlive        = IsParticleAlive(P);
    Sprite.WorldPosition = P.Position;
    Sprite.UV            = kParticleQuadUVs[QuadIndex];
    Sprite.AxisRight     = float3(1.0, 0.0, 0.0);
    Sprite.AxisUp        = float3(0.0, 1.0, 0.0);
    Sprite.Normal        = float3(0.0, 0.0, 1.0);
    Sprite.Extent        = float2(0.0, 0.0);

    if (!Sprite.bAlive)
    {
        return Sprite;
    }

    const FParticleViewBasis View = ParticleViewBasis(P.Position);
    const float3 CameraRight   = View.Right;
    const float3 CameraUp      = View.Up;
    const float3 CameraForward = View.Forward;

    float3 BillboardRight;
    float3 BillboardUp;
    // Trail moves the quad to the midpoint of the segment it spans, so the center is a variable.
    float3 QuadCenter = P.Position;
    // A directional quad ignores rotation, which would swing it off the axis the mode exists to align to.
    bool bDirectional = false;
    float2 Extent = float2(P.Size, P.Size);
    Extent.x *= ReadParticleAttr(ParticleIndex, Pass().AttrSlotSizeScaleX, 1.0);
    Extent.y *= ReadParticleAttr(ParticleIndex, Pass().AttrSlotSizeScaleY, 1.0);

    // A declared PrevPos attribute means a Trail module, a different primitive that beats FacingMode.
    if (Pass().AttrSlotPrevPosX >= 0)
    {
        const float3 Prev = float3(ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosX, P.Position.x),
                                   ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosY, P.Position.y),
                                   ReadParticleAttr(ParticleIndex, Pass().AttrSlotPrevPosZ, P.Position.z));

        const float3 Delta = P.Position - Prev;
        const float  Len   = length(Delta);
        if (Len > 1e-5)
        {
            const float3 Dir = Delta / Len;
            float3 Side = cross(Dir, CameraForward);
            const float SideLen = length(Side);
            Side = (SideLen > 1e-5) ? (Side / SideLen) : CameraRight;

            BillboardUp    = Dir;
            BillboardRight = Side;
            // The sprite's own extent stays on top, so the streak has a cap at each end.
            QuadCenter   = Prev + Dir * (Len * 0.5);
            Extent.y    += Len * 0.5;
            bDirectional = true;
        }
        else
        {
            BillboardRight = CameraRight;
            BillboardUp    = CameraUp;
        }
    }
    else if (Pass().FacingMode == PARTICLE_FACING_VELOCITY)
    {
        const float Speed = length(P.Velocity);
        if (Speed > 1e-5)
        {
            const float3 Dir = P.Velocity / Speed;
            float3 Side = cross(Dir, CameraForward);
            const float SideLen = length(Side);
            Side = (SideLen > 1e-5) ? (Side / SideLen) : CameraRight;

            BillboardUp    = Dir;
            BillboardRight = Side;
            // Stretch measured in seconds of travel, so a fixed value reads consistently across speeds.
            Extent.y += Speed * Pass().VelocityStretch;
            bDirectional = true;
        }
        else
        {
            BillboardRight = CameraRight;
            BillboardUp    = CameraUp;
        }
    }
    else if (Pass().FacingMode == PARTICLE_FACING_WORLD_XZ)
    {
        BillboardRight = float3(1.0, 0.0, 0.0);
        BillboardUp    = float3(0.0, 0.0, 1.0);
    }
    else if (Pass().FacingMode == PARTICLE_FACING_SURFACE)
    {
        // Lies across its velocity, which a sub-emitter spawned along a collision normal points out of the surface hit.
        const float Speed = length(P.Velocity);
        const float3 Normal = Speed > 1e-8 ? P.Velocity / Speed : float3(0.0, 1.0, 0.0);
        const float3 Across = abs(Normal.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        BillboardRight = normalize(cross(Across, Normal));
        BillboardUp    = cross(Normal, BillboardRight);
    }
    else if (Pass().FacingMode == PARTICLE_FACING_VERTICAL)
    {
        const float3 Flat = float3(View.Eye.x - P.Position.x, 0.0, View.Eye.z - P.Position.z);
        BillboardUp    = float3(0.0, 1.0, 0.0);
        BillboardRight = dot(Flat, Flat) > 1e-8 ? normalize(cross(BillboardUp, Flat)) : CameraRight;
    }
    else
    {
        BillboardRight = CameraRight;
        BillboardUp    = CameraUp;
    }

    // Wholly inside the camera fade the sprite would shade every pixel only to discard it, so it is dropped before rasterizing.
    const float CameraFade = Pass().CameraFadeDistance;
    if (CameraFade > 0.0 && Pass().bShadowPass == 0u)
    {
        const float FarthestDepth = WorldToClip(QuadCenter).w + max(Extent.x, Extent.y);
        if (FarthestDepth < CameraFade * 0.3)
        {
            Sprite.bAlive = false;
            return Sprite;
        }
    }

    // Turning the axes rather than the corner offset gives the same vertex and leaves a usable frame.
    const float Angle = bDirectional ? 0.0 : P.Rotation;
    const float s     = sin(Angle);
    const float c     = cos(Angle);
    const float3 AxisRight = BillboardRight * c + BillboardUp * s;
    const float3 AxisUp    = BillboardUp * c - BillboardRight * s;

    const float2 Local = kParticleQuadPositions[QuadIndex] * Extent;
    const float3 WorldPos = QuadCenter + AxisRight * Local.x + AxisUp * Local.y;

    float3 Normal = normalize(cross(AxisRight, AxisUp));
    if (dot(Normal, View.Eye - WorldPos) < 0.0)
    {
        Normal = -Normal;
    }

    const float2 UV = ParticleFlipbookUV(P, kParticleQuadUVs[QuadIndex]);

    Sprite.WorldPosition = WorldPos;
    Sprite.UV            = UV;
    Sprite.AxisRight     = AxisRight;
    Sprite.AxisUp        = AxisUp;
    Sprite.Normal        = Normal;
    Sprite.Extent        = Extent;
    return Sprite;
}

// Interpolants the Particle material domain carries; the built-in shaders use a leaner set of their own.
struct FParticleMaterialVertexOutput
{
    float4 Color         : PARTICLE_COLOR;
    float2 UV            : TEXCOORD0;
    float3 WorldPos      : PARTICLE_WORLDPOS;
    // The corner before the graph displaced it; the sprite expansion itself is not a material offset.
    float3 WorldPosNoWPO : PARTICLE_WORLDPOS_NO_WPO;
    float3 ViewPos       : PARTICLE_VIEWPOS;
    float3 Normal        : PARTICLE_NORMAL;
    float4 Tangent       : PARTICLE_TANGENT;
    float3 Velocity      : PARTICLE_VELOCITY;
    float3 Center        : PARTICLE_CENTER;   // the particle itself, not the expanded corner
    float4 SizeLifeRand  : PARTICLE_LIFE;     // xy extent, z relative time, w per-particle random
    float4 Position      : SV_Position;
};

// Normalized life, the value a Particle Relative Time node reads.
float ParticleRelativeTime(FGPUParticle P)
{
    return saturate(P.Age / max(P.Lifetime, 1e-4));
}

// Stable per-particle 0..1 from the spawn seed, so a graph can vary one sprite against its neighbors.
float ParticleRandom(FGPUParticle P)
{
    return ParticleSeedRandom(P);
}
