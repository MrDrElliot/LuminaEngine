// Skeleton FParticleCompiler fills with a module stack, sharing its layout and helpers with ParticleSimulate.slang.

#include "Includes/ParticleSimCommon.slang"

// Floats per particle in the attribute buffer, at least one so the indexing stays well formed.
static const uint PARTICLE_ATTR_FLOATS = $PARTICLE_ATTR_COUNT;

// Where a dead slot's new particle came from, so the spawn knows its origin and what to override.
static const uint SPAWN_NONE    = 0u;
static const uint SPAWN_EMITTER = 1u;
static const uint SPAWN_SCRIPT  = 2u;
static const uint SPAWN_EVENT   = 3u;

// Must match PARTICLE_STAGE_PARTICLES and PARTICLE_STAGE_TARGET in SceneRenderTypes.h, with grid stage N dispatched as N + 1.
static const uint PARTICLE_STAGE_PARTICLES = 0u;
static const uint PARTICLE_STAGE_TARGET    = 0xFFFFFFFFu;
static const uint PARTICLE_STAGE_NEIGHBORS = 0xFFFFFFFEu;

// Must match PARTICLE_GRID_FLAG in SceneRenderTypes.h.
static const uint GRID_FLAG_ABSORB = 1u << 0;
static const uint GRID_FLAG_WRAP   = 1u << 1;

// Floats per grid cell, at least one so the cell arrays stay well formed on an emitter without a grid.
static const uint PARTICLE_GRID_CHANNELS = $PARTICLE_GRID_CHANNELS;

// A deposit is summed as an integer this many times larger, since integer atomics are the portable kind.
static const float PARTICLE_GRID_DEPOSIT_SCALE = 8192.0;

$PARTICLE_GRID_CONSTANTS
$PARTICLE_ARRAY_CONSTANTS
// How many values a script last handed the array, zero before it has.
uint ArrayCount(uint Array)
{
    return SimArgs().ArrayRanges.Get(Array).y;
}

// Clamped to the last value, so an index past the end repeats it rather than reading another array.
float4 ArrayAt(uint Array, uint Index)
{
    const uint2 Range = SimArgs().ArrayRanges.Get(Array);
    return Range.y == 0u ? float4(0.0, 0.0, 0.0, 0.0) : SimArgs().ArrayData.Get(Range.x + min(Index, Range.y - 1u));
}

uint2  GridSize()      { return SimParams().GridInfo.xy; }
float2 GridWorldSize() { return max(SimParams().GridParams.xy, float2(1e-4, 1e-4)); }
bool   GridWraps()     { return (SimParams().GridInfo.w & GRID_FLAG_WRAP) != 0u; }

uint GridSlot(int2 Coord, uint Channel)
{
    const int2 Size = int2(GridSize());
    Coord = GridWraps() ? ((Coord % Size) + Size) % Size : clamp(Coord, int2(0, 0), Size - 1);
    return ((uint)Coord.y * (uint)Size.x + (uint)Coord.x) * PARTICLE_GRID_CHANNELS + Channel;
}

// The grid as the previous stage left it, so a stencil sees every neighbor from the same moment.
float GridAt(int2 Coord, uint Channel)
{
    return SimArgs().GridRead.Get(GridSlot(Coord, Channel));
}

float2 GridCellSize()
{
    return GridWorldSize() / float2(GridSize());
}

// Four neighbors less four times the center, the discrete Laplacian in cell units.
float GridLaplacian(int2 Coord, uint Channel)
{
    return GridAt(Coord + int2(1, 0), Channel) + GridAt(Coord - int2(1, 0), Channel)
         + GridAt(Coord + int2(0, 1), Channel) + GridAt(Coord - int2(0, 1), Channel)
         - 4.0 * GridAt(Coord, Channel);
}

// The nine point Laplacian reaction diffusion is usually tuned against, smoother on diagonals than the five point one.
float GridLaplacian9(int2 Coord, uint Channel)
{
    const float Adjacent = GridAt(Coord + int2(1, 0), Channel) + GridAt(Coord - int2(1, 0), Channel)
                         + GridAt(Coord + int2(0, 1), Channel) + GridAt(Coord - int2(0, 1), Channel);
    const float Diagonal = GridAt(Coord + int2(1, 1), Channel) + GridAt(Coord - int2(1, 1), Channel)
                         + GridAt(Coord + int2(1, -1), Channel) + GridAt(Coord + int2(-1, 1), Channel);
    return 0.2 * Adjacent + 0.05 * Diagonal - GridAt(Coord, Channel);
}

