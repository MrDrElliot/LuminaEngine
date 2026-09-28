#pragma once
#include "SceneGlobals.slang"
#include "Noise.slang"

// Shared data layout + RNG/noise/emitter helpers for the two particle simulation shaders
// (ParticleSimulate.slang, the data-driven default, and Particles/ParticleSimulateTemplate.slang,
// the FParticleCompiler module-stack skeleton). Both consume the SAME CPU-side blocks, so the
// structs here must stay byte-identical to CParticleSystem / FParticleSimArgs in
// DefaultSceneRenderer.cpp -- keeping them in one place is what prevents the two sims drifting.

// Emitter shape enums (must match EParticleEmitterShape)
static const uint EMITTER_SHAPE_POINT  = 0u;
static const uint EMITTER_SHAPE_SPHERE = 1u;
static const uint EMITTER_SHAPE_BOX    = 2u;
static const uint EMITTER_SHAPE_CONE   = 3u;
static const uint EMITTER_SHAPE_RING   = 4u;
static const uint EMITTER_SHAPE_DISK   = 5u;
static const uint EMITTER_SHAPE_HEMISPHERE = 6u;

// Velocity mode enums (must match EParticleVelocityMode)
static const uint VELOCITY_MODE_EXPLICIT = 0u;
static const uint VELOCITY_MODE_RADIAL   = 1u;

// Sim flags (bitmask in Counts.w)
static const uint SIM_FLAG_LOOP          = (1u << 0);
static const uint SIM_FLAG_BURST_PENDING = (1u << 1);

// 64-byte particle. Do not change stride without updating DefaultSceneRenderer.
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

    float4  Timing;              // x=DeltaTime, y=TotalTime, z=SystemAge, w=EmitterScale
};

// Everything by device address; matches FParticleSimArgs in DefaultSceneRenderer.cpp.
struct FParticleSimArgs
{
    FParticleSimParams*  Params;
    GPUSpan<FGPUParticle> Particles;
    GPUSpan<uint>         SpawnCounter;   // scratch counter, one element
    // Module-stack value inputs, one float4 slot per parameter, in the order FParticleCompiler assigned
    // them. Empty when the stack declared no parameters, in which case nothing reads MP().
    GPUSpan<float4>       ModuleParams;
    // Declared per-particle attributes: PARTICLE_ATTR_FLOATS floats per particle, parallel to the core
    // particle buffer and indexed by the same particle index. A plain float span rather than a generated
    // struct because this header is parsed before any generated code and cannot name a generated type.
    GPUSpan<float>        Attributes;
};

FParticleSimArgs*      SimArgs()      { return GetPassArgs<FParticleSimArgs>(); }
FParticleSimParams*    SimParams()    { return SimArgs().Params; }
GPUSpan<FGPUParticle>* Particles()    { return &SimArgs().Particles; }
GPUSpan<uint>*         SpawnCounter() { return &SimArgs().SpawnCounter; }
GPUSpan<float>*        PAttr()        { return &SimArgs().Attributes; }

// Uniform scale of the emitter's transform, so a scaled effect keeps its shape at a different size.
float EmitterScale() { return SimParams().Timing.w; }

// Module parameter slot accessor. Generated module code indexes this instead of embedding literals, so
// the compiled shader is invariant to the values and only a structural stack edit needs a rebuild.
float4 MP(uint Slot) { return SpanGet(&SimArgs().ModuleParams, Slot); }

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

// Direction within a cone around 'up' with the given half-angle (radians), the axis rings and disks face.
float3 RandDirectionInCone(inout uint state, float3 forward, float3 right, float3 up, float halfAngle)
{
    float angle = halfAngle * RandUnit(state);
    float t = RandUnit(state) * 6.2831853;
    float s = sin(angle);
    return up * cos(angle) + (right * cos(t) + forward * sin(t)) * s;
}

