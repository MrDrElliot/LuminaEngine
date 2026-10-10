#pragma once
#include "SceneGlobals.slang"
#include "Noise.slang"

// Structs must stay byte-identical to CParticleSystem and FParticleSimArgs in DefaultSceneRenderer.cpp.

// Must match EParticleEmitterShape.
static const uint EMITTER_SHAPE_POINT  = 0u;
static const uint EMITTER_SHAPE_SPHERE = 1u;
static const uint EMITTER_SHAPE_BOX    = 2u;
static const uint EMITTER_SHAPE_CONE   = 3u;
static const uint EMITTER_SHAPE_RING   = 4u;
static const uint EMITTER_SHAPE_DISK   = 5u;
static const uint EMITTER_SHAPE_HEMISPHERE = 6u;
static const uint EMITTER_SHAPE_MESH       = 7u;

// Must match EParticleVelocityMode.
static const uint VELOCITY_MODE_EXPLICIT = 0u;
static const uint VELOCITY_MODE_RADIAL   = 1u;

// Bitmask in Counts.w.
static const uint SIM_FLAG_LOOP          = (1u << 0);
static const uint SIM_FLAG_BURST_PENDING = (1u << 1);
static const uint SIM_FLAG_LOCAL_SPACE   = (1u << 2);

// Must match EParticleEventType.
static const uint PARTICLE_EVENT_DEATH      = 0u;
static const uint PARTICLE_EVENT_COLLISION  = 1u;
static const uint PARTICLE_EVENT_CONTINUOUS = 2u;
static const uint PARTICLE_NO_EVENT_LIST    = 0xFFFFFFFFu;

// Speed a particle spawned along its parent's collision normal moves at, which is only there to carry the direction.
static const float PARTICLE_SURFACE_NORMAL_SPEED = 0.001;

// Must match EParticleEmitFlags.
static const uint PARTICLE_EMIT_POSITION = 1u << 0;
static const uint PARTICLE_EMIT_VELOCITY = 1u << 1;
static const uint PARTICLE_EMIT_COLOR    = 1u << 2;
static const uint PARTICLE_EMIT_SIZE     = 1u << 3;

// Must match EParticleShapeType.
static const uint PARTICLE_SHAPE_SPHERE = 0u;
static const uint PARTICLE_SHAPE_BOX    = 1u;

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

// 256-byte uniform block, kept vec4-aligned for std140.
struct FParticleSimParams
{
    float4  EmitterPosition;
    float4  EmitterForward;      // xyz = forward (normalized), w = EmitterVelocity.x
    float4  EmitterRight;        // xyz = right   (normalized), w = EmitterVelocity.y
    float4  EmitterUp;           // xyz = up      (normalized), w = EmitterVelocity.z

    uint4   Counts;              // x=MaxParticles, y=SpawnCount, z=FrameSeed, w=SimFlags
    uint4   Modes;               // x=Shape, y=VelocityMode, z=reserved, w=reserved

    float4  ShapeSize;           // xyz dims; w=cone half-angle (radians)

    float4  VelocityMin;
    float4  VelocityMax;

    float4  SpeedAndLifetime;    // x=speedMin, y=speedMax, z=lifeMin, w=lifeMax
    float4  Gravity;             // xyz=gravity, w=drag

    float4  StartColor;
    float4  EndColor;

    float4  SizeRange;           // xy=start(min,max); zw=end(min,max)
    float4  RotationRange;       // xy=rot(min,max); zw=rotSpeed(min,max)

    float4  NoiseStrength;       // xyz=strength; w=scale
    float4  NoiseParams;         // x=speed, y=InheritEmitterVelocity; zw reserved

    float4  Timing;              // x=DeltaTime, y=TotalTime, z=SystemAge, w=EmitterScale

    float4x4 EmitterDelta;       // last frame's emitter transform to this frame's, for local space
    float4  EventParams;         // x=continuous events per second raised, y=parent velocity inherited
    uint4   EventInfo;           // x=event lists raised, y=list consumed, z=emission mesh slot, w=particles per event
    uint4   RibbonInfo;          // x=segments, y=head sample, z=head advanced this step
};

// A parent particle's death, contact or tick for a sub-emitter, and the payload of a script EmitParticle.
struct FParticleEvent
{
    float4  PositionSize;        // xyz position, w size
    float4  VelocityFlags;       // xyz velocity, w PARTICLE_EMIT flags as float bits
    float4  Extra;               // a script emit's color, or a collision's surface normal in xyz and impact speed in w
};