// One inside Radius scaled by Hardness, fading to zero at Radius.
float GridBrushMask(float Distance, float Radius, float Hardness)
{
    return saturate((Radius - Distance) / max(Radius * (1.0 - Hardness), 1e-5));
}

// Bilinear between cell centers, with UV running zero to one across the grid.
float SampleGridUV(uint Channel, float2 UV)
{
    const float2 Cells = UV * float2(GridSize()) - 0.5;
    const int2   Base  = int2(floor(Cells));
    const float2 F     = Cells - float2(Base);
    const float  Low   = lerp(GridAt(Base, Channel), GridAt(Base + int2(1, 0), Channel), F.x);
    const float  High  = lerp(GridAt(Base + int2(0, 1), Channel), GridAt(Base + int2(1, 1), Channel), F.x);
    return lerp(Low, High, F.y);
}

// The grid lies across the emitter's right and forward axes, centered on it.
float2 WorldToGridUV(float3 WorldPos)
{
    const float3 Offset = WorldPos - SimParams().EmitterPosition.xyz;
    return float2(dot(Offset, SimParams().EmitterRight.xyz), dot(Offset, SimParams().EmitterForward.xyz)) / GridWorldSize() + 0.5;
}

float3 GridUVToWorld(float2 UV)
{
    const float2 Plane = (UV - 0.5) * GridWorldSize();
    return SimParams().EmitterPosition.xyz + SimParams().EmitterRight.xyz * Plane.x + SimParams().EmitterForward.xyz * Plane.y;
}

// A vector kept in grid axes, the way the fluid modules store velocity, turned into world space.
float3 GridToWorldDir(float2 GridDir)
{
    return SimParams().EmitterRight.xyz * GridDir.x + SimParams().EmitterForward.xyz * GridDir.y;
}

float2 WorldToGridDir(float3 WorldDir)
{
    return float2(dot(WorldDir, SimParams().EmitterRight.xyz), dot(WorldDir, SimParams().EmitterForward.xyz));
}

float SampleGrid(uint Channel, float3 WorldPos)
{
    return SampleGridUV(Channel, WorldToGridUV(WorldPos));
}

// How fast a channel rises per meter across the grid plane, as a world direction.
float3 GridGradient(uint Channel, float3 WorldPos)
{
    const float2 UV    = WorldToGridUV(WorldPos);
    const float2 Texel = 1.0 / float2(GridSize());
    const float  DX    = SampleGridUV(Channel, UV + float2(Texel.x, 0.0)) - SampleGridUV(Channel, UV - float2(Texel.x, 0.0));
    const float  DY    = SampleGridUV(Channel, UV + float2(0.0, Texel.y)) - SampleGridUV(Channel, UV - float2(0.0, Texel.y));
    return GridToWorldDir(float2(DX, DY) / (2.0 * Texel * GridWorldSize()));
}

bool IsInsideGrid(float3 WorldPos)
{
    const float2 UV = WorldToGridUV(WorldPos);
    return GridWraps() || all(UV >= 0.0 && UV <= 1.0);
}

void DepositCell(int2 Coord, uint Channel, float Amount)
{
    const int2 Size = int2(GridSize());
    if (!GridWraps() && (any(Coord < 0) || any(Coord >= Size)))
    {
        return;
    }

    int Previous;
    InterlockedAdd(SimArgs().GridDeposit.At(GridSlot(Coord, Channel))[0], (int)round(Amount * PARTICLE_GRID_DEPOSIT_SCALE), Previous);
}

// Splats Amount over the four nearest cells, where the next grid stage adds it in.
void DepositGrid(uint Channel, float3 WorldPos, float Amount)
{
    const float2 Cells = WorldToGridUV(WorldPos) * float2(GridSize()) - 0.5;
    const int2   Base  = int2(floor(Cells));
    const float2 F     = Cells - float2(Base);
    DepositCell(Base,              Channel, Amount * (1.0 - F.x) * (1.0 - F.y));
    DepositCell(Base + int2(1, 0), Channel, Amount * F.x * (1.0 - F.y));
    DepositCell(Base + int2(0, 1), Channel, Amount * (1.0 - F.x) * F.y);
    DepositCell(Base + int2(1, 1), Channel, Amount * F.x * F.y);
}

// Cell holds this cell's channels, which each module reads and rewrites in stack order.
$PARTICLE_GRID_STAGE_FUNCS
void RunGridStage(uint Stage, inout float Cell[PARTICLE_GRID_CHANNELS], int2 Coord, float2 UV, float3 WorldPos, float DeltaTime, float TotalTime, uint Iteration)
{
    switch (Stage)
    {
$PARTICLE_GRID_STAGE_CASES
    default:
        break;
    }
}

