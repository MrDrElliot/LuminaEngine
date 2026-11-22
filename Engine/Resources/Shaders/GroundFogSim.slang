#include "Includes/SceneGlobals.slang"
#include "Includes/GroundFog.slang"
#include "Includes/GlobalRHI.slang"

// A 2D incompressible flow over the ground fog around the camera. Bodies entering a column push fog out of it and bodies leaving draw it back in.

struct FPushConstants
{
    GPUSpan<FGroundFogSim> Params;
    uint         Stage;
    FTexture2D   FlowIn;
    FTexture2D   FillIn;
    FRWTexture2D FlowOut;
    FRWTexture2D FillOut;
    FTexture2D   PressureIn;
    FRWTexture2D PressureOut;
    FTexture2D   DivCurlIn;
    FRWTexture2D DivCurlOut;
    FTexture2D   Obstacles;
    uint         _Pad0;
    uint         _Pad1;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

static const uint kStageAdvect     = 0u;
static const uint kStageDivergence = 1u;
static const uint kStageJacobi     = 2u;
static const uint kStageProject    = 3u;

FGroundFogSim Sim() { return Pass().Params.Get(0u); }

bool InWindow(int2 Texel, uint Resolution)
{
    return all(Texel >= 0) && all(Texel < int(Resolution));
}

// Past the window the fog lies at rest, which is what an open boundary flows out into.
float4 LoadOr(FTexture2D Image, int2 Texel, uint Resolution, float4 Rest)
{
    return InWindow(Texel, Resolution) ? Image.Load(int3(Texel, 0)) : Rest;
}

float4 SampleOr(FTexture2D Image, float2 TexelCoord, uint Resolution, float4 Rest)
{
    const float2 UV = TexelCoord / float(Resolution);
    if (any(UV < 0.0) || any(UV > 1.0))
    {
        return Rest;
    }
    return Image.SampleLevel(Samplers::LinearClamp, UV, 0.0);
}

float ObstacleOccupancyAt(FGroundFogSim S, int2 Texel, int2 Size)
{
    const float Depth = Pass().Obstacles.Load(int3(clamp(Texel, int2(0, 0), Size - 1), 0)).r;
    if (Depth >= 1.0)
    {
        return 0.0;
    }
    const float BodyBottom = Depth * S.DepthToY.x + S.DepthToY.y;
    return saturate((S.BandTop - max(BodyBottom, S.BandBottom)) / max(S.BandTop - S.BandBottom, 1.0e-3));
}

// Fraction of the fog band a body fills over a column, filtered across the four nearest obstacle texels so a limb moving under a texel does not flicker.
float ObstacleOccupancy(FGroundFogSim S, float2 WorldXZ)
{
    if (!Pass().Obstacles.IsValid())
    {
        return 0.0;
    }

    uint Width;
    uint Height;
    Pass().Obstacles.GetDimensions(Width, Height);
    const int2   Size  = int2(Width, Height);
    const float2 UV    = float2(dot(S.ObstacleU.xy, WorldXZ) + S.ObstacleU.z, dot(S.ObstacleV.xy, WorldXZ) + S.ObstacleV.z);
    const float2 Coord = UV * float2(Size) - 0.5;
    const int2   Base  = int2(floor(Coord));
    const float2 Frac  = Coord - float2(Base);

    const float O00 = ObstacleOccupancyAt(S, Base, Size);
    const float O10 = ObstacleOccupancyAt(S, Base + int2(1, 0), Size);
    const float O01 = ObstacleOccupancyAt(S, Base + int2(0, 1), Size);
    const float O11 = ObstacleOccupancyAt(S, Base + int2(1, 1), Size);
    return lerp(lerp(O00, O10, Frac.x), lerp(O01, O11, Frac.x), Frac.y);
}

void Advect(FGroundFogSim S, int2 Texel)
{
    const float  H      = S.TexelWorld;
    const float2 World  = S.Origin + (float2(Texel) + 0.5) * H;
    const int2   Source = Texel + S.Shift;

    const float4 RestFlow = float4(0.0, 0.0, 0.0, 0.0);
    const float4 RestFill = float4(1.0, 0.0, 0.0, 0.0);

    // Back along the flow the fog drifts with, in the texel space of the state as it was written.
    const float4 Here     = LoadOr(Pass().FlowIn, Source, S.Resolution, RestFlow);
    const float2 Carry    = Here.xy + S.Wind;
    const float2 FromCoord = float2(Source) + 0.5 - Carry * (S.Dt / H);

    const float4 Flow = SampleOr(Pass().FlowIn, FromCoord, S.Resolution, RestFlow);
    float4       Fill = SampleOr(Pass().FillIn, FromCoord, S.Resolution, RestFill);

    float2 Velocity   = Flow.xy * S.VelocityKeep;
    float2 WispOffset = (Flow.zw + Flow.xy * S.Dt) * S.SwirlKeep;

    float Column = Fill.x + (1.0 - Fill.x) * S.RefillRate;

    // Occupancy belongs to the ground, not to the fog drifting over it, so it is read in place rather than advected.
    const float LastOccupancy = LoadOr(Pass().FillIn, Source, S.Resolution, RestFill).y;
    float Occupancy = LastOccupancy;
    float Displaced = 0.0;
    if (S.bIngest != 0u)
    {
        Occupancy = ObstacleOccupancy(S, World);
        Displaced = (Occupancy - LastOccupancy) / max(S.Dt, 1.0e-4) * S.Displacement;
    }
    Column = min(Column, 1.0 - Occupancy);

    const float Speed = length(Velocity);
    if (Speed > S.MaxSpeed)
    {
        Velocity *= S.MaxSpeed / Speed;
    }

    Pass().FlowOut[Texel] = float4(Velocity, WispOffset);
    Pass().FillOut[Texel] = float4(saturate(Column), Occupancy, Displaced, 0.0);
}

// The divergence the projection must leave is the rate bodies push fog out, so the right-hand side subtracts it.
void Divergence(FGroundFogSim S, int2 Texel)
{
    const float4 Rest = float4(0.0, 0.0, 0.0, 0.0);
    const float2 L = LoadOr(Pass().FlowIn, Texel + int2(-1, 0), S.Resolution, Rest).xy;
    const float2 R = LoadOr(Pass().FlowIn, Texel + int2(1, 0), S.Resolution, Rest).xy;
    const float2 B = LoadOr(Pass().FlowIn, Texel + int2(0, -1), S.Resolution, Rest).xy;
    const float2 T = LoadOr(Pass().FlowIn, Texel + int2(0, 1), S.Resolution, Rest).xy;

    const float InvTwoH   = 0.5 / S.TexelWorld;
    const float Div       = ((R.x - L.x) + (T.y - B.y)) * InvTwoH;
    const float Curl      = ((R.y - L.y) - (T.x - B.x)) * InvTwoH;
    const float Displaced = Pass().FillIn.Load(int3(Texel, 0)).z;

    Pass().DivCurlOut[Texel] = float4(Div - Displaced, Curl, 0.0, 0.0);
}

void Jacobi(FGroundFogSim S, int2 Texel)
{
    const float4 Rest = float4(0.0, 0.0, 0.0, 0.0);
    const float Sum = LoadOr(Pass().PressureIn, Texel + int2(-1, 0), S.Resolution, Rest).x
                    + LoadOr(Pass().PressureIn, Texel + int2(1, 0), S.Resolution, Rest).x
                    + LoadOr(Pass().PressureIn, Texel + int2(0, -1), S.Resolution, Rest).x
                    + LoadOr(Pass().PressureIn, Texel + int2(0, 1), S.Resolution, Rest).x;
    const float Rhs = Pass().DivCurlIn.Load(int3(Texel, 0)).x;
    Pass().PressureOut[Texel] = float4((Sum - S.TexelWorld * S.TexelWorld * Rhs) * 0.25, 0.0, 0.0, 0.0);
}

void Project(FGroundFogSim S, int2 Texel)
{
    const float4 Rest = float4(0.0, 0.0, 0.0, 0.0);
    const float InvTwoH = 0.5 / S.TexelWorld;

    const float PL = LoadOr(Pass().PressureIn, Texel + int2(-1, 0), S.Resolution, Rest).x;
    const float PR = LoadOr(Pass().PressureIn, Texel + int2(1, 0), S.Resolution, Rest).x;
    const float PB = LoadOr(Pass().PressureIn, Texel + int2(0, -1), S.Resolution, Rest).x;
    const float PT = LoadOr(Pass().PressureIn, Texel + int2(0, 1), S.Resolution, Rest).x;

    float4 Flow = Pass().FlowOut[Texel];
    float2 Velocity = Flow.xy - float2(PR - PL, PT - PB) * InvTwoH;

    // Vorticity confinement feeds back the curl the coarse grid smears away, which is what rolls a wake into eddies.
    if (S.Vorticity > 0.0)
    {
        const float  Curl = Pass().DivCurlIn.Load(int3(Texel, 0)).y;
        const float  CL = abs(LoadOr(Pass().DivCurlIn, Texel + int2(-1, 0), S.Resolution, Rest).y);
        const float  CR = abs(LoadOr(Pass().DivCurlIn, Texel + int2(1, 0), S.Resolution, Rest).y);
        const float  CB = abs(LoadOr(Pass().DivCurlIn, Texel + int2(0, -1), S.Resolution, Rest).y);
        const float  CT = abs(LoadOr(Pass().DivCurlIn, Texel + int2(0, 1), S.Resolution, Rest).y);
        const float2 Gradient = float2(CR - CL, CT - CB) * InvTwoH;
        const float  Length   = length(Gradient);
        if (Length > 1.0e-5)
        {
            const float2 N = Gradient / Length;
            Velocity += float2(N.y, -N.x) * Curl * S.Vorticity * S.TexelWorld * S.Dt;
        }
    }

    const float Speed = length(Velocity);
    if (Speed > S.MaxSpeed)
    {
        Velocity *= S.MaxSpeed / Speed;
    }
    Pass().FlowOut[Texel] = float4(Velocity, Flow.zw);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const FGroundFogSim S = Sim();
    const int2 Texel = int2(DTid.xy);
    if (!InWindow(Texel, S.Resolution))
    {
        return;
    }

    switch (Pass().Stage)
    {
    case kStageAdvect:     Advect(S, Texel);     break;
    case kStageDivergence: Divergence(S, Texel); break;
    case kStageJacobi:     Jacobi(S, Texel);     break;
    default:               Project(S, Texel);    break;
    }
}
