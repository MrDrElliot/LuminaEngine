#pragma once
#include "SceneGlobals.slang"

// Shared data layout + RNG/noise/emitter helpers for the two particle simulation shaders
// (ParticleSimulate.slang, the data-driven default, and Particles/ParticleSimulateTemplate.slang,
// the FParticleCompiler module-stack skeleton). Both consume the SAME CPU-side blocks, so the
// structs here must stay byte-identical to CParticleSystem / FParticleSimArgs in
// ForwardRenderScene.cpp -- keeping them in one place is what prevents the two sims drifting.

// Emitter shape enums (must match EParticleEmitterShape)
static const uint EMITTER_SHAPE_POINT  = 0u;
static const uint EMITTER_SHAPE_SPHERE = 1u;
static const uint EMITTER_SHAPE_BOX    = 2u;
static const uint EMITTER_SHAPE_CONE   = 3u;
static const uint EMITTER_SHAPE_RING   = 4u;
static const uint EMITTER_SHAPE_DISK   = 5u;

// Velocity mode enums (must match EParticleVelocityMode)
static const uint VELOCITY_MODE_EXPLICIT = 0u;
static const uint VELOCITY_MODE_RADIAL   = 1u;

// Sim flags (bitmask in Counts.w)
static const uint SIM_FLAG_LOOP          = (1u << 0);
static const uint SIM_FLAG_BURST_PENDING = (1u << 1);

// 64-byte particle. Do not change stride without updating ForwardRenderScene.
struct FGPUParticle
{
    float3  Position;
    float   Age;

    float3  Velocity;
    float   Lifetime;

    float4  Color;

    float   Size;
    float   Rotation;
    float   RotationSpeed;
    float   Seed;
};

// 256-byte uniform block. Kept vec4-aligned for safe std140 layout.
struct FParticleSimParams
{
    float4  EmitterPosition;     // xyz = world position
    float4  EmitterForward;      // xyz = forward (normalized), w = EmitterVelocity.x
    float4  EmitterRight;        // xyz = right   (normalized), w = EmitterVelocity.y
    float4  EmitterUp;           // xyz = up      (normalized), w = EmitterVelocity.z

    uint4   Counts;              // x=MaxParticles, y=SpawnCount, z=FrameSeed, w=SimFlags
    uint4   Modes;               // x=Shape, y=VelocityMode, z=reserved, w=reserved

    float4  ShapeSize;           // xyz dims; w=cone half-angle (radians)

    float4  VelocityMin;         // xyz
    float4  VelocityMax;         // xyz

    float4  SpeedAndLifetime;    // x=speedMin, y=speedMax, z=lifeMin, w=lifeMax
    float4  Gravity;             // xyz=gravity, w=drag

    float4  StartColor;
    float4  EndColor;

    float4  SizeRange;           // xy=start(min,max); zw=end(min,max)
    float4  RotationRange;       // xy=rot(min,max); zw=rotSpeed(min,max)

    float4  NoiseStrength;       // xyz=strength; w=scale
    float4  NoiseParams;         // x=speed, y=InheritEmitterVelocity; zw reserved

    float4  Timing;              // x=DeltaTime, y=TotalTime, z=SystemAge, w=unused
};

// Everything by device address; matches FParticleSimArgs in ForwardRenderScene.cpp.
struct FParticleSimArgs
{
    FParticleSimParams* Params;
    FGPUParticle*       Particles;
    uint*               SpawnCounter;   // scratch counter, sized 4 bytes
    // Module-stack value inputs, one float4 slot per parameter, in the order FParticleCompiler assigned
    // them. Null when the stack declared no parameters, in which case nothing reads MP().
    float4*             ModuleParams;
    // Declared per-particle attributes: PARTICLE_ATTR_FLOATS floats per particle, parallel to the core
    // particle buffer and indexed by the same particle index. A plain float* rather than a generated
    // struct because this header is parsed before any generated code and cannot name a generated type.
    float*              Attributes;
};

FParticleSimArgs*   SimArgs()      { return GetPassArgs<FParticleSimArgs>(); }
FParticleSimParams* SimParams()    { return SimArgs().Params; }
FGPUParticle*       Particles()    { return SimArgs().Particles; }
uint*               SpawnCounter() { return SimArgs().SpawnCounter; }
float*              PAttr()        { return SimArgs().Attributes; }

// Module parameter slot accessor. Generated module code indexes this instead of embedding literals, so
// the compiled shader is invariant to the values and only a structural stack edit needs a rebuild.
float4 MP(uint Slot) { return SimArgs().ModuleParams[Slot]; }

// Baked curve/gradient tables. MUST match kParticleLUTSamples in ParticleCompiler.h -- the compiler lays
// the tables out with this stride, so a mismatch reads into the next parameter.
static const uint PARTICLE_LUT_SAMPLES = 16u;

// Normalized index pair + blend factor shared by both samplers.
void LUTIndices(float T, out uint I0, out uint I1, out float Frac)
{
    const float U = saturate(T) * (float)(PARTICLE_LUT_SAMPLES - 1u);
    I0   = (uint)floor(U);
    I1   = min(I0 + 1u, PARTICLE_LUT_SAMPLES - 1u);
    Frac = U - (float)I0;
}