// Reads the opaque depth buffer, so only surfaces the camera sees collide. Returns true on contact.
bool CollideWithSceneDepth(inout float3 Position, inout float3 Velocity, float DeltaTime, float Restitution, float Friction, float Thickness)
{
    const uint DepthIndex = Scene().SceneDepthIndex;
    if (DepthIndex == 0xFFFFFFFFu)
    {
        return false;
    }

    const float3 ViewPos = WorldToView(Position + Velocity * DeltaTime);
    const float4 Clip    = mul(GetCameraProjection(), float4(ViewPos, 1.0));
    if (Clip.w <= 0.0)
    {
        return false;
    }

    const float2 UV = (Clip.xy / Clip.w) * 0.5 + 0.5;
    if (any(saturate(UV) != UV))
    {
        return false;
    }

    const float Raw = SampleTexture2DLevel(DepthIndex, SAMPLER_POINT_CLAMP, UV, 0.0).r;
    if (Raw <= 0.0)
    {
        return false;
    }

    // A fast particle crosses more than the authored thickness in one step, and must still register the surface it passed.
    const float4x4 InvProj  = GetInverseCameraProjection();
    const float3   SurfaceP = ReconstructViewPos(UV, Raw, InvProj);
    const float    Depth    = ViewPos.z - SurfaceP.z;
    if (Depth <= 0.0 || Depth > max(Thickness, length(Velocity) * DeltaTime * 2.0))
    {
        return false;
    }

    const float2 Texel = 2.0 / float2(GetScreenSize());
    const float2 UVx   = UV + float2(Texel.x, 0.0);
    const float2 UVy   = UV + float2(0.0, Texel.y);
    const float3 Px    = ReconstructViewPos(UVx, SampleTexture2DLevel(DepthIndex, SAMPLER_POINT_CLAMP, UVx, 0.0).r, InvProj);
    const float3 Py    = ReconstructViewPos(UVy, SampleTexture2DLevel(DepthIndex, SAMPLER_POINT_CLAMP, UVy, 0.0).r, InvProj);
    float3 ViewN = normalize(cross(Py - SurfaceP, Px - SurfaceP));
    if (dot(ViewN, SurfaceP) > 0.0)
    {
        ViewN = -ViewN;
    }

    const float3 Normal = normalize(mul(GetInverseCameraView(), float4(ViewN, 0.0)).xyz);
    const float  Into   = dot(Velocity, Normal);
    if (Into < 0.0)
    {
        const float3 Along = Velocity - Normal * Into;
        Velocity = Along * (1.0 - Friction) - Normal * (Into * Restitution);
    }

    Position = mul(GetInverseCameraView(), float4(SurfaceP, 1.0)).xyz + Normal * 0.02;
    return true;
}

// Emitter-local offset or velocity to world, x along right, y along up and z along forward.
float3 EmitterToWorld(float3 Local)
{
    return SimParams().EmitterRight.xyz * Local.x + SimParams().EmitterUp.xyz * Local.y + SimParams().EmitterForward.xyz * Local.z;
}

// Signed remap of the shared value noise, which is what the turbulence field expects.
float ValueNoise(float3 x)
{
    return ValueNoise3D(x) * 2.0 - 1.0;
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
    else if (shape == EMITTER_SHAPE_HEMISPHERE)
    {
        float3 p = RandInUnitSphere(state);
        return (p - up * min(0.0, 2.0 * dot(p, up))) * shapeSize.x;
    }
    else if (shape == EMITTER_SHAPE_BOX)
    {
        float3 r = float3(RandSigned(state), RandSigned(state), RandSigned(state)) * shapeSize;
        return right * r.x + up * r.y + forward * r.z;
    }
    else if (shape == EMITTER_SHAPE_CONE)
    {
        // Cone opening along up; base radius = x, height = y.
        float h = RandUnit(state);
        float t = RandUnit(state) * 6.2831853;
        float rAt = shapeSize.x * h;
        float3 offset = (right * cos(t) + forward * sin(t)) * rAt + up * (shapeSize.y * h);
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
