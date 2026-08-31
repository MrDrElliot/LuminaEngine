#pragma once
#include "ParticleSimCommon.slang"

// Billboard expansion shared by the built-in particle shaders and the Particle material template.

// Must match EParticleFacingMode in ParticleSystem.h.
static const uint PARTICLE_FACING_CAMERA   = 0u;
static const uint PARTICLE_FACING_WORLD_XZ = 1u;
static const uint PARTICLE_FACING_VELOCITY = 2u;

// Mirrors FParticlePushConstants in DefaultSceneRenderer.cpp; every particle pipeline pushes this block.
struct FParticlePushConstants
{
    FGPUParticle* Particles;       // device address of the simulated particle array
    uint   TextureIndex;           // bindless 2D SRV (0 = default sprite)
    uint   FacingMode;             // EParticleFacingMode
    float4 Tint;                   // xyz tint rgb, w intensity multiplier
    float  VelocityStretch;        // extra length along velocity, in seconds of travel
    uint   SubUVColumns;           // sprite-sheet subdivisions; 1x1 = whole texture
    uint   SubUVRows;
    uint   AttrFloats;             // floats per particle in the attribute buffer
    float* Attributes;             // declared-attribute buffer, null when the emitter has none
    int    AttrSlotSizeScaleX;     // -1 when the stack did not declare it
    int    AttrSlotSizeScaleY;
    int    AttrSlotPrevPosX;       // written by the Trail module; -1 without one
    int    AttrSlotPrevPosY;
    int    AttrSlotPrevPosZ;
    uint   MaterialIndex;          // Materials() slot, read only by the Particle material domain
    uint*  SortedIndices;          // live particles back-to-front, null when the sort pass did not run
};
FParticlePushConstants* Pass() { return GetPassArgs<FParticlePushConstants>(); }

// FParticleCompiler resolves Slot, which is how this non-generated shader reads a layout it cannot know.
float ReadParticleAttr(uint ParticleIndex, int Slot, float Fallback)
{
    if (Slot < 0)
    {
        return Fallback;
    }
    return Pass().Attributes[ParticleIndex * Pass().AttrFloats + (uint)Slot];
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

    // The sorted draw covers only live particles, so the slot has to be looked up, not derived.
    const uint ParticleIndex = (Pass().SortedIndices != nullptr)
        ? Pass().SortedIndices[VertexIndex / 6]
        : (VertexIndex / 6);

    FGPUParticle P = Pass().Particles[ParticleIndex];
    OutParticle = P;

    FParticleSprite Sprite;
    Sprite.ParticleIndex = ParticleIndex;
    Sprite.bAlive        = (P.Lifetime > 0.0) && (P.Age < P.Lifetime) && (P.Size > 0.0);
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

    // Fetched once and handed back to the caller's clip transform, which avoids a second matrix load.
    const float4x4 ViewMatrix = GetCameraView();

    const float3 CameraRight =  float3(ViewMatrix[0][0], ViewMatrix[0][1], ViewMatrix[0][2]);
    const float3 CameraUp    = -float3(ViewMatrix[1][0], ViewMatrix[1][1], ViewMatrix[1][2]);

    float3 BillboardRight;
    float3 BillboardUp;
    // Trail moves the quad to the midpoint of the segment it spans, so the center is a variable.
    float3 QuadCenter = P.Position;
    // A directional quad ignores rotation, which would swing it off the axis the mode exists to align to.
    bool bDirectional = false;
    // Multipliers on the simulated size, so an emitter declaring neither behaves exactly as before.
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
        // A particle that has not moved has no segment to span, so it draws as a plain sprite this frame.
        if (Len > 1e-5)
        {
            const float3 Dir = Delta / Len;
            // Row 2 of the view matrix, matching CameraRight/CameraUp above.
            const float3 CameraForward = float3(ViewMatrix[2][0], ViewMatrix[2][1], ViewMatrix[2][2]);
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
        // A particle with no velocity has no direction to align to, so it stays camera-facing.
        if (Speed > 1e-5)
        {
            const float3 Dir = P.Velocity / Speed;
            const float3 CameraForward = float3(ViewMatrix[2][0], ViewMatrix[2][1], ViewMatrix[2][2]);
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
    else
    {
        BillboardRight = CameraRight;
        BillboardUp    = CameraUp;
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
    if (dot(Normal, GetCameraPosition() - WorldPos) < 0.0)
    {
        Normal = -Normal;
    }

    // The sub-UV frame advances with age, so a flipbook needs no per-particle storage.
    float2 UV = kParticleQuadUVs[QuadIndex];
    const uint FrameCount = max(Pass().SubUVColumns * Pass().SubUVRows, 1u);
    if (FrameCount > 1u)
    {
        const float LifeRatio = saturate(P.Age / max(P.Lifetime, 1e-4));
        // Clamped off the last frame, since at LifeRatio 1 the floor lands one cell past the sheet.
        const uint  Frame     = min((uint)(LifeRatio * (float)FrameCount), FrameCount - 1u);
        const float2 CellSize = float2(1.0 / (float)Pass().SubUVColumns, 1.0 / (float)Pass().SubUVRows);
        const float2 CellIdx  = float2((float)(Frame % Pass().SubUVColumns), (float)(Frame / Pass().SubUVColumns));
        UV = (CellIdx + UV) * CellSize;
    }

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
    return frac(P.Seed * 0.61803398875 + 0.5);
}