// Attractor or collider volume with unit axes, its scale folded into Extent.
struct FParticleShape
{
    float4  Center;              // xyz world center, w PARTICLE_SHAPE type
    float4  AxisX;
    float4  AxisY;
    float4  AxisZ;
    float4  Extent;              // xyz half extents, x alone for a sphere's radius
    float4  Params;              // x strength, y attenuation, z directionality
};

struct FParticleTerrain
{
    float4  OriginSize;          // x origin X, y origin Z, z tile world size, w base height
    float4  HeightParams;        // x max height
    uint4   Textures;            // x heightmap, y normal map
};

// Matches FParticleSimArgs in DefaultSceneRenderer.cpp.
struct FParticleSimArgs
{
    FParticleSimParams*  Params;
    GPUSpan<FGPUParticle> Particles;
    GPUSpan<uint>         SpawnCounter;   // scratch counter, one element
    // One float4 per module parameter, in FParticleCompiler slot order.
    GPUSpan<float4>       ModuleParams;
    // PARTICLE_ATTR_FLOATS per particle, a plain float span since this header cannot name generated types.
    GPUSpan<float>        Attributes;
    // Lists this emitter raises for its sub-emitters, PARTICLE_EVENT_CAPACITY per list, and their counts.
    GPUSpan<FParticleEvent> OwnEvents;
    GPUSpan<uint>           OwnEventCounts;
    // The parent's lists when this emitter is a sub-emitter.
    GPUSpan<FParticleEvent> SourceEvents;
    GPUSpan<uint>           SourceEventCounts;
    // EmitParticle calls queued from script this frame.
    GPUSpan<FParticleEvent> ScriptEvents;
    // RibbonInfo.x past positions per particle.
    GPUSpan<float4>         RibbonHistory;
    GPUSpan<FParticleShape> Attractors;
    GPUSpan<FParticleShape> Colliders;
    GPUSpan<FParticleTerrain> Terrains;
};

FParticleSimArgs*      SimArgs()      { return GetPassArgs<FParticleSimArgs>(); }
FParticleSimParams*    SimParams()    { return SimArgs().Params; }
GPUSpan<FGPUParticle> Particles()    { return SimArgs().Particles; }
GPUSpan<uint>         SpawnCounter() { return SimArgs().SpawnCounter; }
GPUSpan<float>        PAttr()        { return SimArgs().Attributes; }

// Uniform scale of the emitter's transform, so a scaled effect keeps its shape at a different size.
float EmitterScale() { return SimParams().Timing.w; }

// Must match EParticleSortMode.
static const uint PARTICLE_SORT_BY_DISTANCE    = 0u;
static const uint PARTICLE_SORT_OLDEST_FIRST   = 1u;
static const uint PARTICLE_SORT_YOUNGEST_FIRST = 2u;

// Zero to one, where an ascending sort draws the smallest first.
float ParticleSortValue(FGPUParticle P, float3 CameraPos, uint SortMode)
{
    const float LifeRatio = saturate(P.Age / max(P.Lifetime, 1e-4));
    if (SortMode == PARTICLE_SORT_OLDEST_FIRST)
    {
        return 1.0 - LifeRatio;
    }
    if (SortMode == PARTICLE_SORT_YOUNGEST_FIRST)
    {
        return LifeRatio;
    }
    // Reciprocal distance, so far particles take the small keys and draw first.
    return 1.0 / (1.0 + length(P.Position - CameraPos));
}

// Appends to one of this emitter's event lists when a sub-emitter consumes it, and is a no-op otherwise.
void RaiseParticleEvent(uint List, FGPUParticle P)
{
    if ((SimParams().EventInfo.x & (1u << List)) == 0u)
    {
        return;
    }

    uint Slot;
    InterlockedAdd(SimArgs().OwnEventCounts.At(List)[0], 1u, Slot);
    if (Slot < PARTICLE_EVENT_CAPACITY)
    {
        FParticleEvent Event;
        Event.PositionSize  = float4(P.Position, P.Size);
        Event.VelocityFlags = float4(P.Velocity, 0.0);
        Event.Extra         = P.Color;
        SimArgs().OwnEvents.Set(List * PARTICLE_EVENT_CAPACITY + Slot, Event);
    }
}