// Scalar curve: samples packed 4-per-slot from BaseSlot.
float SampleCurveLUT(uint BaseSlot, float T)
{
    uint I0, I1; float Frac;
    LUTIndices(T, I0, I1, Frac);
    const float A = MP(BaseSlot + (I0 >> 2u))[I0 & 3u];
    const float B = MP(BaseSlot + (I1 >> 2u))[I1 & 3u];
    return lerp(A, B, Frac);
}

// Colour gradient: one RGBA sample per slot from BaseSlot.
float4 SampleGradientLUT(uint BaseSlot, float T)
{
    uint I0, I1; float Frac;
    LUTIndices(T, I0, I1, Frac);
    return lerp(MP(BaseSlot + I0), MP(BaseSlot + I1), Frac);
}

uint WangHash(uint x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}

uint PCGHash(uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float RandUnit(inout uint state)
{
    state = PCGHash(state);
    return float(state) / 4294967295.0;
}

// [-1, 1]
float RandSigned(inout uint state)
{
    return RandUnit(state) * 2.0 - 1.0;
}

float3 RandInUnitSphere(inout uint state)
{
    // Reject until inside sphere.
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 p = float3(RandSigned(state), RandSigned(state), RandSigned(state));
        if (dot(p, p) <= 1.0) return p;
    }
    // Fallback normalized point on sphere surface.
    float u = RandUnit(state);
    float v = RandUnit(state);
    float theta = u * 6.2831853;
    float phi   = acos(2.0 * v - 1.0);
    float sinPhi = sin(phi);
    return float3(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi));
}

float3 RandOnUnitSphere(inout uint state)
{
    float u = RandUnit(state);
    float v = RandUnit(state);
    float theta = u * 6.2831853;
    float phi   = acos(2.0 * v - 1.0);
    float sinPhi = sin(phi);
    return float3(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi));
}

// Direction within a cone around 'forward' with the given half-angle (radians).
float3 RandDirectionInCone(inout uint state, float3 forward, float3 right, float3 up, float halfAngle)
{
    float angle = halfAngle * RandUnit(state);
    float t = RandUnit(state) * 6.2831853;
    float s = sin(angle);
    return forward * cos(angle) + (right * cos(t) + up * sin(t)) * s;
}

// Cheap 3D value noise for particle turbulence (no 3D texture). Output in [-1, 1].
float Hash31(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float ValueNoise(float3 x)
{
    float3 p = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = Hash31(p + float3(0, 0, 0));
    float n100 = Hash31(p + float3(1, 0, 0));
    float n010 = Hash31(p + float3(0, 1, 0));
    float n110 = Hash31(p + float3(1, 1, 0));
    float n001 = Hash31(p + float3(0, 0, 1));
    float n101 = Hash31(p + float3(1, 0, 1));
    float n011 = Hash31(p + float3(0, 1, 1));
    float n111 = Hash31(p + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);

    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);

    return lerp(nxy0, nxy1, f.z) * 2.0 - 1.0;
}

// Pseudo-curl: sample noise at three offsets to build a divergence-free-ish field.
float3 CurlishNoise(float3 p, float time, float scale, float speed)
{
    float3 q = p * scale + float3(0.0, time * speed, 0.0);
    const float e = 0.15;
    float3 d;
    d.x = ValueNoise(q + float3(31.3, 0.0, 0.0))
        - ValueNoise(q - float3(31.3, 0.0, 0.0));
    d.y = ValueNoise(q + float3(0.0, 47.7, 0.0))
        - ValueNoise(q - float3(0.0, 47.7, 0.0));
    d.z = ValueNoise(q + float3(0.0, 0.0, 19.1))
        - ValueNoise(q - float3(0.0, 0.0, 19.1));
    return d * (1.0 / (2.0 * e));
}

float3 SampleEmitterShape(inout uint state, uint shape, float3 shapeSize, float coneAngle,
                          float3 forward, float3 right, float3 up)
{
    if (shape == EMITTER_SHAPE_POINT)
    {
        return float3(0, 0, 0);
    }
    else if (shape == EMITTER_SHAPE_SPHERE)
    {
        return RandInUnitSphere(state) * shapeSize.x;
    }
    else if (shape == EMITTER_SHAPE_BOX)
    {
        float3 r = float3(RandSigned(state), RandSigned(state), RandSigned(state));
        return r * shapeSize;
    }
    else if (shape == EMITTER_SHAPE_CONE)
    {
        // Cone oriented along forward; base radius = x, height = y.
        float h = RandUnit(state);
        float t = RandUnit(state) * 6.2831853;
        float rAt = shapeSize.x * h;
        float3 offset = (right * cos(t) + up * sin(t)) * rAt + forward * (shapeSize.y * h);
        return offset;
    }
    else if (shape == EMITTER_SHAPE_RING)
    {
        float t = RandUnit(state) * 6.2831853;
        float outer = shapeSize.x;
        float inner = min(shapeSize.y, outer);
        float r = lerp(inner, outer, RandUnit(state));
        return (right * cos(t) + forward * sin(t)) * r;
    }
    else /* DISK */
    {
        float t = RandUnit(state) * 6.2831853;
        float outer = shapeSize.x;
        float inner = min(shapeSize.y, outer);
        float r = lerp(inner, outer, sqrt(RandUnit(state)));
        return (right * cos(t) + forward * sin(t)) * r;
    }
}