void RunGridCell(uint Index)
{
    const uint2 Size    = GridSize();
    const int2  Coord   = int2(Index % Size.x, Index / Size.x);
    const uint  Base    = Index * PARTICLE_GRID_CHANNELS;
    const bool  bAbsorb = (SimParams().GridInfo.w & GRID_FLAG_ABSORB) != 0u;

    float Cell[PARTICLE_GRID_CHANNELS];
    for (uint Channel = 0u; Channel < PARTICLE_GRID_CHANNELS; ++Channel)
    {
        Cell[Channel] = SimArgs().GridRead.Get(Base + Channel);
        if (bAbsorb)
        {
            Cell[Channel] += (float)SimArgs().GridDeposit.Get(Base + Channel) / PARTICLE_GRID_DEPOSIT_SCALE;
            SimArgs().GridDeposit.Set(Base + Channel, 0);
        }
    }

    const float2 UV = (float2(Coord) + 0.5) / float2(Size);
    RunGridStage(SimParams().GridInfo.z - 1u, Cell, Coord, UV, GridUVToWorld(UV), SimParams().Timing.x, SimParams().Timing.y, (uint)SimParams().GridParams.z);

    for (uint Channel = 0u; Channel < PARTICLE_GRID_CHANNELS; ++Channel)
    {
        SimArgs().GridWrite.Set(Base + Channel, Cell[Channel]);
    }
}

// The first four channels map to RGBA unless an output module says otherwise, with alpha one when fewer exist.
float4 GridTargetColor(float Cell[PARTICLE_GRID_CHANNELS], float2 UV, float3 WorldPos)
{
    float4 Color = float4(0.0, 0.0, 0.0, 1.0);
    for (uint Channel = 0u; Channel < min(PARTICLE_GRID_CHANNELS, 4u); ++Channel)
    {
        Color[Channel] = Cell[Channel];
    }
$PARTICLE_GRID_OUTPUT_FUNC
    return Color;
}

void WriteGridTarget(uint Index)
{
    const uint2  Extent = SimParams().GridTarget.yz;
    const uint2  Texel  = uint2(Index % Extent.x, Index / Extent.x);
    const float2 UV     = (float2(Texel) + 0.5) / float2(Extent);

    float Cell[PARTICLE_GRID_CHANNELS];
    for (uint Channel = 0u; Channel < PARTICLE_GRID_CHANNELS; ++Channel)
    {
        Cell[Channel] = SampleGridUV(Channel, UV);
    }

    FRWTexture2D(SimParams().GridTarget.x)[Texel] = GridTargetColor(Cell, UV, GridUVToWorld(UV));
}

// The cube of neighbor cells is centered on the emitter and lines up with the world axes.
int3 NeighborCellOf(float3 WorldPos)
{
    const float Resolution = SimParams().NeighborParams.y;
    return int3(floor((WorldPos - SimParams().EmitterPosition.xyz) / max(SimParams().NeighborParams.x, 1e-4) + Resolution * 0.5));
}

// The cell's flat index, or 0xFFFFFFFF outside the cube.
uint NeighborCellIndex(int3 Cell)
{
    const int Resolution = (int)SimParams().NeighborParams.y;
    if (SimArgs().NeighborCounts.IsEmpty() || any(Cell < 0) || any(Cell >= Resolution))
    {
        return 0xFFFFFFFFu;
    }
    return ((uint)Cell.z * (uint)Resolution + (uint)Cell.y) * (uint)Resolution + (uint)Cell.x;
}

uint NeighborCount(int3 Cell)
{
    const uint Flat = NeighborCellIndex(Cell);
    return Flat == 0xFFFFFFFFu ? 0u : min(SimArgs().NeighborCounts.Get(Flat), (uint)SimParams().NeighborParams.z);
}

uint NeighborAt(int3 Cell, uint Slot)
{
    return SimArgs().NeighborIndices.Get(NeighborCellIndex(Cell) * (uint)SimParams().NeighborParams.z + Slot);
}

void InsertNeighbor(uint Index)
{
    const FGPUParticle P = Particles().Get(Index);
    const uint Flat = NeighborCellIndex(NeighborCellOf(P.Position));
    if (P.Lifetime <= 0.0 || P.Age >= P.Lifetime || Flat == 0xFFFFFFFFu)
    {
        return;
    }

    uint Slot;
    InterlockedAdd(SimArgs().NeighborCounts.At(Flat)[0], 1u, Slot);
    if (Slot < (uint)SimParams().NeighborParams.z)
    {
        SimArgs().NeighborIndices.Set(Flat * (uint)SimParams().NeighborParams.z + Slot, Index);
    }
}