// A contact carries the surface it met and how hard, for sub-emitters that lie on it and scripts that read hits back.
void RaiseParticleCollision(FGPUParticle P, float3 Normal, float ImpactSpeed)
{
    if ((SimParams().EventInfo.x & (1u << PARTICLE_EVENT_COLLISION)) == 0u)
    {
        return;
    }

    uint Slot;
    InterlockedAdd(SimArgs().OwnEventCounts.At(PARTICLE_EVENT_COLLISION)[0], 1u, Slot);
    if (Slot < PARTICLE_EVENT_CAPACITY)
    {
        FParticleEvent Event;
        Event.PositionSize  = float4(P.Position, P.Size);
        Event.VelocityFlags = float4(P.Velocity, 0.0);
        Event.Extra         = float4(Normal, ImpactSpeed);
        SimArgs().OwnEvents.Set(PARTICLE_EVENT_COLLISION * PARTICLE_EVENT_CAPACITY + Slot, Event);
    }
}

// Carries a live particle, and its ribbon history, along with the emitter's motion since last frame.
void ApplyLocalSpace(uint Index, inout FGPUParticle P)
{
    if ((SimParams().Counts.w & SIM_FLAG_LOCAL_SPACE) == 0u)
    {
        return;
    }

    const float4x4 Delta = SimParams().EmitterDelta;
    P.Position = mul(Delta, float4(P.Position, 1.0)).xyz;
    P.Velocity = mul(Delta, float4(P.Velocity, 0.0)).xyz;

    const uint Segments = SimParams().RibbonInfo.x;
    for (uint Sample = 0u; Sample < Segments; ++Sample)
    {
        const uint Slot = Index * Segments + Sample;
        const float4 Old = SimArgs().RibbonHistory.Get(Slot);
        SimArgs().RibbonHistory.Set(Slot, float4(mul(Delta, float4(Old.xyz, 1.0)).xyz, Old.w));
    }
}

void InitializeRibbon(uint Index, float3 Position)
{
    const uint Segments = SimParams().RibbonInfo.x;
    for (uint Sample = 0u; Sample < Segments; ++Sample)
    {
        SimArgs().RibbonHistory.Set(Index * Segments + Sample, float4(Position, 0.0));
    }
}

void RecordRibbon(uint Index, float3 Position)
{
    const uint Segments = SimParams().RibbonInfo.x;
    if (Segments != 0u && SimParams().RibbonInfo.z != 0u)
    {
        SimArgs().RibbonHistory.Set(Index * Segments + SimParams().RibbonInfo.y, float4(Position, 0.0));
    }
}

float3 ShapeToLocal(FParticleShape Shape, float3 WorldPos)
{
    const float3 Offset = WorldPos - Shape.Center.xyz;
    return float3(dot(Offset, Shape.AxisX.xyz), dot(Offset, Shape.AxisY.xyz), dot(Offset, Shape.AxisZ.xyz));
}

float3 ShapeToWorldDir(FParticleShape Shape, float3 LocalDir)
{
    return Shape.AxisX.xyz * LocalDir.x + Shape.AxisY.xyz * LocalDir.y + Shape.AxisZ.xyz * LocalDir.z;
}

void ApplyAttractors(float3 Position, inout float3 Velocity, float DeltaTime, float StrengthScale)
{
    const uint Count = SimArgs().Attractors.Count;
    for (uint Index = 0u; Index < Count; ++Index)
    {
        const FParticleShape Shape = SimArgs().Attractors.Get(Index);
        const float3 Local = ShapeToLocal(Shape, Position);

        // Zero at the edge and one at the center, whichever shape the volume is.
        float Closeness;
        if ((uint)Shape.Center.w == PARTICLE_SHAPE_BOX)
        {
            Closeness = 1.0 - max(max(abs(Local.x) / max(Shape.Extent.x, 1e-4), abs(Local.y) / max(Shape.Extent.y, 1e-4)), abs(Local.z) / max(Shape.Extent.z, 1e-4));
        }
        else
        {
            Closeness = 1.0 - length(Local) / max(Shape.Extent.x, 1e-4);
        }

        if (Closeness <= 0.0)
        {
            continue;
        }

        const float3 ToCenter = Shape.Center.xyz - Position;
        const float3 Pull     = normalize(lerp(ToCenter / max(length(ToCenter), 1e-4), Shape.AxisZ.xyz, Shape.Params.z) + 1e-6);
        const float  Falloff  = pow(saturate(Closeness), max(Shape.Params.y, 0.0));
        Velocity += Pull * (Shape.Params.x * StrengthScale * Falloff * DeltaTime);
    }
}

void ReflectOffSurface(inout float3 Velocity, float3 Normal, float Restitution, float Friction)
{
    const float Into = dot(Velocity, Normal);
    if (Into < 0.0)
    {
        const float3 Along = Velocity - Normal * Into;
        Velocity = Along * (1.0 - Friction) - Normal * (Into * Restitution);
    }
}