struct FNeighborSums
{
    // Each close neighbor's direction away from it, stronger the closer it is.
    float3 Away;
    float3 Velocity;
    float3 Position;
    float  Count;
};

// Totals the live particles within Radius, which the 27 cells around this one hold in full only up to one cell size.
FNeighborSums GatherNeighbors(uint Index, float3 Position, float Radius)
{
    FNeighborSums Sums = (FNeighborSums)0;
    const float Reach = min(Radius, SimParams().NeighborParams.x);
    const int3  Home  = NeighborCellOf(Position);
    for (int Z = -1; Z <= 1; ++Z)
    {
        for (int Y = -1; Y <= 1; ++Y)
        {
            for (int X = -1; X <= 1; ++X)
            {
                const int3 Cell  = Home + int3(X, Y, Z);
                const uint Count = NeighborCount(Cell);
                for (uint Slot = 0u; Slot < Count; ++Slot)
                {
                    const uint Other = NeighborAt(Cell, Slot);
                    if (Other == Index)
                    {
                        continue;
                    }

                    const FGPUParticle Neighbor = Particles().Get(Other);
                    const float3 Offset   = Position - Neighbor.Position;
                    const float  Distance = length(Offset);
                    if (Distance < Reach && Distance > 1e-5)
                    {
                        Sums.Away     += Offset / Distance * (1.0 - Distance / Reach);
                        Sums.Velocity += Neighbor.Velocity;
                        Sums.Position += Neighbor.Position;
                        Sums.Count    += 1.0;
                    }
                }
            }
        }
    }
    return Sums;
}

// SpawnOrigin is the emitter, or the parent particle a sub-emitter spawns from.
void SpawnParticleGraph(inout FGPUParticle P, inout uint Seed, float TotalTime, uint Index, float3 SpawnOrigin)
{
$PARTICLE_SPAWN_FUNC
}

void UpdateParticleGraph(inout FGPUParticle P, float DeltaTime, float TotalTime, uint Index)
{
    float LifeRatio = saturate(P.Age / max(P.Lifetime, 0.0001));
$PARTICLE_UPDATE_FUNC
}

// The emitter's own rate and burst first, then script calls, then the parent's events, each claimed by one dead slot.
uint ClaimSpawn(out FParticleEvent Source)
{
    Source = (FParticleEvent)0;

    uint Claim = WaveInterlockedAdd(SpawnCounter().At(0u), 1u);
    if (Claim < SimParams().Counts.y)
    {
        return SPAWN_EMITTER;
    }

    const uint ScriptCount = SimArgs().ScriptEvents.Count;
    if (ScriptCount != 0u)
    {
        Claim = WaveInterlockedAdd(SpawnCounter().At(1u), 1u);
        if (Claim < ScriptCount)
        {
            Source = SimArgs().ScriptEvents.Get(Claim);
            return SPAWN_SCRIPT;
        }
    }

    const uint List = SimParams().EventInfo.y;
    if (List < PARTICLE_EVENT_LISTS && SimArgs().SourceEventCounts.Count > List)
    {
        const uint PerEvent = max(SimParams().EventInfo.w, 1u);
        const uint Events   = min(SimArgs().SourceEventCounts.Get(List), PARTICLE_EVENT_CAPACITY);
        Claim = WaveInterlockedAdd(SpawnCounter().At(2u), 1u);
        if (Claim < Events * PerEvent)
        {
            Source = SimArgs().SourceEvents.Get(List * PARTICLE_EVENT_CAPACITY + Claim / PerEvent);
            return SPAWN_EVENT;
        }
    }

    return SPAWN_NONE;
}