// Pushes a particle of the given Radius out of every collider volume and terrain. Returns true on contact.
bool CollideWithShapes(inout float3 Position, inout float3 Velocity, float Restitution, float Friction, float Radius, bool bTerrain, out float3 HitNormal)
{
    bool bHit = false;
    HitNormal = float3(0.0, 1.0, 0.0);

    const uint ColliderCount = SimArgs().Colliders.Count;
    for (uint Index = 0u; Index < ColliderCount; ++Index)
    {
        const FParticleShape Shape = SimArgs().Colliders.Get(Index);
        float3 Local = ShapeToLocal(Shape, Position);
        float3 LocalNormal;

        if ((uint)Shape.Center.w == PARTICLE_SHAPE_BOX)
        {
            const float3 Reach = Shape.Extent.xyz + Radius;
            const float3 Depth = Reach - abs(Local);
            if (any(Depth <= 0.0))
            {
                continue;
            }

            // Out through the face the particle is least far inside.
            if (Depth.x <= Depth.y && Depth.x <= Depth.z)
            {
                LocalNormal = float3(sign(Local.x), 0.0, 0.0);
                Local.x = LocalNormal.x * Reach.x;
            }
            else if (Depth.y <= Depth.z)
            {
                LocalNormal = float3(0.0, sign(Local.y), 0.0);
                Local.y = LocalNormal.y * Reach.y;
            }
            else
            {
                LocalNormal = float3(0.0, 0.0, sign(Local.z));
                Local.z = LocalNormal.z * Reach.z;
            }
        }
        else
        {
            const float Reach    = Shape.Extent.x + Radius;
            const float Distance = length(Local);
            if (Distance >= Reach)
            {
                continue;
            }

            LocalNormal = Distance > 1e-5 ? Local / Distance : float3(0.0, 1.0, 0.0);
            Local = LocalNormal * Reach;
        }

        const float3 Normal = ShapeToWorldDir(Shape, LocalNormal);
        Position = Shape.Center.xyz + ShapeToWorldDir(Shape, Local);
        ReflectOffSurface(Velocity, Normal, Restitution, Friction);
        HitNormal = Normal;
        bHit = true;
    }

    const uint TerrainCount = bTerrain ? SimArgs().Terrains.Count : 0u;
    for (uint Index = 0u; Index < TerrainCount; ++Index)
    {
        const FParticleTerrain Terrain = SimArgs().Terrains.Get(Index);
        const float2 UV = (Position.xz - Terrain.OriginSize.xy) / Terrain.OriginSize.z + 0.5;
        if (any(saturate(UV) != UV))
        {
            continue;
        }

        const float Ground = Terrain.OriginSize.w + FTexture2D(Terrain.Textures.x).SampleLevel(Samplers::LinearClamp, UV, 0.0).r * Terrain.HeightParams.x;
        if (Position.y >= Ground + Radius)
        {
            continue;
        }

        const float3 Normal = normalize(FTexture2D(Terrain.Textures.y).SampleLevel(Samplers::LinearClamp, UV, 0.0).xyz * 2.0 - 1.0);
        Position.y = Ground + Radius;
        ReflectOffSurface(Velocity, Normal, Restitution, Friction);
        HitNormal = Normal;
        bHit = true;
    }

    return bHit;
}

bool CollideWithShapes(inout float3 Position, inout float3 Velocity, float Restitution, float Friction, float Radius, bool bTerrain)
{
    float3 HitNormal;
    return CollideWithShapes(Position, Velocity, Restitution, Friction, Radius, bTerrain, HitNormal);
}

// Generated module code reads values here so only a structural stack edit needs a rebuild.
float4 MP(uint Slot) { return SimArgs().ModuleParams.Get(Slot); }

// Must match kParticleLUTSamples in ParticleCompiler.h, the stride the compiler lays tables out with.
static const uint PARTICLE_LUT_SAMPLES = 16u;

void LUTIndices(float T, out uint I0, out uint I1, out float Frac)
{
    const float U = saturate(T) * (float)(PARTICLE_LUT_SAMPLES - 1u);
    I0   = (uint)floor(U);
    I1   = min(I0 + 1u, PARTICLE_LUT_SAMPLES - 1u);
    Frac = U - (float)I0;
}

// Samples packed four per slot from BaseSlot.
float SampleCurveLUT(uint BaseSlot, float T)
{
    uint I0, I1; float Frac;
    LUTIndices(T, I0, I1, Frac);
    const float A = MP(BaseSlot + (I0 >> 2u))[I0 & 3u];
    const float B = MP(BaseSlot + (I1 >> 2u))[I1 & 3u];
    return lerp(A, B, Frac);
}

// One RGBA sample per slot from BaseSlot.
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

float RandSigned(inout uint state)
{
    return RandUnit(state) * 2.0 - 1.0;
}

float3 RandInUnitSphere(inout uint state)
{
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 p = float3(RandSigned(state), RandSigned(state), RandSigned(state));
        if (dot(p, p) <= 1.0) return p;
    }
    // Falls back to a surface point after four rejections.
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

// Turns a color about the gray axis, which shifts its hue while keeping its brightness.
float3 RotateHue(float3 Color, float Angle)
{
    const float3 Gray = float3(0.57735027, 0.57735027, 0.57735027);
    const float  C    = cos(Angle);
    return max(Color * C + cross(Gray, Color) * sin(Angle) + Gray * dot(Gray, Color) * (1.0 - C), 0.0);
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
bool CollideWithSceneDepth(inout float3 Position, inout float3 Velocity, float DeltaTime, float Restitution, float Friction, float Thickness, out float3 HitNormal)
{
    HitNormal = float3(0.0, 1.0, 0.0);
    const FTexture2D SceneDepth = Scene().SceneDepth;
    if (!SceneDepth.IsValid())
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

    const float Raw = SceneDepth.SampleLevel(Samplers::PointClamp, UV, 0.0).r;
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
    const float3 Px    = ReconstructViewPos(UVx, SceneDepth.SampleLevel(Samplers::PointClamp, UVx, 0.0).r, InvProj);
    const float3 Py    = ReconstructViewPos(UVy, SceneDepth.SampleLevel(Samplers::PointClamp, UVy, 0.0).r, InvProj);
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
    HitNormal = Normal;
    return true;
}

bool CollideWithSceneDepth(inout float3 Position, inout float3 Velocity, float DeltaTime, float Restitution, float Friction, float Thickness)
{
    float3 HitNormal;
    return CollideWithSceneDepth(Position, Velocity, DeltaTime, Restitution, Friction, Thickness, HitNormal);
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

// Pseudo-curl from three offset samples, only roughly divergence free.
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

// A random point on the emission mesh's finest LOD in its own space, or the origin when it is not resident.
float3 SampleEmissionMesh(inout uint state)
{
    const uint Slot = SimParams().EventInfo.z;
    if (Slot == 0u || Slot >= MeshletHeaders().Count)
    {
        return float3(0.0, 0.0, 0.0);
    }

    FMeshletHeader* Header = MeshletHeaders().At(Slot);
    const uint MeshletCount = Header.MeshletCount;
    if (MeshletCount == 0u)
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Coarser LODs overlap the finest one, so a draw from them would bias toward simplified surfaces.
    FMeshlet M = Header.Meshlets[min((uint)(RandUnit(state) * (float)MeshletCount), MeshletCount - 1u)];
    for (uint Retry = 0u; Retry < 4u && M.LODIndex != 0u; ++Retry)
    {
        M = Header.Meshlets[min((uint)(RandUnit(state) * (float)MeshletCount), MeshletCount - 1u)];
    }

    if (M.TriangleCount == 0u)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const uint Triangle = min((uint)(RandUnit(state) * (float)M.TriangleCount), M.TriangleCount - 1u);
    const uint3 Corners = unpack_u8u32(Header.Triangles[M.TriangleOffset + Triangle]).xyz;
    if (any(Corners >= M.VertexCount))
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Folding the square keeps the barycentric sample uniform over the triangle.
    float U = RandUnit(state);
    float V = RandUnit(state);
    if (U + V > 1.0)
    {
        U = 1.0 - U;
        V = 1.0 - V;
    }

    const float3 A = LoadBindPosePosition(Header, M, Corners.x);
    const float3 B = LoadBindPosePosition(Header, M, Corners.y);
    const float3 C = LoadBindPosePosition(Header, M, Corners.z);
    return A + (B - A) * U + (C - A) * V;
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
    else if (shape == EMITTER_SHAPE_MESH)
    {
        const float3 p = SampleEmissionMesh(state) * shapeSize.x;
        return right * p.x + up * p.y + forward * p.z;
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
    else // EMITTER_SHAPE_DISK
    {
        float t = RandUnit(state) * 6.2831853;
        float outer = shapeSize.x;
        float inner = min(shapeSize.y, outer);
        float r = lerp(inner, outer, sqrt(RandUnit(state)));
        return (right * cos(t) + forward * sin(t)) * r;
    }
}