FGPUParticle SpawnParticle(uint Index, uint Origin, FParticleEvent Source)
{
    uint Seed = PCGHash(Index ^ SimParams().Counts.z);
    const float3 SpawnOrigin = (Origin == SPAWN_EMITTER) ? SimParams().EmitterPosition.xyz : Source.PositionSize.xyz;

    FGPUParticle P;
    P.Position      = SpawnOrigin;
    P.Velocity      = float3(0.0, 0.0, 0.0);
    P.Lifetime      = 1.0;
    P.Color         = float4(1.0, 1.0, 1.0, 1.0);
    P.Size          = 0.1;
    P.Age           = 0.0;
    P.Rotation      = 0.0;
    P.RotationSpeed = 0.0;
    P.Seed          = RandUnit(Seed);

    // Seeded before the stack, so a module reading an attribute nobody wrote gets its declared default.
$PARTICLE_ATTR_DEFAULTS
    SpawnParticleGraph(P, Seed, SimParams().Timing.y, Index, SpawnOrigin);

    P.Lifetime = max(P.Lifetime, 0.01);

    if (Origin == SPAWN_EVENT)
    {
        P.Velocity += Source.VelocityFlags.xyz * SimParams().EventParams.y;

        // Only a direction for SurfaceAligned facing to lie across, far too slow to carry the particle anywhere.
        if (SimParams().EventParams.z > 0.0 && dot(Source.Extra.xyz, Source.Extra.xyz) > 0.5)
        {
            P.Velocity = Source.Extra.xyz * PARTICLE_SURFACE_NORMAL_SPEED;
        }
    }
    else
    {
        const float3 EmitterVelocity = float3(SimParams().EmitterForward.w, SimParams().EmitterRight.w, SimParams().EmitterUp.w);
        P.Velocity += EmitterVelocity * SimParams().NoiseParams.y;
    }

    if (Origin == SPAWN_SCRIPT)
    {
        const uint Flags = asuint(Source.VelocityFlags.w);
        if ((Flags & PARTICLE_EMIT_POSITION) != 0u) { P.Position = Source.PositionSize.xyz; }
        if ((Flags & PARTICLE_EMIT_VELOCITY) != 0u) { P.Velocity = Source.VelocityFlags.xyz; }
        if ((Flags & PARTICLE_EMIT_COLOR)    != 0u) { P.Color    = Source.Extra; }
        if ((Flags & PARTICLE_EMIT_SIZE)     != 0u) { P.Size     = Source.PositionSize.w; }
    }

    InitializeRibbon(Index, P.Position);
    return P;
}

void KillParticle(inout FGPUParticle P)
{
    P.Lifetime = 0.0;
    P.Age      = 0.0;
    P.Color    = float4(0, 0, 0, 0);
    P.Size     = 0.0;
}

// Counts how many 1/Rate boundaries this step's aging crossed.
void RaiseContinuousEvents(FGPUParticle P, float DeltaTime)
{
    const float Rate = SimParams().EventParams.x;
    if ((SimParams().EventInfo.x & (1u << PARTICLE_EVENT_CONTINUOUS)) == 0u || Rate <= 0.0)
    {
        return;
    }

    const uint Crossed = (uint)(floor(P.Age * Rate) - floor(max(P.Age - DeltaTime, 0.0) * Rate));
    for (uint Event = 0u; Event < min(Crossed, 4u); ++Event)
    {
        RaiseParticleEvent(PARTICLE_EVENT_CONTINUOUS, P);
    }
}

void SimulateParticle(uint Index)
{
    FGPUParticle P = Particles().Get(Index);

    bool bAlive = (P.Lifetime > 0.0) && (P.Age < P.Lifetime);

    if (!bAlive)
    {
        FParticleEvent Source;
        const uint Origin = ClaimSpawn(Source);
        if (Origin != SPAWN_NONE)
        {
            P = SpawnParticle(Index, Origin, Source);
        }
        else
        {
            KillParticle(P);
        }
    }
    else
    {
        ApplyLocalSpace(Index, P);

        P.Age += SimParams().Timing.x;
        if (P.Age >= P.Lifetime)
        {
            RaiseParticleEvent(PARTICLE_EVENT_DEATH, P);
            KillParticle(P);
        }
        else
        {
            UpdateParticleGraph(P, SimParams().Timing.x, SimParams().Timing.y, Index);
            RaiseContinuousEvents(P, SimParams().Timing.x);
            RecordRibbon(Index, P.Position);
        }
    }

    Particles().Set(Index, P);
}

// Counts.x is the particles, grid cells or target texels this dispatch covers, and every lane of one dispatch takes the same branch.
[shader("compute")]
[numthreads(64, 1, 1)]
void ComputeMain(uint3 GroupId : SV_GroupID, uint Lane : SV_GroupIndex)
{
    uint Index = BeginParticleSimGroup(GroupId.x, Lane);
    if (Index >= SimParams().Counts.x) return;

    const uint Stage = SimParams().GridInfo.z;
    if (Stage == PARTICLE_STAGE_PARTICLES)
    {
        SimulateParticle(Index);
    }
    else if (Stage == PARTICLE_STAGE_TARGET)
    {
        WriteGridTarget(Index);
    }
    else if (Stage == PARTICLE_STAGE_NEIGHBORS)
    {
        InsertNeighbor(Index);
    }
    else
    {
        RunGridCell(Index);
    }
}
